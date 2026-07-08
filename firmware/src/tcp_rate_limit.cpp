#include "tcp_rate_limit.h"

#include <Arduino.h>

#include <lwip/pbuf.h>
#include <lwip/netif.h>
#include <lwip/prot/ip.h>
#include <lwip/prot/ip4.h>
#include <lwip/ip_addr.h>
#include <lwip/ip4_addr.h>

// State lives only on the lwIP TCPIP thread (the only context that invokes
// LWIP_HOOK_IP4_INPUT), so no locking is required.  Stats reads from other
// threads are intentionally unsynchronised — racy reads of counters are fine
// for diagnostics.

// Byte-rate config: caps inbound throughput in bytes/sec.  Set high enough
// that it doesn't fire on normal load — the byte cap exists mainly as a
// belt-and-braces guard against pathological streams.  The PPS cap below
// is the one that defends against the actual stall mechanism.
static uint32_t s_rate_bps = 250000;   // 2 Mbit/s sustained per direction
static uint32_t s_burst    = 65536;    // 64 KB per direction

// Packet-rate config: caps inbound packets/sec.  This is the cap that
// actually matters for the softAP->HaLow stall: the MM6108 wedges its
// internal TX queue when fed bursts of small packets at >~150-300 pps
// (multi-flow TLS handshakes, QUIC, web page loads).  A modest cap
// here keeps every flow honest without harming iperf-style throughput
// (1500-byte packets at 250 KB/s = 170 pps, which still fits).  Set
// to 0 to disable the pps gate entirely.
static uint32_t s_pps       = 100;     // packets/sec sustained per direction
static uint32_t s_pps_burst = 50;      // burst depth in packets

// One bucket per direction.  Without this, big inbound DATA drains the
// bucket and outbound ACKs (returning to the sender) get dropped — sender
// doesn't see ACKs, retransmits, snowballs.  Per-direction buckets keep
// ACK paths independent from the heavy data path going the other way.
//
// Two-dimensional token bucket: byte_tokens + pps_tokens, each with its own
// last_ms accumulator so neither dimension loses fractional refill time at
// high call rates (refill of <1 token-per-ms must not get rounded to 0
// repeatedly — that would freeze the bucket).
struct rl_bucket {
    int32_t  byte_tokens;
    uint32_t byte_last_ms;
    int32_t  pps_tokens;
    uint32_t pps_last_ms;
};
static struct rl_bucket s_bkt_in  = { 65536, 0, 50, 0 };   // HaLow-RX direction
static struct rl_bucket s_bkt_out = { 65536, 0, 50, 0 };   // softAP-RX direction

static struct netif *s_halow_netif  = nullptr;
static struct netif *s_softap_netif = nullptr;
static hc33_tcp_limit_stats_t s_stats = {};

extern "C" void hc33_tcp_limit_set_halow_netif(struct netif *halow_netif) {
    s_halow_netif = halow_netif;
}

extern "C" void hc33_tcp_limit_set_softap_netif(struct netif *softap_netif) {
    s_softap_netif = softap_netif;
}

extern "C" void hc33_tcp_limit_configure(uint32_t rate_bytes_per_sec,
                                         uint32_t burst_bytes) {
    s_rate_bps = rate_bytes_per_sec;
    s_burst    = burst_bytes;
    if (s_bkt_in.byte_tokens  > (int32_t)burst_bytes) s_bkt_in.byte_tokens  = (int32_t)burst_bytes;
    if (s_bkt_out.byte_tokens > (int32_t)burst_bytes) s_bkt_out.byte_tokens = (int32_t)burst_bytes;
}

extern "C" void hc33_tcp_limit_configure_pps(uint32_t pps_rate,
                                             uint32_t pps_burst) {
    s_pps       = pps_rate;
    s_pps_burst = pps_burst;
    if (s_bkt_in.pps_tokens  > (int32_t)pps_burst) s_bkt_in.pps_tokens  = (int32_t)pps_burst;
    if (s_bkt_out.pps_tokens > (int32_t)pps_burst) s_bkt_out.pps_tokens = (int32_t)pps_burst;
}

extern "C" void hc33_tcp_limit_get_stats(hc33_tcp_limit_stats_t *out) {
    *out = s_stats;
    // Report the smaller of the two buckets so the operator sees pressure
    // from whichever direction is hot.
    int32_t bt = (s_bkt_in.byte_tokens < s_bkt_out.byte_tokens) ? s_bkt_in.byte_tokens : s_bkt_out.byte_tokens;
    int32_t pt = (s_bkt_in.pps_tokens  < s_bkt_out.pps_tokens)  ? s_bkt_in.pps_tokens  : s_bkt_out.pps_tokens;
    out->tokens_now     = (bt < 0) ? 0 : (uint32_t)bt;
    out->pps_tokens_now = (pt < 0) ? 0 : (uint32_t)pt;
    out->rate_bps       = s_rate_bps;
    out->burst          = s_burst;
    out->pps            = s_pps;
    out->pps_burst      = s_pps_burst;
}

