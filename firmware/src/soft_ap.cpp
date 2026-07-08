#ifndef USE_STANDARD_WIFI

#include "soft_ap.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_netif_net_stack.h>
#include <esp_wifi.h>
#include <lwip/netif.h>
#include <lwip/ip_addr.h>
#include <lwip/dns.h>          // dns_getserver() — read the HaLow DHCP-learned DNS
#include <dhcpserver/dhcpserver.h>
// Path C: mmipal_data is static inside mmipal_lwip.c (was non-static in
// Heltec); we use mmipal_get_lwip_netif() accessor instead of the global.
#include "mmipal.h"

#include "config.h"
#include "config_load.h"     // g_cfg — softap_ssid/softap_pass come from here now
#include "halow_tx_nonblock.h"
#include "tcp_rate_limit.h"

// Our patched lwIP stores the napt-enabled bit in netif->flags using this
// value (see esp-idf clone's netif.h patch).  Heltec's bundled netif.h header
// doesn't have this define since their lwIP didn't have IP_NAPT — declare it
// locally so the diagnostic compiles against either header set.
#ifndef NETIF_FLAG_NAPT
#define NETIF_FLAG_NAPT         0x80U
#endif

static const char* TAG = "soft_ap";

// Tracks whether WiFi.softAP() is currently up.  Set true at the end of
// soft_ap_begin() on success; cleared by soft_ap_stop().  Used to make both
// idempotent and to expose soft_ap_is_up() for the HaLow link tracker in main.
static bool s_softap_up = false;

// Cut through ESP_LOG's tag/level filtering with raw Serial.printf + flush.
// Used for soft_ap status lines because the WiFi-driver bring-up reliably eats
// ESP_LOG output on Heltec's framework — observed: AP comes up, mower can
// associate, but every ESP_LOGI from this TU vanishes on the serial monitor.
#define SAP_PRINTF(fmt, ...) do { \
    Serial.printf("[soft_ap] " fmt "\n", ##__VA_ARGS__); \
    Serial.flush(); \
} while (0)

bool soft_ap_is_up() {
    return s_softap_up;
}

// Surface softAP station + DHCP-lease events via raw Serial.  The WiFi driver's
// own ESP_LOG join/leave lines are eaten on Heltec's framework (same reason
// SAP_PRINTF exists), so a join attempt looks like total silence — which is
// exactly what made "the mower won't join" undiagnosable.  The DISCONNECT
// *reason* code is the decisive split:
//   reason=15 (4WAY_HANDSHAKE_TIMEOUT) / 2 (AUTH_EXPIRE) / 23 (802.1X) → the
//       WPA2/PMF handshake is failing (auth problem, fix at the AP).
//   JOINED + GOT IP, then the mower LEAVES on its own a few seconds later →
//       it associated and got a lease but rejected the link for lack of usable
//       upstream internet (look at the HaLow uplink / NAPT at that instant).
static void soft_ap_wifi_event(arduino_event_id_t event, arduino_event_info_t info) {
    switch (event) {
    case ARDUINO_EVENT_WIFI_AP_STACONNECTED: {
        const uint8_t* m = info.wifi_ap_staconnected.mac;
        SAP_PRINTF("STA JOINED %02x:%02x:%02x:%02x:%02x:%02x aid=%u",
                   m[0], m[1], m[2], m[3], m[4], m[5],
                   (unsigned)info.wifi_ap_staconnected.aid);
        break;
    }
    case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED: {
        const uint8_t* m = info.wifi_ap_stadisconnected.mac;
        SAP_PRINTF("STA LEFT   %02x:%02x:%02x:%02x:%02x:%02x reason=%u",
                   m[0], m[1], m[2], m[3], m[4], m[5],
                   (unsigned)info.wifi_ap_stadisconnected.reason);
        break;
    }
    case ARDUINO_EVENT_WIFI_AP_STAIPASSIGNED: {
        uint32_t a = info.wifi_ap_staipassigned.ip.addr;
        SAP_PRINTF("STA GOT IP %u.%u.%u.%u (dhcps lease handed out)",
                   (unsigned)(a & 0xff), (unsigned)((a >> 8) & 0xff),
                   (unsigned)((a >> 16) & 0xff), (unsigned)((a >> 24) & 0xff));
        break;
    }
    default:
        break;
    }
}

