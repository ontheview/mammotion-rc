# NAPT support for Heltec ESP_HaLow / arduino-esp32 — handoff to Heltec

This folder documents everything we changed to get NAPT (source-NAT, used to route
a softAP client's traffic out through the HaLow STA) working with `arduino-esp32`
on the HT-HC33 — originally written to hand off to Heltec, but it doubles as a
complete rebuild guide.  We did this by **rebuilding `liblwip.a` and `libesp_netif.a`
from ESP-IDF 5.1.2 with a few sdkconfig changes and two header patches**, then
dropping the rebuilt archives into `ESP_HaLow/libraries/wifi-halow/src/`.  No
arduino-esp32 source changes — Heltec's framework consumes the rebuilt
archives transparently.

Contents of this folder:

- `sdkconfig.napt-additions` — the **NAPT-specific** items you need to add to
  whatever sdkconfig your IDF rebuild uses (we used your `halow_config.h`
  converted to sdkconfig.defaults; this file lists only the NAPT additions on
  top of that baseline).
- `patches/lwipopts.h.snippet` — header edits to ESP-IDF's
  `components/lwip/port/include/lwipopts.h`.  Two reasons: one Kconfig knob
  IDF 5.1 doesn't expose, plus pbuf pool sizing for NAT-forwarded TCP bursts.
- `patches/lwip_napt.h.snippet` — single-line UDP NAPT timeout bump in
  `components/lwip/lwip/src/include/lwip/lwip_napt.h`.  Without this DNS
  breaks intermittently and Windows clients fall back to slow TCP DNS.
- `arduino-test-sketch/` — minimal Arduino sketch that brings up HaLow STA +
  softAP + enables NAPT, so you can verify a phone associated to the softAP
  can reach the upstream LAN via NAPT through the HaLow uplink.
- `runtime-napt-enable-snippet.cpp` — the exact runtime API calls
  (`esp_netif_napt_enable` + `netif_set_default`) that activate NAPT once the
  rebuilt libs are in place.

---

## Summary of changes (in order of necessity)

### 1. sdkconfig — four NAPT-specific Kconfig items

These are the **only NAPT-specific Kconfig changes**.  All four are required:

```
CONFIG_LWIP_IP_FORWARD=y
CONFIG_LWIP_IPV4_NAPT=y
CONFIG_LWIP_IPV4_NAPT_PORTMAP=y
CONFIG_LWIP_L2_TO_L3_COPY=y
```

The non-obvious one is `CONFIG_LWIP_L2_TO_L3_COPY=y`.  The comment in
`components/lwip/port/include/lwipopts.h` is explicit: both `IP_FORWARD` AND
`L2_TO_L3_COPY` are required for NAPT to function on ESP platforms — without
the L2-to-L3 copy, packets received by the WiFi driver on the AP iface never
reach lwIP for forwarding.  **This is the single most likely cause of "NAPT
didn't work" if you only enable the obvious three knobs.**

### 2. lwIP header edit — UDP NAPT timeout

In `components/lwip/lwip/src/include/lwip/lwip_napt.h`, change the default UDP
NAPT timeout.  Some IDF versions ship a 2-second default — that's far too
short for DNS.  Bump to 180 s (3 minutes) to match common router behaviour:

```c
#define IP_NAPT_TIMEOUT_MS_UDP (180*1000)   /* was (2*1000) */
```

In recent IDF this is already 180 s; older trees may differ.  **Verify the
default in the IDF version you're building against.**  If you ship the 2 s
default, you'll get this symptom: NAPT works for HTTP at first, then DNS
queries from clients start failing intermittently, and Windows clients flood
TCP fallback packets — looks like NAPT "doesn't work" but it actually does,
just the DNS lookup translation entry has timed out before the reply arrives.

### 3. lwipopts.h edits (port/include/lwipopts.h)

Two non-NAPT items but they were required to get the rebuilt lwIP linking
cleanly into Heltec's framework alongside your other `.a` files:

```c
/* libmmipal.a links against netif_set_link_callback which is gated on
 * LWIP_NETIF_LINK_CALLBACK.  IDF 5.1 doesn't expose this via Kconfig, so
 * hard-enable it here to match the layout libmmipal expects.  Without this,
 * libmmipal.a fails to link with undefined references. */
#define LWIP_NETIF_LINK_CALLBACK        1

/* pbuf pool sized for NAT-forwarded TCP bursts.  Default 16 starved the
 * softAP under load — beacons got delayed when the pool exhausted, causing
 * client disassociations.  64 reclaims a fair amount vs 128 while keeping
 * enough headroom. */
#define PBUF_POOL_SIZE                  64
#define MEMP_NUM_PBUF                   64
```

(Pool sizing is tuneable to taste — 16 is too low, 128 wastes RAM, 64 has
worked well for our HC33 use case with one or two softAP clients + ~1 Mbps
bidirectional through NAPT.)

### 4. Runtime — enable NAPT and set the default route

NAPT must be enabled at runtime after the softAP and the HaLow STA are both
up.  See `runtime-napt-enable-snippet.cpp` for the exact code, but the
essentials:

```c
/* AP netif: enable NAPT (source-NAT for outbound packets) */
esp_netif_t* ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
esp_netif_napt_enable(ap_netif);

/* HaLow netif: make it lwIP's default route, so NAPT-forwarded packets
 * have an egress interface.  Without this, NAPT decides to forward but
 * has nowhere to send and silently drops. */
struct netif* halow_netif = mmipal_get_lwip_netif();
netif_set_default(halow_netif);
```

The default-route step is easy to miss because nothing fails loudly — packets
just disappear.  `mmipal_init()` doesn't call `netif_set_default()` for the
HaLow netif (we read the source), so you have to do it yourself after both
interfaces are up.

### 5. softAP DHCP — push DNS to clients

Not strictly NAPT, but related: the softAP's default DHCP server doesn't
offer a DNS server to clients, so even with NAPT working, browsers can't
resolve hostnames.  Configure dhcps BEFORE `softAP()` starts it:

```c
dhcps_offer_t offer_dns = OFFER_DNS;
esp_netif_dhcps_option(ap_netif, ESP_NETIF_OP_SET,
                       ESP_NETIF_DOMAIN_NAME_SERVER,
                       &offer_dns, sizeof(offer_dns));

esp_netif_dns_info_t dns_info = {};
dns_info.ip.u_addr.ip4.addr = ipaddr_addr("192.168.1.1");  /* upstream DNS */
dns_info.ip.type = ESP_IPADDR_TYPE_V4;
esp_netif_set_dns_info(ap_netif, ESP_NETIF_DNS_MAIN, &dns_info);
```

---

## Build recipe (rebuilding liblwip.a and libesp_netif.a)

1. Get a clean ESP-IDF 5.1.2 tree (matches the IDF your `framework-libs` was
   built against — important for ABI compatibility).
2. Create a minimal IDF project that depends on the `lwip` component and an
   empty `app_main()`.
3. Copy `sdkconfig.napt-additions` into your project's `sdkconfig.defaults`.
   If you're starting from your `halow_config.h`-derived sdkconfig, append
   these lines at the bottom.
4. Apply the two header patches (`patches/lwipopts.h.snippet` and
   `patches/lwip_napt.h.snippet`) to your IDF tree's lwip component.
5. `idf.py set-target esp32s3 && idf.py build`
6. The relevant artifacts after build:
   - `build/esp-idf/lwip/liblwip.a`
   - `build/esp-idf/esp_netif/libesp_netif.a`
7. Drop both into `ESP_HaLow/libraries/wifi-halow/src/esp32s3/` (or wherever
   the existing copies live in your framework distribution).
8. Important gotcha from our experience: **wipe the build directory and run
   `idf.py set-target esp32s3` AGAIN** between sdkconfig changes.  IDF
   caches the target's sdkconfig in a way that doesn't always pick up your
   sdkconfig.defaults edits.

## Verifying it works

Once Heltec's framework ships the new archives:

1. Run the `arduino-test-sketch/` against it on an HT-HC33.
2. Associate a phone or laptop to the `MowerAP` SSID (192.168.4.0/24).
3. From that client, ping the upstream LAN gateway (e.g. 192.168.1.1, the
   HaLow side's router).  Should succeed.
4. From that client, browse `http://example.com` or any public host.  Should
   succeed (this exercises DNS through the softAP's dhcps push, then HTTP
   through NAPT).
5. Sustained test: from the client, run a few minutes of `iperf3` or a
   large file download.  No NAT table exhaustion, no stalls.

If any of these fail, the most likely cause in order of probability:
- `CONFIG_LWIP_L2_TO_L3_COPY=y` missing → step 2/3 fail.
- Default route not set on HaLow netif → step 3 fails (ICMP echo gone).
- UDP NAPT timeout too short → step 4 fails intermittently (DNS works once
  then breaks for ~minutes).
- pbuf pool too small → step 5 stalls under load.

---

## Source paths in our tree (for reference)

If you want to see exactly what we shipped:

- Rebuilt archives: `ble-proxy/ESP_HaLow/libraries/wifi-halow/src/liblwip.a` and `libesp_netif.a`
- Our rebuild project: `esp-idf-build/lwip-rebuild/` (sdkconfig.defaults + the standard IDF minimal-project layout)
- Modified IDF headers: `esp-idf-build/esp-idf/components/lwip/port/include/lwipopts.h` and `esp-idf-build/esp-idf/components/lwip/lwip/src/include/lwip/lwip_napt.h`
- Runtime NAPT-enable code (in our firmware): `ble-proxy/firmware/src/soft_ap.cpp`
