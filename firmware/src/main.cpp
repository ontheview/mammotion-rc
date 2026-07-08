// HC33 TCP-to-BLE byte-pipe firmware (Stage 4 of the mower-remote-control plan).
//
// Single role: accept ONE TCP client at TCP_PORT, lazy-connect BLE to the
// Mammotion mower, and pipe length-prefixed binary frames both ways.  The
// HC33 has no protocol knowledge — PyMammotion's HC33ProxyTransport runs the
// BluFi codec on the PC side; each frame on the wire is exactly one BLE
// characteristic operation.
//
// Two build envs (see platformio.ini):
//   env:hc33                  - HaLow uplink (Heltec ESP_HaLow lib overrides WiFi.h)
//   env:hc33-standard-wifi    - built-in 2.4 GHz WiFi STA, sets USE_STANDARD_WIFI

#include <Arduino.h>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <esp_task_wdt.h>
#include <driver/gpio.h>   // gpio_reset_pin — used in setup() MM6108 wedge-clear
#include <WiFi.h>
#include <esp_wifi.h>
#include "log.h"
#include "config.h"
#include "config_load.h"
#include "net_compat.h"
#include "ble_central.h"
#include "tcp_proxy.h"
#include "mdns_advert.h"
#include "discovery.h"
#ifndef USE_STANDARD_WIFI
#include "soft_ap.h"
#include "tcp_rate_limit.h"
#include "halow_tx_nonblock.h"
#endif
#include "cpu_stats.h"

static const char* TAG = "main";

// Set to 1 to skip soft_ap_begin() entirely.  Diagnostic flag for the case
// where the 2.4 GHz softAP appears to desensitize the MM6108 HaLow RX front
// end (asymmetric link signature: HD01 hears HC33 at ~-1 dBm but HC33 hears
// HD01 at ~-72 dBm at touching distance).  With this set: HaLow stays up,
// BLE stays up, TCP proxy still listens — but no softAP / NAT / default
// route.  HaLow-side LAN access to the HC33 (e.g. 192.168.1.31:9876) still
// works because lwIP replies on the inbound netif without needing a default.
#ifndef HC33_SKIP_SOFTAP
#define HC33_SKIP_SOFTAP 0
#endif

// Set to 1 to skip g_ble.begin() and never start NimBLE.  Diagnostic flag
// for isolating whether BLE+WiFi 2.4 GHz coex on the ESP32-S3 is creating
// periodic blackout windows that block the HaLow SPI driver.  Symptom this
// is meant to test: ~14-17 s stalls in HaLow TX during sustained softAP
// traffic (web page load), persists across CPU pinning and bigger pbuf
// pools.  If stalls disappear with this set, coex is the cause.
#ifndef HC33_SKIP_BLE
#define HC33_SKIP_BLE 0
#endif

static BleCentral g_ble;
static TcpProxy   g_tcp(g_ble);

#ifdef USE_STANDARD_WIFI
static void net_connect() {
    WiFi.mode(WIFI_STA);
    WiFi.begin(g_cfg.uplink_ssid, g_cfg.uplink_pass);
    ESP_LOGI(TAG, "WiFi connecting to %s ...", g_cfg.uplink_ssid);
    uint32_t t = millis();
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        if (millis() - t > 30000) {
            ESP_LOGE(TAG, "WiFi connect timeout — rebooting");
            ESP.restart();
        }
    }
    ESP_LOGI(TAG, "WiFi connected, IP: %s", WiFi.localIP().toString().c_str());
}
#else
#include "halow_init.h"
#include "mmwlan.h"
#include "mmipal.h"
#include <cstring>
#include <lwip/sockets.h>
#include <arpa/inet.h>
#include <lwip/tcpip.h>
#include <lwip/timeouts.h>

// lwIP TCPIP-thread heartbeat. Bootstrapped via tcpip_callback() so the first
// arming runs on the TCPIP thread; the callback then re-arms itself via
// sys_timeout(). If the TCPIP thread is ever blocked (e.g. inside a long
// netif->linkoutput call on the WiFi softAP path), g_lwip_hb_ms stops
// updating and (millis() - g_lwip_hb_ms) grows. The stat dump prints this
// age so we can tell whether stalls are lwIP-thread-blocked or elsewhere.
static volatile uint32_t g_lwip_hb_ms = 0;
static void lwip_heartbeat_cb(void* /*arg*/) {
    g_lwip_hb_ms = millis();
    sys_timeout(200, lwip_heartbeat_cb, nullptr);
}
static void lwip_heartbeat_bootstrap(void* /*arg*/) {
    g_lwip_hb_ms = millis();
    sys_timeout(200, lwip_heartbeat_cb, nullptr);
}

