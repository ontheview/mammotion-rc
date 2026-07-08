// HaLow init helper.
//
// Heltec's HalowSTAClass::begin() ships with the prerequisite mmwlan init
// commented out (mmhal_init, mmwlan_init, mmwlan_set_channel_list).  Without
// these, mmwlan_sta_enable() asserts at runtime with "Channel list not set".
//
// halow_init() does what their library forgot.  Call it BEFORE HaLow.begin().

#pragma once

#ifndef USE_STANDARD_WIFI
#include <stddef.h>
#include "config.h"

extern "C" bool halow_init(const char *region);

// Soft-disable STA mode between association attempts — preserves the mmipal
// netif binding (unlike mmwlan_shutdown which strands it).  Call between
// failed mmwlan_sta_enable attempts.
extern "C" void halow_sta_disable_for_retry(void);

// Read the current IPv4 address from the IP-adaptation layer.  We can't use
// HaLow.localIP() because Heltec never wires up its WL_CONNECTED status flag
// (see halow_init.cpp for the gory detail).  Returns false until DHCP/STATIC
// produces a non-zero address.
extern "C" bool halow_get_ip(char *ip_buf, size_t ip_buf_len);

#if HALOW_USE_STATIC_IP
// Override the IP config to static.  Call AFTER HaLow.begin() returns —
// applying static via mmipal_init() before sta_enable trips an assert in
// Heltec's library.
extern "C" bool halow_apply_static_ip(void);
#endif
#endif
