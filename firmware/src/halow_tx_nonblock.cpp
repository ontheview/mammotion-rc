#include "halow_tx_nonblock.h"

// HaLow-only: depends on mmwlan/mmpkt headers that don't exist in the
// standard-wifi build.  main.cpp already guards the #include "halow_tx_nonblock.h"
// behind the same flag; this guard makes the TU itself empty under
// USE_STANDARD_WIFI so PIO's src-glob can keep compiling it.
#ifndef USE_STANDARD_WIFI

#include <Arduino.h>
#include <lwip/netif.h>
#include <lwip/pbuf.h>
#include <lwip/err.h>

#include "mmwlan.h"
#include "mmpkt.h"

// State.  All access is from the lwIP TCPIP thread (linkoutput is invoked
// only from there) plus the unsynchronised diagnostic reader.  Racy reads
// of the counters are fine for diagnostics.
static uint32_t s_attempts     = 0;
static uint32_t s_ok           = 0;
static uint32_t s_busy_drops   = 0;
static uint32_t s_alloc_drops  = 0;
static uint32_t s_send_drops   = 0;
static uint32_t s_timeout_ms   = 50;

// Replacement linkoutput.  Mirrors the structure of MorseMicro's
// mmnetif_tx but with a bounded wait_until_ready timeout — converts
// "TCPIP thread parks up to 1 s" into "drop and let TCP recover".
static err_t hc33_halow_linkoutput(struct netif *netif, struct pbuf *p) {
    s_attempts++;

    if (mmwlan_tx_wait_until_ready(s_timeout_ms) != MMWLAN_SUCCESS) {
        s_busy_drops++;
        return ERR_BUF;
    }

    struct mmpkt *pkt = mmwlan_alloc_mmpkt_for_tx(p->tot_len, 0);
    if (pkt == nullptr) {
        s_alloc_drops++;
        return ERR_MEM;
    }

    struct mmpktview *pktview = mmpkt_open(pkt);
    for (struct pbuf *walk = p; walk != nullptr; walk = walk->next) {
        mmpkt_append_data(pktview, (const uint8_t *)walk->payload, walk->len);
    }
    mmpkt_close(&pktview);

    // Heltec's SDK predates multi-VIF support; metadata struct has only
    // `tid`.  Passing NULL is documented as "use defaults" (TID 0 = best-
    // effort, matches what mmnetif uses for non-QoS-tagged egress).
    if (mmwlan_tx_pkt(pkt, nullptr) != MMWLAN_SUCCESS) {
        s_send_drops++;
        return ERR_BUF;
    }

    s_ok++;
    return ERR_OK;
}

extern "C" void hc33_halow_tx_nonblock_install(struct netif *halow_netif) {
    if (halow_netif == nullptr) return;
    halow_netif->linkoutput = hc33_halow_linkoutput;
}

extern "C" void hc33_halow_tx_nonblock_set_timeout(uint32_t timeout_ms) {
    s_timeout_ms = timeout_ms;
}

extern "C" void hc33_halow_tx_nonblock_get_stats(hc33_halow_tx_nonblock_stats_t *out) {
    if (out == nullptr) return;
    out->attempts    = s_attempts;
    out->ok          = s_ok;
    out->busy_drops  = s_busy_drops;
    out->alloc_drops = s_alloc_drops;
    out->send_drops  = s_send_drops;
    out->timeout_ms  = s_timeout_ms;
}

#endif  // !USE_STANDARD_WIFI