// Status callback for mmwlan_sta_enable.  We poll mmwlan_get_sta_state()
// directly during the initial associate loop, but ALSO log async state
// transitions so we can see any disconnect/reassoc events after Ready (useful
// for diagnosing HaLow link drops under load).
static void halow_sta_status_cb(enum mmwlan_sta_state s) {
    const char *name = "?";
    switch (s) {
        case MMWLAN_STA_DISABLED:    name = "DISABLED"; break;
        case MMWLAN_STA_CONNECTING:  name = "CONNECTING"; break;
        case MMWLAN_STA_CONNECTED:   name = "CONNECTED"; break;
    }
    Serial.printf("[halow] sta_state=%s (%d) @ %lu ms\n", name, (int)s, millis());
    Serial.flush();
}

// Try mmwlan_sta_enable up to 8 times, with a hardware reset of the HT-HC01
// between failures.  First cold attempt commonly gets stuck in CONNECTING;
// the radio needs a hard reset (not just software shutdown) to recover.
// On success, returns true with the wlan stably associated.  mmipal_init
// hasn't been called yet at this point — caller must invoke
// halow_finalize_ip_layer() before touching the IP layer.
static bool halow_associate_with_retry() {
    struct mmwlan_sta_args sta_args = MMWLAN_STA_ARGS_INIT;
    std::strncpy((char*)sta_args.ssid, g_cfg.uplink_ssid, sizeof(sta_args.ssid) - 1);
    sta_args.ssid_len = std::strlen((const char*)sta_args.ssid);
    std::strncpy(sta_args.passphrase, g_cfg.uplink_pass, sizeof(sta_args.passphrase) - 1);
    sta_args.passphrase_len = std::strlen(sta_args.passphrase);
    sta_args.security_type = WIFI_SECURITY_HALOW;

    // Default in MMWLAN_STA_ARGS_INIT is MMWLAN_PMF_REQUIRED — which is
    // nonsensical against the open MowerAP we're joining (no MFP -> no PMF
    // negotiated -> per-spec the STA must refuse).  Some chip firmware
    // versions paper over this with a quiet downgrade; others reject.
    // Either way, "REQUIRED" against an OPEN AP is the wrong knob.  For our
    // OPEN test network, make it explicit: PMF_DISABLED.  Flip to REQUIRED
    // when we re-enable SAE on the AP.
    if (sta_args.security_type == MMWLAN_OPEN) {
        sta_args.pmf_mode = MMWLAN_PMF_DISABLED;
    }

    for (int attempt = 1; attempt <= 8; attempt++) {
        ESP_LOGI(TAG, "HaLow associate attempt %d", attempt);
        enum mmwlan_status st = mmwlan_sta_enable(&sta_args, halow_sta_status_cb);
        if (st != MMWLAN_SUCCESS) {
            ESP_LOGW(TAG, "mmwlan_sta_enable returned %d — sta_disable + retry", (int)st);
            halow_sta_disable_for_retry();
            delay(500);
            continue;
        }

        uint32_t t = millis();
        while (mmwlan_get_sta_state() != MMWLAN_STA_CONNECTED) {
            if (millis() - t > 15000) {
                ESP_LOGW(TAG, "attempt %d: stuck in state %d — sta_disable + retry",
                         attempt, (int)mmwlan_get_sta_state());
                halow_sta_disable_for_retry();
                delay(500);
                break;
            }
            delay(200);
        }
        if (mmwlan_get_sta_state() == MMWLAN_STA_CONNECTED) {
            return true;
        }
    }
    return false;
}