// Refill the byte bucket.  Advances byte_last_ms only by the time actually
// converted to integer tokens, so fractional-ms accruals aren't lost when
// the hook is called at >1 kHz.
static inline void refill_bytes(struct rl_bucket *b, uint32_t now_ms) {
    if (b->byte_last_ms == 0) { b->byte_last_ms = now_ms; return; }
    uint32_t dt = now_ms - b->byte_last_ms;
    if (dt == 0) return;
    uint64_t add = ((uint64_t)dt * (uint64_t)s_rate_bps) / 1000ULL;
    if (add == 0) return;
    int64_t nb = (int64_t)b->byte_tokens + (int64_t)add;
    if (nb > (int64_t)s_burst) nb = (int64_t)s_burst;
    b->byte_tokens = (int32_t)nb;
    // Advance last_ms by exactly the time we consumed (`add` bytes / rate).
    uint32_t consumed_ms = (s_rate_bps > 0)
        ? (uint32_t)((add * 1000ULL) / s_rate_bps)
        : dt;
    b->byte_last_ms += consumed_ms;
}

// Refill the packet bucket.  Same fractional-time pattern as bytes.
static inline void refill_pps(struct rl_bucket *b, uint32_t now_ms) {
    if (b->pps_last_ms == 0) { b->pps_last_ms = now_ms; return; }
    uint32_t dt = now_ms - b->pps_last_ms;
    if (dt == 0) return;
    uint64_t add = ((uint64_t)dt * (uint64_t)s_pps) / 1000ULL;
    if (add == 0) return;
    int64_t np = (int64_t)b->pps_tokens + (int64_t)add;
    if (np > (int64_t)s_pps_burst) np = (int64_t)s_pps_burst;
    b->pps_tokens = (int32_t)np;
    uint32_t consumed_ms = (s_pps > 0)
        ? (uint32_t)((add * 1000ULL) / s_pps)
        : dt;
    b->pps_last_ms += consumed_ms;
}

extern "C" int hc33_lwip_ip4_input_hook(struct pbuf *p, struct netif *inp) {
    if (p == nullptr) return 0;
    if (p->len < (u16_t)sizeof(struct ip_hdr)) return 0;

    // Pick the bucket and proto policy for this direction.
    //   HaLow-RX  -> s_bkt_in  caps both TCP and UDP (bulk downstream traffic
    //                          like QUIC/HTTP3 webpage loads, video streaming,
    //                          can bufferbloat the softAP TX queue and even
    //                          crash the device via heap exhaustion).
    //   softAP-RX -> s_bkt_out caps TCP only.  Outbound UDP is left
    //                          untouched so the mower's Agora video upstream
    //                          runs at full HaLow capacity.
    // Packets on un-registered netifs pass through.
    struct rl_bucket *bkt;
    bool limit_udp;
    if (s_halow_netif != nullptr && inp == s_halow_netif) {
        bkt = &s_bkt_in;
        limit_udp = true;
    } else if (s_softap_netif != nullptr && inp == s_softap_netif) {
        bkt = &s_bkt_out;
        limit_udp = false;
    } else {
        return 0;
    }

    struct ip_hdr *iphdr = (struct ip_hdr *)p->payload;
    if (IPH_V(iphdr) != 4) return 0;

    uint8_t proto = IPH_PROTO(iphdr);
    if (proto == IP_PROTO_UDP) {
        s_stats.udp_seen_pkts++;
        if (!limit_udp) return 0;
        // fall through to bucket logic with UDP packet
    } else if (proto != IP_PROTO_TCP) {
        return 0;
    }

    uint32_t now = millis();
    refill_bytes(bkt, now);
    refill_pps(bkt, now);

    uint16_t plen = p->tot_len;
    bool bytes_ok = (bkt->byte_tokens >= (int32_t)plen);
    bool pps_ok   = (s_pps == 0) || (bkt->pps_tokens >= 1);
    if (bytes_ok && pps_ok) {
        bkt->byte_tokens -= (int32_t)plen;
        if (s_pps != 0) bkt->pps_tokens -= 1;
        s_stats.tcp_passed_pkts++;
        s_stats.tcp_passed_bytes += plen;
        return 0;
    }

    // Bucket dry — drop.  Returning 1 means "packet consumed"; lwIP will not
    // free it for us, so we free here.  TCP sender sees the loss and backs
    // off via standard congestion control.
    s_stats.tcp_dropped_pkts++;
    s_stats.tcp_dropped_bytes += plen;
    if (!bytes_ok) s_stats.drop_bytes_full_pkts++;
    if (!pps_ok)   s_stats.drop_pps_full_pkts++;
    pbuf_free(p);
    return 1;
}
