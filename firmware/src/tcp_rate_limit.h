#pragma once

// TCP-only rate limiter in the lwIP IP4 input/forward path.
//
// Purpose: prevent the softAP TX queue from bufferbloating under inbound NAPT
// load, AND prevent the MM6108 HaLow chip's TX path from wedging under
// burst small-packet traffic (the multi-flow webpage-load failure mode —
// see memory hc33-stall-not-cpu-bound).  Two-dimensional token bucket:
//   - byte cap (rate_bytes_per_sec, burst_bytes) — guards against pure
//     throughput pathologies; rarely fires on normal traffic.
//   - packet cap (pps, pps_burst)                — guards against bursts
//     of small packets that don't move many bytes but overwhelm the
//     MM6108 SPI/TX queue depth.  This is the cap that actually matters
//     for the softAP+NAPT+HaLow stall.
//
// A packet is forwarded only if BOTH buckets have enough tokens.  If
// either is dry, the packet is dropped, the TCP sender's congestion
// control will back off, and UDP senders will see loss.
//
// UDP policy:
//   - HaLow-RX direction: UDP is rate-limited along with TCP (defends
//     against QUIC/HTTP3 webpage downloads, video streaming).
//   - softAP-RX direction: UDP is NOT rate-limited so the mower's
//     Agora video upstream runs at full HaLow capacity.
//
// See memory: hc33-softap-napt-stalls, hc33-stall-not-cpu-bound.

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>

struct pbuf;
struct netif;

// Wired up to LWIP_HOOK_IP4_INPUT via lwipopts.h.  Return 1 to consume the
// packet (we free it), 0 to let lwIP continue normal input/forward.
int hc33_lwip_ip4_input_hook(struct pbuf *p, struct netif *inp);

// Configure the byte-rate dimension.  Safe to call before or after softAP
// is up.
// rate_bytes_per_sec — sustained forwarded-TCP cap (default 250000 = 2 Mbit/s)
// burst_bytes        — bucket depth, max instantaneous burst (default 65536)
void hc33_tcp_limit_configure(uint32_t rate_bytes_per_sec, uint32_t burst_bytes);

// Configure the packet-rate dimension.  Pass pps_rate=0 to disable the
// packet gate entirely (byte gate still applies).
// pps_rate  — sustained packets/sec per direction (default 100)
// pps_burst — burst depth in packets (default 50)
void hc33_tcp_limit_configure_pps(uint32_t pps_rate, uint32_t pps_burst);

// Bind the limiter to the HaLow + softAP netifs.  The hook rate-limits TCP
// that arrives on EITHER of these — both directions of forwarded TCP can
// bufferbloat the respective egress queue (HaLow-RX/softAP-TX direction
// bloats the softAP TX queue; softAP-RX/HaLow-TX direction bloats the
// HaLow chip TX path).  Packets on netifs that aren't registered here pass
// through unfiltered (so HC33-local sockets that don't go via these two
// netifs are untouched).  Until the netifs are set, the limiter is a
// pass-through.
void hc33_tcp_limit_set_halow_netif(struct netif *halow_netif);
void hc33_tcp_limit_set_softap_netif(struct netif *softap_netif);

typedef struct {
    uint32_t tcp_passed_pkts;
    uint32_t tcp_passed_bytes;
    uint32_t tcp_dropped_pkts;
    uint32_t tcp_dropped_bytes;
    uint32_t drop_bytes_full_pkts;   // packets dropped because byte bucket empty
    uint32_t drop_pps_full_pkts;     // packets dropped because pps bucket empty
    uint32_t udp_seen_pkts;
    uint32_t tokens_now;             // byte tokens (min of two buckets)
    uint32_t pps_tokens_now;         // pps  tokens (min of two buckets)
    uint32_t rate_bps;
    uint32_t burst;
    uint32_t pps;
    uint32_t pps_burst;
} hc33_tcp_limit_stats_t;

void hc33_tcp_limit_get_stats(hc33_tcp_limit_stats_t *out);

#ifdef __cplusplus
}
#endif