static void net_connect() {
    ESP_LOGI(TAG, "HaLow init for region %s ...", g_cfg.region);
    // Loop instead of restart so the user has time to send AT+CDKEY without
    // the firmware kicking out from under them.  When halowtcpipInit()
    // (called by halow_init) is waiting for a license, the library prints
    // its own prompt in a loop — we just need to not blow it away.
    while (!halow_init(g_cfg.region)) {
        ESP_LOGW(TAG, "HaLow init returned false — retrying in 5s "
                      "(license required? AT+CDKEY=... over serial)");
        delay(5000);
    }
    ESP_LOGI(TAG, "HaLow connecting to %s ...", g_cfg.uplink_ssid);

    // Bypass HaLow.begin() — it MMOSAL_ASSERTs on sta_enable failure.  Use a
    // retry-with-hard-reset loop (radio HW reset between attempts) to unstick
    // the HT-HC01 when it wedges in CONNECTING on cold boot.
    if (!halow_associate_with_retry()) {
        ESP_LOGE(TAG, "HaLow never associated after 8 attempts — rebooting");
        delay(500);
        ESP.restart();
    }
    ESP_LOGI(TAG, "HaLow associated (mmwlan=CONNECTED)");

    // One-shot link sanity print.  Confirms WHICH AP we joined (BSSID) and
    // starting RSSI.  This pairs with the periodic [halow] line (which shows
    // negotiated MCS/BW via rc_stats) to verify the link came up the way we
    // asked it to — pre-load, before any traffic skews the picture.
    // (mmwlan_get_aid is declared in the header but NOT exported by Heltec's
    // libmorse.a, so we skip it.)
    {
        uint8_t bssid[6] = {0};
        int32_t rssi0 = mmwlan_get_rssi();
        enum mmwlan_status bs = mmwlan_get_bssid(bssid);
        Serial.printf("[main] link: bssid=%02X:%02X:%02X:%02X:%02X:%02X (rc=%d) "
                      "rssi0=%ddBm pmf=%s\n",
                      bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5],
                      (int)bs, (int)rssi0,
                      "DISABLED");
        Serial.flush();
    }

    // Re-apply PS=DISABLED after association.  halow_init() sets this before
    // sta_enable per the API contract, but the symptom of multi-second TX
    // stalls (chip's sent counter stops incrementing for 10-15s windows,
    // ping timeouts cluster, AP buffers overflow) suggests the setting may
    // not survive association.  Belt-and-braces re-disable here.
    {
        enum mmwlan_status ps_st = mmwlan_set_power_save_mode(MMWLAN_PS_DISABLED);
        Serial.printf("[main] post-associate mmwlan_set_power_save_mode(DISABLED) rc=%d\n", (int)ps_st);
        Serial.flush();
    }

#if HALOW_USE_STATIC_IP
    // HD01 firmware doesn't reliably bridge DHCP from the HaLow side to LAN,
    // so once associated we force a static IP via mmipal_set_ip_config().
    // Doing this AFTER begin() avoids the assert that fires when mmipal_init
    // is given MMIPAL_STATIC up front.
    if (!halow_apply_static_ip()) {
        ESP_LOGE(TAG, "halow_apply_static_ip failed — rebooting");
        ESP.restart();
    }
    ESP_LOGI(TAG, "Applied static IP %s", HALOW_STATIC_IP);
#endif

    // Poll the IP-adaptation layer until an IPv4 address is bound to the
    // netif.  Can't use HaLow.localIP() — it's gated on a WL_CONNECTED flag
    // that nothing in Heltec's lib ever sets, so it always returns 0.0.0.0.
    char ip_str[48] = {0};
    uint32_t t = millis();
    while (!halow_get_ip(ip_str, sizeof(ip_str))) {
        if (millis() - t > 30000) {
            ESP_LOGW(TAG, "IP assignment timeout — rebooting");
            ESP.restart();
        }
        delay(500);
    }
    ESP_LOGI(TAG, "HaLow connected, IP: %s", ip_str);

    // Diagnostic: prove HC33 can initiate outbound TCP to the LAN router.  If
    // this fails, HC33 → LAN is broken and NAPT can never work regardless of
    // setup.  If it succeeds, HC33 → LAN is fine and any NAT failure is
    // forwarding-specific.
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s >= 0) {
        struct sockaddr_in dst{};
        dst.sin_family = AF_INET;
        dst.sin_port = htons(80);
        dst.sin_addr.s_addr = inet_addr("192.168.1.1");
        struct timeval tv{2, 0};
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        int rc = connect(s, (struct sockaddr*)&dst, sizeof(dst));
        ESP_LOGI(TAG, "HC33->192.168.1.1:80 connect rc=%d errno=%d (%s)",
                 rc, errno, rc == 0 ? "REACHED" : "FAILED");
        close(s);
    }
}
#endif

