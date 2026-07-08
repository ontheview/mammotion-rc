#pragma once

// Non-blocking replacement for MorseMicro's mmnetif_tx linkoutput.
//
// The MorseMicro SDK (and Heltec's downstream library) wires the HaLow
// netif->linkoutput to a synchronous function that calls
// `mmwlan_tx_wait_until_ready(1000)` before every TX.  That blocks the
// lwIP TCPIP thread for up to a full second when the chip's TX pool
// (default 20 mmpkts) is full — which is the root cause of the 700-900 ms
// hb_age stalls we observe under softAP+NAPT+burst load.  See memory:
// hc33-stall-not-cpu-bound.
//
// This module installs a replacement linkoutput that uses a configurable
// short timeout (default 50 ms).  When the chip is busy beyond that:
// drop the packet, return ERR_BUF, let TCP retransmit at normal cadence.
// TCPIP thread never parks for more than the timeout.

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

struct netif;

// Replace halow_netif->linkoutput with our non-blocking version.  Call
// once after HaLow.begin() has brought the netif up.  Safe to call
// regardless of whether the netif has associated.
void hc33_halow_tx_nonblock_install(struct netif *halow_netif);

// Adjust the chip-ready wait timeout at runtime.  Default 50 ms.  Lower
// values reduce worst-case TCPIP-thread block at the cost of more drops;
// 0 means "drop immediately if not instantly ready".
void hc33_halow_tx_nonblock_set_timeout(uint32_t timeout_ms);

typedef struct {
    uint32_t attempts;       // total TX calls into linkoutput
    uint32_t ok;             // successfully handed to chip
    uint32_t busy_drops;     // dropped because chip not ready within timeout
    uint32_t alloc_drops;    // dropped because mmpkt allocation failed
    uint32_t send_drops;     // dropped because mmwlan_tx_pkt returned error
    uint32_t timeout_ms;     // current wait timeout
} hc33_halow_tx_nonblock_stats_t;

void hc33_halow_tx_nonblock_get_stats(hc33_halow_tx_nonblock_stats_t *out);

#ifdef __cplusplus
}
#endif
