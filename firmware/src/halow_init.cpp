// Implementation of halow_init() — see halow_init.h for why this exists.
//
// PATH C variant: uses Morse Micro's mm-iot-esp32 2.8.2 morselib directly,
// no Heltec wrapper.  The only difference vs the Heltec variant is that we
// drop the call to halowtcpipInit() (which lived in libheltec_halow.a) —
// mmipal_init() now handles all lwIP netif binding directly.
//
// Lives in its own TU so the regulatory-db tables (~10 KB of pre-baked
// channel definitions for AU/CA/EU/IN/JP/KR/NZ/US) are only included once.

#ifndef USE_STANDARD_WIFI

#include "halow_init.h"

#include <Arduino.h>   // Serial — for PS-disable rc logging
#include <cstring>
#include "config.h"
#include "mmwlan.h"
#include "mmwlan_regdb.h"   // pulls in get_regulatory_db() + all country tables
#include "mmhal.h"
#include "mmhal_wlan.h"     // exposes mmhal_wlan_hard_reset() — pulses HT-HC01 reset GPIO
#include "mmipal.h"         // IP adaptation layer — wires lwIP to HaLow netif
#include "lwip/netif.h"     // Path C: for sizeof(struct netif) layout diagnostic

extern "C" bool halow_init(const char *region) {
    // Path C: no halowtcpipInit() — mmipal_init handles netif binding directly.
    // Path A (Heltec) had: if (!halowtcpipInit()) return false; before mmhal_init.
    mmhal_init();

    // Belt-and-suspenders hardware reset.  The actual wedge fix lives at the
    // top of setup() (gpio_reset_pin(6)+(7) then RESET_N pulse) — by the time
    // we get here mmhal_init has already brought the chip up.  See
    // memory: hc33-mm6108-wedge-cascade.
    mmhal_wlan_hard_reset();

    mmwlan_init();

    // Make AMPDU / RTS settings explicit instead of relying on undocumented
    // mmwlan defaults.  See halow-wedge-rts-mismatch and halow-wedge-not-ampdu
    // memories — RTS=1000 matches HD01; AMPDU stays ON (disabling made wedge worse).
    {
        enum mmwlan_status a = mmwlan_set_ampdu_enabled(true);
        enum mmwlan_status r = mmwlan_set_rts_threshold(1000);
        Serial.printf("[halow_init] explicit ampdu=on rc=%d rts=1000 rc=%d (match HD01)\n",
                      (int)a, (int)r);
        Serial.flush();
    }

    // Disable 802.11 power-save. See Heltec original for full rationale —
    // default is MMWLAN_PS_ENABLED which causes wildly variable RTTs.
    {
        enum mmwlan_status ps_st = mmwlan_set_power_save_mode(MMWLAN_PS_DISABLED);
        Serial.printf("[halow_init] pre-enable mmwlan_set_power_save_mode(DISABLED) rc=%d\n", (int)ps_st);
        Serial.flush();
    }

    const struct mmwlan_s1g_channel_list *cl =
        mmwlan_lookup_regulatory_domain(get_regulatory_db(), region);
    if (cl == nullptr) {
        return false;
    }
    if (mmwlan_set_channel_list(cl) != MMWLAN_SUCCESS) {
        return false;
    }

    // mmipal_init boots the chip (loads firmware) AND binds the lwIP netif to
    // the wlan link layer.  Has to run BEFORE mmwlan_sta_enable, otherwise the
    // chip never finishes booting and sta_enable spins in CONNECTING forever.
    // In Path C, mmipal_init also does the work that Heltec's halowtcpipInit
    // used to do (calling tcpip_init / registering the netif).
    struct mmipal_init_args ipal_args = MMIPAL_INIT_ARGS_DEFAULT;
    if (mmipal_init(&ipal_args) != MMIPAL_SUCCESS) {
        return false;
    }

    // Chip is booted — print version so we can confirm the swap stuck.
    // Expecting: morse_fw=1.15.3  morselib=2.8.2-esp32  (NOT -dbg)
    {
        struct mmwlan_version ver = {};
        if (mmwlan_get_version(&ver) == MMWLAN_SUCCESS) {
            Serial.printf("[halow_init] morse_fw=%s morselib=%s chip_id=0x%lx\n",
                          ver.morse_fw_version, ver.morselib_version,
                          (unsigned long)ver.morse_chip_id);
            Serial.flush();
        }
    }

    // Path C diagnostic: print struct netif layout as seen by THIS compile.
    // If any of these numbers differ from what Heltec's prebuilt liblwip.a
    // expects, the linked netif accesses misalign and we crash on link-up.
    // Compare across runs / against a Heltec-built reference.
    Serial.printf("[halow_init] sizeof(struct netif)=%u  offsets: linkoutput=%u "
                  "state=%u flags=%u hwaddr=%u name=%u\n",
                  (unsigned)sizeof(struct netif),
                  (unsigned)offsetof(struct netif, linkoutput),
                  (unsigned)offsetof(struct netif, state),
                  (unsigned)offsetof(struct netif, flags),
                  (unsigned)offsetof(struct netif, hwaddr),
                  (unsigned)offsetof(struct netif, name));
    Serial.flush();
    return true;
}

// SOFT retry helper — just disables STA mode without tearing down the wlan
// stack or mmipal netif binding.
extern "C" void halow_sta_disable_for_retry(void) {
    mmwlan_sta_disable();
}

#if HALOW_USE_STATIC_IP
extern "C" bool halow_apply_static_ip(void) {
    struct mmipal_ip_config cfg = MMIPAL_IP_CONFIG_DEFAULT;
    cfg.mode = MMIPAL_STATIC;
    std::strncpy(cfg.ip_addr,      HALOW_STATIC_IP,      sizeof(cfg.ip_addr) - 1);
    std::strncpy(cfg.netmask,      HALOW_STATIC_NETMASK, sizeof(cfg.netmask) - 1);
    std::strncpy(cfg.gateway_addr, HALOW_STATIC_GATEWAY, sizeof(cfg.gateway_addr) - 1);
    return mmipal_set_ip_config(&cfg) == MMIPAL_SUCCESS;
}
#endif

extern "C" bool halow_get_ip(char *ip_buf, size_t ip_buf_len) {
    struct mmipal_ip_config cfg = MMIPAL_IP_CONFIG_DEFAULT;
    if (mmipal_get_ip_config(&cfg) != MMIPAL_SUCCESS) {
        return false;
    }
    if (cfg.ip_addr[0] == '\0' || std::strcmp(cfg.ip_addr, "0.0.0.0") == 0) {
        return false;
    }
    std::strncpy(ip_buf, cfg.ip_addr, ip_buf_len - 1);
    ip_buf[ip_buf_len - 1] = '\0';
    return true;
}

#endif  // !USE_STANDARD_WIFI