void setup() {
    Serial.begin(115200);
    delay(200);   // let CP210x settle so the first log line isn't lost

#ifndef USE_STANDARD_WIFI
    // MM6108 wedge-cascade fix.  See memory: hc33-mm6108-wedge-cascade.
    //
    // After a soft reset (rst:0xc) the MM6108 keeps SPI_IRQ (GPIO 6) asserted
    // low, AND the S3's GPIO peripheral config for that pin survives the
    // reset: intr_type=GPIO_INTR_LOW_LEVEL, enable bit set.  The moment
    // mmhal_init() calls gpio_install_isr_service(0), the dispatcher reconnects
    // and the still-asserted level-triggered IRQ fires continuously with no
    // handler registered → Interrupt WDT on Core 1 → another soft reset → loop.
    //
    // Two things have to happen and the order matters:
    //   1) Clear the zombie IRQ config on SPI_IRQ (GPIO 6) and BUSY (GPIO 7)
    //      so any latched/asserted level can't fire when gpio_install_isr_service
    //      brings the dispatcher up inside mmhal_init().
    //   2) Hardware-reset the MM6108 (pulse RESET_N on GPIO 9) so the chip
    //      itself stops asserting SPI_IRQ.
    // Pulsing RESET alone (what we tried first) wasn't enough — the GPIO IRQ
    // config survives independently of whatever the chip is doing.
    Serial.printf("[main] MM6108 wedge-clear: reset GPIO 6 (SPI_IRQ) + 7 (BUSY) + pulse GPIO 9 (RESET_N)\n");
    Serial.flush();
    gpio_reset_pin((gpio_num_t)6);   // CONFIG_MM_SPI_IRQ — LOW_LEVEL triggered, the killer
    gpio_reset_pin((gpio_num_t)7);   // CONFIG_MM_BUSY — POSEDGE triggered
    pinMode(9, OUTPUT);
    digitalWrite(9, LOW);
    delay(10);
    digitalWrite(9, HIGH);
    delay(30);
    Serial.printf("[main] MM6108 wedge-clear done\n");
    Serial.flush();
#endif

    // Load runtime config from the `config` partition (or fall back to the
    // compiled defaults in config.h if it's blank/foreign/corrupt).  Must run
    // before net_connect() / soft_ap_begin() since they read g_cfg.  See
    // config_load.h for the rationale and the dev workflow.
    config_load();

    // Per-core idle hooks must be registered BEFORE WiFi/lwIP/NimBLE start so
    // the 200 ms baseline is captured at a relatively quiet moment.
    cpu_stats_init();

    // Task watchdog on the Arduino loopTask.  Detects hard wedges (blocked
    // BLE writes after the mower goes unresponsive, NimBLE state corruption,
    // HaLow driver hangs) and reboots so HaLow re-associates automatically —
    // otherwise the only recovery is a physical power cycle.  ICMP keeps
    // working when the loop is wedged (handled in lwIP's tcpip thread) so
    // external pings can't detect the failure.
    //
    // 30 s timeout chosen so planned slow ops in loop() don't trip:
    //   - BLE scan: up to 15 s (SCAN_TIMEOUT_MS)
    //   - BLE connect: up to 10 s (BLE_CONNECT_TIMEOUT_MS)
    //   - Single blocked BLE write: usually <5 s
    //
    // CRITICAL: we register loopTask with TWDT at the END of setup(), NOT here.
    // net_connect() below can take >30 s when HaLow associate retries (up to
    // 8 × 15 s) — if loopTask were watched during setup, that path panics
    // before loop() ever runs.  The watchdog is for steady-state recovery,
    // not setup wedges.
    //
    // TWDT is already initialized at boot by ESP-IDF/Arduino with a 5 s
    // default; widen now so the timer is correct before we arm it later.
    //
    // The TWDT API was rewritten between IDF 4.4 (espressif32@6.10.0, used
    // by env:hc33-standard-wifi) and IDF 5.x (pioarduino 51.x, used by
    // env:hc33).  Pick the right one based on the IDF major version.
#if ESP_IDF_VERSION_MAJOR >= 5
    esp_task_wdt_config_t wdt_cfg = {
        .timeout_ms      = 30000,
        .idle_core_mask  = 0,        // don't watchdog idle tasks
        .trigger_panic   = true,     // panic + reboot on timeout (vs. just log)
    };
    if (esp_task_wdt_reconfigure(&wdt_cfg) != ESP_OK) {
        // TWDT wasn't initialized yet — initialize now with our config.
        esp_task_wdt_init(&wdt_cfg);
    }
#else
    // IDF 4.4: older API takes (seconds, panic) and has no reconfigure.
    // Arduino-ESP32 v2 already inits TWDT at 5 s — deinit first so our
    // 30 s value can take effect (init() rejects a duplicate init).
    esp_task_wdt_deinit();
    esp_task_wdt_init(30, true);
#endif

    net_connect();

#ifndef USE_STANDARD_WIFI
#if HC33_SKIP_SOFTAP
    // Diagnostic path: init WiFi driver enough that esp_wifi-dependent
    // subsystems (BLE coex, esp_netif registrations) find an initialized
    // interface and don't crash, but do NOT call softAP() — no beacons, no
    // associations.  Then esp_wifi_stop() to power down the 2.4 GHz radio
    // so any RX desense it was causing on the MM6108 goes away.
    //
    // If the firmware crashes right after this block, the next thing to try
    // is removing the esp_wifi_stop() call — WiFi.mode(WIFI_AP) alone leaves
    // the radio "started but idle" (no beacons broadcast), which still tests
    // the desense hypothesis even if it's slightly less clean than a fully
    // powered-down radio.
    Serial.println("[main] HC33_SKIP_SOFTAP=1 — init WiFi, skip AP broadcast, stop radio");
    Serial.flush();
    WiFi.mode(WIFI_AP);
    delay(200);
    esp_err_t stop_err = esp_wifi_stop();
    Serial.printf("[main] esp_wifi_stop() rc=%d (0=ok)\n", (int)stop_err);
    Serial.flush();
#else
    // In Mode 2, bring up the softAP for the mower right after HaLow is up.
    // The mower can begin associating while BLE/TCP init below continues.
    //
    // delay() before+after so the WiFi driver's heavy radio-init noise doesn't
    // gobble our log lines — observed on Heltec's framework where UART output
    // during WiFi.mode(WIFI_AP) goes missing without a breathing pause.
    delay(200);
    if (!soft_ap_begin()) {
        ESP_LOGE(TAG, "soft_ap_begin failed — continuing without mower AP");
    }
    delay(200);
#endif
#endif

#if HC33_SKIP_BLE
    ESP_LOGW(TAG, "HC33_SKIP_BLE=1 — NimBLE not started (coex diagnostic mode)");
#else
    g_ble.begin();
#endif
    g_tcp.begin();

    // Publish on mDNS so the web-server onboarding can discover this proxy
    // by browsing _hc33proxy._tcp.local.  bonded_name starts empty and gets
    // pushed once the BLE scan in connect_mower() picks a candidate; the
    // refresh call in loop() propagates that into the TXT record.
    //
    // mDNS works on the standard-wifi build but NOT on HaLow (can't enable
    // lwIP's responder without breaking Heltec netif ABI).  The UDP broadcast
    // responder below is the cross-variant mechanism the server actually uses;
    // mDNS stays as a wifi-side debug convenience.
    mdns_advert_begin();

    // UDP broadcast discovery — works on both variants (pure BSD sockets).
    // The bonded-name provider is polled per reply, so answers always reflect
    // the latest BLE scan.  g_ble is a file-global, no capture needed.
    discovery_begin(TCP_PORT, [] { return g_ble.get_bonded_name(); });

#ifndef USE_STANDARD_WIFI
    // Kick off the lwIP TCPIP-thread heartbeat (see definition above).
    // tcpip_callback() posts the bootstrap to run on the tcpip thread, which
    // then arms a recurring sys_timeout. Stats dump shows the age of the
    // last heartbeat — if it grows past ~300 ms during a "stall", the tcpip
    // thread is provably blocked inside a single call.
    g_lwip_hb_ms = millis();
    tcpip_callback(lwip_heartbeat_bootstrap, nullptr);
#endif
    ESP_LOGI(TAG, "Ready — TCP proxy on port %d, mower auto-discovered by service %s",
             TCP_PORT, UUID_SERVICE);

    // Arm the task watchdog NOW that all slow init is done.  loop() resets the
    // WDT every iteration; if it ever fails to (BLE write blocked, NimBLE
    // wedge, HaLow driver hang), the 30 s timer panics and reboots so HaLow
    // re-associates automatically.  See wdt_cfg setup at the top of setup().
    esp_task_wdt_add(NULL);

    // One-shot task table dump ~3s after Ready, by which time WiFi+lwIP+NimBLE
    // tasks have settled and we can see the steady-state core pinning of
    // every task in the system.  Heavy (~4 KB of serial output), so guarded
    // by a flag in loop() rather than printed every second.
}