void soft_ap_stop() {
    if (!s_softap_up) return;
    SAP_PRINTF("soft_ap_stop() — tearing down AP");
    // softAPdisconnect(true) stops dhcps, deletes the AP netif, AND turns the
    // WiFi mode off.  Next soft_ap_begin() rebuilds the whole stack from
    // scratch, which is fine — the entire bring-up is well under 1s.
    WiFi.softAPdisconnect(true);
    s_softap_up = false;
}

bool soft_ap_begin() {
    if (s_softap_up) {
        SAP_PRINTF("soft_ap_begin() — already up, skipping");
        return true;
    }
    SAP_PRINTF("soft_ap_begin() entered");

    // WiFi 2.4 GHz driver uses ESP-IDF defaults (static_tx_buf_num=16,
    // dynamic_tx_buf_num=32, cache_tx_buf_num=32, ampdu_tx_enable=1).  We
    // do not shrink TX buffers here: the actual softAP+NAPT stall mechanism
    // was inside Heltec's mmnetif_tx (synchronous mmwlan_tx_wait_until_ready
    // parking the lwIP TCPIP thread up to 1 s) and is fixed by the
    // non-blocking linkoutput replacement below.  See memory:
    // halow-tx-blocking-root-cause.
    //
    // Path C: rate limiter disabled.  The wedge that required the 30 pps cap
    // was caused by the -dbg morselib in libheltec_halow.a; release morselib
    // 2.8.2-esp32 (no -dbg) does not exhibit the hard 15 s freeze, so the
    // workaround is no longer needed.  Files tcp_rate_limit.{h,cpp} kept on
    // disk so we can re-arm without rebuilding all the plumbing.

    hc33_halow_tx_nonblock_install(mmipal_get_lwip_netif());
    SAP_PRINTF("HaLow TX linkoutput replaced with non-blocking (timeout 50ms)");

    // Note: in the HaLow env, net_connect() doesn't touch the S3's built-in
    // WiFi at all (HaLow is a separate radio via the HT-HC01 module).  So we
    // start the WiFi driver fresh here, in pure AP mode — no APSTA, no STA.
    size_t heap_before_mode = esp_get_free_internal_heap_size();
    if (!WiFi.mode(WIFI_AP)) {
        SAP_PRINTF("WiFi.mode(WIFI_AP) failed");
        return false;
    }
    size_t heap_after_mode = esp_get_free_internal_heap_size();
    SAP_PRINTF("heap delta after WiFi.mode: %d bytes (free now %d)",
               (int)(heap_before_mode - heap_after_mode), (int)heap_after_mode);

    // Register the AP station/lease event logger exactly once.  Must be in
    // place before WiFi.softAP() so we catch the very first association.
    static bool s_event_registered = false;
    if (!s_event_registered) {
        WiFi.onEvent(soft_ap_wifi_event, ARDUINO_EVENT_WIFI_AP_STACONNECTED);
        WiFi.onEvent(soft_ap_wifi_event, ARDUINO_EVENT_WIFI_AP_STADISCONNECTED);
        WiFi.onEvent(soft_ap_wifi_event, ARDUINO_EVENT_WIFI_AP_STAIPASSIGNED);
        s_event_registered = true;
        SAP_PRINTF("AP station event logger registered");
    }

    // Configure the AP netif so dhcps starts EXACTLY ONCE, already correct.
    //
    // Root cause of the "mower gets 192.168.4.1 (the AP's own IP) as DNS" bug:
    // the ESP-IDF dhcpserver offers its configured dns_server ONLY if the
    // OFFER_DNS bit is set; otherwise it offers its own server_address
    // (dhcpserver.c:442-450).  And esp_netif_dhcps_option(SET, OFFER_DNS)
    // returns ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED if dhcps is already running
    // (esp_netif_lwip.c, OP_SET path).  The old code set OFFER_DNS WITHOUT first
    // stopping dhcps and never checked the return — so whenever dhcps happened
    // to already be started, OFFER_DNS silently never applied and dhcps fell
    // back to offering the AP IP.  (It is NOT a set_ip_info clobber:
    // set_ip_info only clears lwIP's *global resolver* DNS, never the dhcps
    // offer, and dhcps_start preserves both dns_server and the OFFER_DNS bit.)
    //
    // Fix: explicitly stop dhcps, THEN set OFFER_DNS + dns_server while stopped,
    // THEN one dhcps_start.  WiFi.softAP() only sets SSID/auth and never touches
    // dhcps (WiFiAP.cpp:136-176), so nothing resets the option after us.  We
    // avoid WiFi.softAPConfig() (which would stop/start dhcps a second time) and
    // we do NOT stop/start AFTER softAP() under HaLow traffic — that burst is
    // what overflows the tcpip mailbox (mmnetif_link_state "sched callback
    // failed").  See memory: softap-dhcp-dns-push, mmnetif-link-state-assert.
    esp_netif_t* ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (ap_netif == nullptr) {
        SAP_PRINTF("esp_netif_get_handle_from_ifkey(WIFI_AP_DEF) returned null");
        return false;
    }

    // Guarantee dhcps is STOPPED: this is what makes the OFFER_DNS set below
    // actually take (it errors with ALREADY_STARTED otherwise).  No client is
    // connected yet, so this isn't the restart-under-traffic that asserts.
    esp_netif_dhcps_stop(ap_netif);

    // Set the AP IP (replaces WiFi.softAPConfig so dhcps isn't started twice).
    esp_netif_ip_info_t ipinfo = {};
    ipinfo.ip.addr      = ipaddr_addr(AP_IP);
    ipinfo.gw.addr      = ipaddr_addr(AP_GATEWAY);
    ipinfo.netmask.addr = ipaddr_addr(AP_NETMASK);
    if (esp_netif_set_ip_info(ap_netif, &ipinfo) != ESP_OK) {
        SAP_PRINTF("esp_netif_set_ip_info failed");
        return false;
    }

    // Choose the DNS to advertise.  This must work for BOTH HaLow modes:
    //   - HALOW_USE_STATIC_IP=0 (DHCP): hand the mower whatever the *uplink
    //     lease* learned — NOT a hardcoded gateway, which may be wrong for the
    //     actual subnet/resolver.
    //   - HALOW_USE_STATIC_IP=1 (static): the uplink learns no DNS, so
    //     dns_getserver(0) is empty and we fall back to AP_DNS_SERVER (the
    //     configured static gateway) — i.e. the previous behaviour, unchanged.
    // lwIP's DHCP client stores the learned DNS in the global table; we read it
    // via dns_getserver(0) rather than mmipal_get_dns_server() because that wrapper
    // isn't exported by the precompiled HaLow archive (header-drift trap) while
    // dns_getserver() always links.  Timing is safe: soft_ap_begin() runs after
    // net_connect()'s IP-poll, so the lease is BOUND and the DNS option from the
    // same DHCP ACK is already applied.  Fall back to AP_DNS_SERVER if the
    // upstream offered no DNS (dns_getserver returns ip_addr_any).
    uint32_t dns_a = ipaddr_addr(AP_DNS_SERVER);   // compiled fallback
    const ip_addr_t* up_dns = dns_getserver(0);
    bool dns_from_lease = (up_dns != nullptr && !ip_addr_isany(up_dns));
    if (dns_from_lease) {
        dns_a = ip4_addr_get_u32(ip_2_ip4(up_dns));
    }

    dhcps_offer_t offer_dns = OFFER_DNS;
    esp_netif_dhcps_option(ap_netif, ESP_NETIF_OP_SET,
                           ESP_NETIF_DOMAIN_NAME_SERVER,
                           &offer_dns, sizeof(offer_dns));
    esp_netif_dns_info_t dns_info = {};
    dns_info.ip.u_addr.ip4.addr = dns_a;
    dns_info.ip.type = ESP_IPADDR_TYPE_V4;
    esp_netif_set_dns_info(ap_netif, ESP_NETIF_DNS_MAIN, &dns_info);
    SAP_PRINTF("dhcps pre-configured: IP=%s DNS=%u.%u.%u.%u (%s) — single start, no softAPConfig",
               AP_IP,
               (unsigned)(dns_a & 0xff), (unsigned)((dns_a >> 8) & 0xff),
               (unsigned)((dns_a >> 16) & 0xff), (unsigned)((dns_a >> 24) & 0xff),
               dns_from_lease ? "from HaLow DHCP lease" : "compiled fallback");

    // 3) Bring up the radio.  softAP() only sets SSID/auth and starts wifi — it
    //    does not touch IP/DNS — so the config above survives.
    size_t heap_pre_softap = esp_get_free_internal_heap_size();
    if (!WiFi.softAP(g_cfg.softap_ssid, g_cfg.softap_pass, g_cfg.softap_channel, /*hidden=*/0, AP_MAX_CONN)) {
        SAP_PRINTF("softAP failed");
        return false;
    }
    size_t heap_after_softap = esp_get_free_internal_heap_size();
    SAP_PRINTF("heap delta after WiFi.softAP (where esp_wifi_start allocs TX buffers): %d bytes (free now %d)",
               (int)(heap_pre_softap - heap_after_softap), (int)heap_after_softap);

    // Force the AP to unambiguous legacy WPA2-PSK with PMF OFF.  arduino-esp32
    // brings the AP up PMF-*capable* by default; Mammotion mowers are WPA2-only
    // and some of their wifi stacks fail the 4-way handshake against a
    // PMF-capable AP (shows up as repeated join attempts that never complete /
    // STADISCONNECTED reason=15).  Setting capable=false makes us a pure
    // non-PMF WPA2 AP — maximum compatibility with old WPA2-only clients.
    wifi_config_t apcfg = {};
    if (esp_wifi_get_config(WIFI_IF_AP, &apcfg) == ESP_OK) {
        apcfg.ap.authmode         = WIFI_AUTH_WPA2_PSK;
        apcfg.ap.pmf_cfg.capable  = false;
        apcfg.ap.pmf_cfg.required = false;
        esp_err_t cerr = esp_wifi_set_config(WIFI_IF_AP, &apcfg);
        SAP_PRINTF("AP auth forced WPA2-PSK, PMF off (rc=%s)", esp_err_to_name(cerr));
    }

    // 4) Ensure dhcps is running with our config.  If esp_netif already started
    //    it on the AP-start event, this returns ALREADY_STARTED — harmless.
    esp_err_t dhcps_err = esp_netif_dhcps_start(ap_netif);
    if (dhcps_err != ESP_OK && dhcps_err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) {
        SAP_PRINTF("esp_netif_dhcps_start failed: %s (0x%x)",
                   esp_err_to_name(dhcps_err), dhcps_err);
        return false;
    }

    // Path C: aggressive 10 s client eviction removed.  It was a wedge
    // workaround (kick the laptop to free per-STA driver state and unblock
    // lwIP) that became actively harmful — under sustained TCP, the laptop
    // got booted mid-transfer and the re-association window appeared as
    // 30 s stalls in the speedtest log.  ESP-IDF default (~6 min) restored.

    SAP_PRINTF("softAP up: SSID=%s channel=%u IP=%s",
               g_cfg.softap_ssid, (unsigned)g_cfg.softap_channel,
               WiFi.softAPIP().toString().c_str());

    // Enable NAPT (source-NAT) on the AP netif so the mower's packets get
    // their src addr rewritten from 192.168.4.X to the HaLow STA address as
    // they leave the HC33.  lwIP picks the egress netif by routing table —
    // the HaLow netif's default route is what makes this work.
    //
    // esp_netif_napt_enable() requires the IDF lwIP to have been built with
    // CONFIG_LWIP_IPV4_NAPT=y AND CONFIG_LWIP_IP_FORWARD=y.  ESP-IDF 5.x
    // defaults have both on, but Heltec's framework ships a custom sdkconfig
    // — if NAPT comes back NOT_SUPPORTED here, we'll need to either patch
    // their sdkconfig or implement a userspace forwarder.
    if (ap_netif == nullptr) {
        SAP_PRINTF("esp_netif_get_handle_from_ifkey(WIFI_AP_DEF) returned null");
        return false;
    }
    esp_err_t err = esp_netif_napt_enable(ap_netif);
    if (err != ESP_OK) {
        SAP_PRINTF("esp_netif_napt_enable failed: %s (0x%x)", esp_err_to_name(err), err);
        return false;
    }
    SAP_PRINTF("NAPT enabled on AP netif — mower traffic will egress via HaLow");

    // Force HaLow to be lwIP's default route.  Without this, packets that
    // NAPT decides to forward have no egress netif and get dropped — the
    // AP netif keeps the WiFi default by virtue of being the most recently
    // brought-up interface.  mmipal_init doesn't call netif_set_default()
    // (we read its source), so we do it here.
    struct netif* halow_netif = mmipal_get_lwip_netif();
    netif_set_default(halow_netif);
    SAP_PRINTF("default route set to HaLow netif (ip=%d.%d.%d.%d)",
               ip4_addr1(&halow_netif->ip_addr.u_addr.ip4),
               ip4_addr2(&halow_netif->ip_addr.u_addr.ip4),
               ip4_addr3(&halow_netif->ip_addr.u_addr.ip4),
               ip4_addr4(&halow_netif->ip_addr.u_addr.ip4));

    s_softap_up = true;
    return true;
}

#endif  // !USE_STANDARD_WIFI