static bool s_task_dump_done = false;
static uint32_t s_task_dump_at_ms = 0;

#ifndef USE_STANDARD_WIFI
// Dump HaLow link diagnostics every 1 second so we can time-correlate the
// firmware-side view of the link against external traffic tests (ping -t,
// iperf, mower load).  Was 5s — too coarse for diagnosing sub-10s outages.
static void halow_link_stats_dump() {
    static uint32_t last_ms = 0;
    if (millis() - last_ms < 1000) return;
    last_ms = millis();

    int32_t rssi = mmwlan_get_rssi();
    enum mmwlan_sta_state sta = mmwlan_get_sta_state();
    const char *sta_str = (sta == MMWLAN_STA_DISABLED)   ? "DISABLED"
                        : (sta == MMWLAN_STA_CONNECTING) ? "CONNECTING"
                        : (sta == MMWLAN_STA_CONNECTED)  ? "CONN"
                        : "?";

    struct mmwlan_rc_stats *st = mmwlan_get_rc_stats();
    uint32_t best_idx = 0;
    uint32_t best_sent = 0;
    if (st != nullptr) {
        for (uint32_t i = 0; i < st->n_entries; i++) {
            if (st->total_sent[i] > best_sent) {
                best_sent = st->total_sent[i];
                best_idx = i;
            }
        }
    }

    size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t min_internal  = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);

    if (st != nullptr && best_sent > 0) {
        uint32_t info = st->rate_info[best_idx];
        uint32_t bw   = (info >> MMWLAN_RC_STATS_RATE_INFO_BW_OFFSET)    & 0xF;
        uint32_t mcs  = (info >> MMWLAN_RC_STATS_RATE_INFO_RATE_OFFSET)  & 0xF;
        uint32_t gi   = (info >> MMWLAN_RC_STATS_RATE_INFO_GUARD_OFFSET) & 0x1;
        // BW encoding per mmwlan.h doxygen: 0=1MHz, 1=2MHz, 2=4MHz.  But the
        // MM6108 supports 8 MHz and HD01 is on an 8 MHz channel — and our
        // log shows BW=? so the chip is reporting a value the docstring
        // didn't enumerate.  Most likely 3 = 8 MHz.  Print raw nibble too so
        // we can see what the chip actually thinks it's negotiating.
        char bw_buf[12];
        const char *bw_name = (bw == 0) ? "1MHz" : (bw == 1) ? "2MHz" :
                              (bw == 2) ? "4MHz" : (bw == 3) ? "8MHz?" : "?";
        snprintf(bw_buf, sizeof(bw_buf), "%s(bw=%lu)", bw_name, (unsigned long)bw);
        const char *bw_str = bw_buf;
        uint32_t succ = st->total_success[best_idx];
        uint32_t pct  = best_sent ? (100u * succ) / best_sent : 0;
        uint32_t hb_age = millis() - g_lwip_hb_ms;
        Serial.printf("[halow] sta=%s rssi=%ddBm rate=%s MCS%u %s sent=%lu ok=%lu (%lu%%)  "
                      "hb_age=%lums  heap_internal: free=%u min=%u\n",
                      sta_str, (int)rssi, bw_str, (unsigned)mcs, gi ? "SGI" : "LGI",
                      (unsigned long)best_sent, (unsigned long)succ, (unsigned long)pct,
                      (unsigned long)hb_age,
                      (unsigned)free_internal, (unsigned)min_internal);
        Serial.flush();
    } else {
        uint32_t hb_age = millis() - g_lwip_hb_ms;
        Serial.printf("[halow] sta=%s rssi=%ddBm  hb_age=%lums  heap_internal: free=%u min=%u\n",
                      sta_str, (int)rssi, (unsigned long)hb_age,
                      (unsigned)free_internal, (unsigned)min_internal);
        Serial.flush();
    }

    if (st != nullptr) mmwlan_free_rc_stats(st);

    // Path C: rate limiter stats print removed.  Limiter is disarmed in
    // soft_ap.cpp — counters would all read zero.  Re-add this block if
    // you ever re-enable hc33_tcp_limit_* calls.

    // HaLow TX linkoutput stats.  busy= climbing during webpage load means
    // the chip's TX pool was full and we cleanly dropped instead of blocking
    // the TCPIP thread (which is exactly what we want — verify hb_age stays
    // low at the same time).  ok+busy+alloc+send = attempts.
    hc33_halow_tx_nonblock_stats_t tx;
    hc33_halow_tx_nonblock_get_stats(&tx);
    Serial.printf("[tx] attempts=%lu ok=%lu busy=%lu alloc=%lu send=%lu timeout=%lums\n",
                  (unsigned long)tx.attempts,
                  (unsigned long)tx.ok,
                  (unsigned long)tx.busy_drops,
                  (unsigned long)tx.alloc_drops,
                  (unsigned long)tx.send_drops,
                  (unsigned long)tx.timeout_ms);
    Serial.flush();

    // Per-core idle %.  When cpu0 plummets toward 0% during a webpage load
    // while cpu1 stays high, we've confirmed core-0 saturation as the stall
    // cause and pinning lwIP to cpu1 is the move.  When BOTH crash, repinning
    // won't help and total work has to drop instead.
    cpu_stats_dump_line();
}

// Bring the softAP up exactly when HaLow is associated, down otherwise.
// Without this, a wedged or out-of-range HaLow leaves the SSID broadcasting
// with no working uplink: the mower can't reach the cloud through us, AND
// because the AP signal is strong it doesn't roam to a better one.  Polled
// from loop() since calling WiFi.softAP{,disconnect}() must happen on the
// Arduino main task, not the mmwlan callback context.
static void halow_softap_sync() {
#if HC33_SKIP_SOFTAP
    return;   // diagnostic mode — softAP intentionally never brought up
#else
    static enum mmwlan_sta_state last_state = MMWLAN_STA_DISABLED;
    enum mmwlan_sta_state state = mmwlan_get_sta_state();
    if (state == last_state) return;

    if (state == MMWLAN_STA_CONNECTED) {
        if (!soft_ap_is_up()) {
            Serial.println("[main] HaLow up — bringing softAP back");
            Serial.flush();
            soft_ap_begin();
        }
    } else {
        if (soft_ap_is_up()) {
            Serial.printf("[main] HaLow state=%d — taking softAP down\n", (int)state);
            Serial.flush();
            soft_ap_stop();
        }
    }
    last_state = state;
#endif
}
#endif

void loop() {
    // Reset task watchdog as soon as we re-enter the loop.  Anything that
    // blocks past TWDT timeout (30 s) without returning to loop() triggers
    // a panic-reboot; setup() registered loopTask with TWDT.
    esp_task_wdt_reset();

    g_tcp.loop();
#ifndef USE_STANDARD_WIFI
    halow_link_stats_dump();
    halow_softap_sync();
#endif

    // Push the bonded mower's BLE-advertised name into mDNS at ~1 Hz so the
    // onboarding flow can match (cloud device_name) ↔ (proxy bonded_name).
    // No-op until the BLE scan runs (first TCP connect) and a no-op on every
    // subsequent unchanged value, so this costs ~one std::string compare.
    static uint32_t s_mdns_last_ms = 0;
    if (millis() - s_mdns_last_ms >= 1000) {
        s_mdns_last_ms = millis();
        mdns_advert_refresh(g_ble.get_bonded_name());
    }

    // Fire the one-shot task table dump 3s after boot, once.  We do it here
    // rather than at the end of setup() so the WiFi/NimBLE/HaLow tasks have
    // genuinely settled — at setup() exit some of them are still spinning up.
    if (!s_task_dump_done) {
        if (s_task_dump_at_ms == 0) s_task_dump_at_ms = millis() + 3000;
        if ((int32_t)(millis() - s_task_dump_at_ms) >= 0) {
            cpu_stats_task_dump();
            s_task_dump_done = true;
        }
    }

    delay(1);   // yield to FreeRTOS so NimBLE and WiFi tasks get CPU
}
