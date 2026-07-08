# HC33 BLE proxy firmware

Firmware for the **Heltec HT-HC33** (ESP32-S3 + Morse Micro MM6108 HaLow) that
turns the module into a network-attached bridge to a Mammotion Luba mower's BLE.
It runs a NimBLE **central** that connects to the mower's GATT and a TCP server
that tunnels PyMammotion's BluFi traffic over the network, so the web-server
(and PyMammotion) can drive the mower from anywhere on the LAN instead of over
short-range Bluetooth.

Wire path:

```
web-server / PyMammotion  ──TCP(9876)──►  HC33 firmware  ──BLE/GATT──►  Luba mower
        (HC33ProxyTransport)              (this repo)      (NimBLE central)
```

Two RF uplinks are supported, selected at build time (see [Build targets](#build-targets)):

- **HaLow (production)** — the MM6108 joins an 802.11ah AP (HD01), and the S3's
  built-in 2.4 GHz radio runs a softAP the mower joins; mower traffic is NAPT'd
  out over HaLow to reach the Mammotion cloud.
- **Standard 2.4 GHz WiFi (dev)** — the S3's radio is a plain WiFi STA. Simpler
  to build and flash; used for bench work.

BLE (NimBLE, internal to the S3) is identical in both modes.

---

## Three ways to build / flash

Pick the path that matches how much you want to build:

| | Path | What you need | When |
|---|---|---|---|
| **1** | **Flash a prebuilt binary** (no toolchain) | A Chromium browser + USB cable | You just want a working proxy and aren't changing firmware code → [Flash with the browser](#flash-with-the-browser-no-toolchain) |
| **2** | **Build the firmware, link our prebuilt HaLow libs** | PlatformIO + an `ESP_HaLow` clone + `halow-overlay/` from this repo | You're editing firmware code (the common case) → [One-time HaLow setup](#one-time-setup-for-the-halow-build-envhc33), then [Build & flash](#build--flash-with-platformio) |
| **3** | **Rebuild the HaLow libraries too** | Path 2 **plus** an ESP-IDF 5.1.2 tree | You need to change NAPT / lwIP / `esp_netif` itself → [Rebuilding the patched libraries](#rebuilding-the-patched-libraries-advanced) |

The **standard-wifi** build (`env:hc33-standard-wifi`) is a lighter Path 2 — it uses
the stock PlatformIO `espressif32` platform and needs **no** `ESP_HaLow` clone or
overlay. Paths 2 and 3 below apply to the HaLow (`env:hc33`) build.

> **Path 3 has a ceiling.** Only the **open-source IDF archives** (`liblwip.a`,
> `libesp_netif.a` — where our NAPT patch lives) are rebuildable. The **Morse HaLow
> stack** (`libmorse.a`, `libmm6108.a`, `libbcf_mf08551.a`) is consumed as Heltec's
> **prebuilt** binaries (Morse proprietary, under the BDL — see `halow-overlay/LICENSES/`);
> we don't rebuild those. So "compile everything" really means *your code + the Heltec
> HAL source + the IDF archives, linked against the Morse blobs.*

---

## Hardware

- **Board:** Heltec HT-HC33 (ESP32-S3, 8 MB flash, MM6108 HaLow).
- **Antennas:** two U.FL connectors only — front = HaLow (MM6108), back = LoRa
  (SX1262, unused here). Don't swap them.
- **USB:** CP210x UART bridge (used for PlatformIO upload and the web flasher).
- **Placement:** the HC33 sits <10 cm from the mower, so it dominates the BLE
  scan — the mower is auto-discovered by service UUID + RSSI, no MAC needed.

## Source layout

| File | Role |
|---|---|
| `src/main.cpp` | Setup + `loop()`; wires the modules together |
| `src/ble_central.cpp` | NimBLE central: scan (by UUID/RSSI), connect, GATT write/notify |
| `src/tcp_proxy.cpp` | TCP server (single client), length-prefixed BluFi framing, idle watchdog |
| `src/tcp_rate_limit.cpp` | Per-client TX pacing (HaLow medium-share cap) |
| `src/halow_init.cpp` | MM6108 bring-up, association, static/DHCP IP |
| `src/halow_tx_nonblock.cpp` | Non-blocking HaLow linkoutput (avoids parking the TCPIP thread) |
| `src/soft_ap.cpp` | 2.4 GHz softAP + NAPT for the mower (HaLow env only) |
| `src/discovery.cpp` | UDP broadcast responder (port 9878) for onboarding auto-discovery |
| `src/mdns_advert.cpp` | mDNS advertisement (`_hc33proxy._tcp`) |
| `src/config_load.cpp` | Reads the `config` flash partition at boot (overrides `config.h`) |
| `src/cpu_stats.cpp` | Optional per-core load logging |
| `include/config.h` | **Compile-time** configuration (SSIDs, region, timeouts, UUIDs) |
| `partitions/hc33.csv` | Flash partition table (both envs) |
| `scripts/pack_config.py`, `scripts/flashcfg.py` | Build the `config` blob and the `flashcfg` PIO target |
| `flasher/` | Browser-based (Web Serial) USB flasher — no toolchain needed |

---

## Prerequisites

- **PlatformIO** (VS Code extension or the `pio` CLI).
- On Windows the CP210x USB-UART driver (Silicon Labs).

### One-time setup for the HaLow build (`env:hc33`)

The HaLow target builds against **Heltec's arduino-esp32 fork** plus the patched
`wifi-halow` library tree. It is *not* a plain PlatformIO platform:

```bash
# from the ble-proxy repo root (one dir above firmware/)
git clone https://github.com/HelTecAutomation/ESP_HaLow.git ../ESP_HaLow
cd ../ESP_HaLow/tools && python get.py     # downloads ~600 MB of IDF binaries
```

> **Edit the absolute paths in `platformio.ini`.** The `env:hc33`
> `platform_packages` and `-L` linker flags hard-code
> `C:/path/to/ble-proxy/ESP_HaLow`. If you cloned the repo
> anywhere else, change those paths to match. PlatformIO uses `file://` URLs
> here because `symlink://` fails silently on Windows without developer mode.

#### Apply our patched HaLow libraries (required for NAPT)

A stock `ESP_HaLow` clone + `get.py` gives you Heltec's **unpatched** libraries —
the firmware will build, but with **no NAPT** (the mower's traffic won't route out
over HaLow) and the old debug morselib. The patched library — patched source **plus**
the rebuilt `.a` archives (NAPT `liblwip.a`/`libesp_netif.a`, the 2.8.2 `libmorse.a`,
`libmm6108.a`, and the correct `libbcf_mf08551.a`) — ships in this repo under
**`firmware/halow-overlay/`**.

**Step 1 — replace the whole library `src/`.** One copy lands the patched source and
all five archives (`libbcf`/`libmorse`/`libmm6108` live in `src/esp32s3/`):

```bash
# from the repo root, after cloning ESP_HaLow + running get.py
rm -rf ESP_HaLow/libraries/wifi-halow/src
cp -r firmware/halow-overlay/wifi-halow/src ESP_HaLow/libraries/wifi-halow/
```
```powershell
# Windows PowerShell
Remove-Item -Recurse -Force ESP_HaLow\libraries\wifi-halow\src
Copy-Item -Recurse firmware\halow-overlay\wifi-halow\src ESP_HaLow\libraries\wifi-halow\
```

**Step 2 — overwrite the *cached* copies of the two IDF archives.** This is the "a
few cached locations" gotcha: PlatformIO **also** resolves `liblwip.a`/`libesp_netif.a`
from the `esp32-arduino-libs` tree (and its own packages cache), and whichever the
linker finds first per the `-L` order in `platformio.ini` wins — so a single stray
**stock** copy silently shadows the patched one (symptom: builds cleanly, NAPT/routing
dead). Replace **every** copy on disk. Find them first:

```powershell
# Windows PowerShell — lists every copy PIO might link
Get-ChildItem "$HOME\.platformio","C:\path\to\ble-proxy\ESP_HaLow" -Recurse -Include liblwip.a,libesp_netif.a | Select FullName
```
```bash
# Linux / macOS
find ~/.platformio <repo>/ESP_HaLow \( -name liblwip.a -o -name libesp_netif.a \)
```

Copy `firmware/halow-overlay/wifi-halow/src/{liblwip.a,libesp_netif.a}` over each hit
(the usual one is `ESP_HaLow/tools/esp32-arduino-libs/esp32s3/lib/`). Then verify NAPT
actually made it in — a patched `liblwip.a` prints a non-zero count, a stock one `0`:

```bash
strings ESP_HaLow/libraries/wifi-halow/src/liblwip.a | grep -c ip_napt
```

The **standard-wifi** build needs none of this — it uses the stock PlatformIO
`espressif32` platform and just works after `pio` is installed.

#### Rebuilding the patched libraries (advanced)

*Path 3 — only if you're changing NAPT / lwIP / `esp_netif` itself; for normal
firmware work the prebuilt archives in `halow-overlay/` are enough.*

Our NAPT support is a rebuild of **`liblwip.a`** and **`libesp_netif.a`** from
**ESP-IDF 5.1.2** with four sdkconfig knobs (`LWIP_IP_FORWARD`, `LWIP_IPV4_NAPT`,
`LWIP_IPV4_NAPT_PORTMAP`, `LWIP_L2_TO_L3_COPY`) and two header patches (a UDP-NAPT
timeout bump and `LWIP_NETIF_LINK_CALLBACK`/pbuf-pool sizing). The full recipe —
the sdkconfig additions, the header snippets, the `idf.py set-target esp32s3 &&
idf.py build` steps, and the "wipe the build dir and re-run `set-target` between
sdkconfig changes" gotcha — is in **[`docs/heltec-napt-handoff/`](../docs/heltec-napt-handoff/)**
(written as a handoff to Heltec, but it's a complete build guide). Drop the two
rebuilt archives back into the Path-2 locations (both `wifi-halow/src/` **and** the
`esp32-arduino-libs` cache copy).

The Morse archives (`libmorse.a` / `libmm6108.a` / `libbcf_mf08551.a`) are **not**
rebuilt here — they're Heltec's prebuilt Morse binaries (proprietary, under the BDL).
Morse's own `mm-iot-esp32` SDK ships morselib *source*, but integrating a self-built
morselib into the Heltec/arduino-esp32 framework is out of scope for this project.

---

## Build targets

Defined in `platformio.ini`. Never build `hc33-base` directly — it's a shared
base for the two real envs.

| Env | Uplink | Toolchain | Use |
|---|---|---|---|
| **`hc33`** | HaLow (MM6108 → HD01) + 2.4 GHz softAP for the mower | `pioarduino` platform (GCC 13, IDF 5.1), Heltec framework | **Production** |
| **`hc33-standard-wifi`** | S3 built-in 2.4 GHz WiFi STA | stock `espressif32@6.10.0` (GCC 8.4, IDF 4.4) | Bench / dev |

Both share `partitions/hc33.csv` and the internal-BLE central.

---

## Configure before building

Edit **`include/config.h`** (compile-time defaults):

- `WIFI_SSID_HALOW` / `WIFI_PASS_HALOW` / `WIFI_REGION_HALOW` /
  `WIFI_SECURITY_HALOW` — HaLow AP credentials, regulatory region (`AU`, `US`,
  `EU`, `JP`, `NZ`, `KR`), and security (`MMWLAN_OPEN` / `MMWLAN_SAE` /
  `MMWLAN_OWE`). A wrong region or security mode makes association **fail
  silently** — try `MMWLAN_OPEN` first to rule out auth.
- `HALOW_USE_STATIC_IP` / `HALOW_STATIC_IP` / … — some HD01 revisions don't
  bridge DHCP over HaLow, so static IP is the default (pick an address outside
  the router's DHCP pool).
- `WIFI_SSID_STANDARD` / `WIFI_PASS_STANDARD` — the 2.4 GHz WiFi for the
  standard-wifi build.
- `AP_SSID` / `AP_PASSWORD` / `AP_CHANNEL` / `AP_IP` … — the mower-facing softAP
  (HaLow env only). Keep `AP_IP`'s subnet off the HaLow LAN's subnet. Password
  must be ≥8 chars or WPA2 rejects it.
- `TCP_PORT` (9876), `CLIENT_IDLE_TIMEOUT_MS` (30 s safety stop), BLE UUIDs
  (must match `pymammotion/bluetooth/const.py`) — rarely changed.

To configure **without recompiling**, use the runtime `config` partition
instead (see [Runtime config](#runtime-config-without-recompiling)).

---

## Build & flash with PlatformIO

```bash
cd firmware

# Standard-wifi (dev):
pio run -e hc33-standard-wifi -t upload

# HaLow (production):
pio run -e hc33 -t upload

# Serial monitor (115200):
pio device monitor -b 115200
```

`upload_port` defaults to `COM3` (Windows) in `platformio.ini`; override with
`--upload-port <PORT>` (e.g. `/dev/ttyUSB0` on Linux) or edit the ini. Upload
speed is 921600. The monitor has the ESP32 exception decoder enabled.

Build artifacts land in:

```
firmware/.pio/build/hc33/                 (HaLow)
firmware/.pio/build/hc33-standard-wifi/   (standard WiFi)
```

---

## Flash with the browser (no toolchain)

For flashing a pre-built binary onto an AliExpress-bought HC33 without
PlatformIO, use the Web Serial flasher in **`flasher/`** — a static page, no
backend. See `flasher/README.md` for hosting (GitHub Pages) and the end-user
steps; `flasher/firmware/README.md` documents the `.bin` layout and offsets.

**Refreshing the flasher's binaries after a firmware change:** copy the four
`.bin` files from each PIO build dir into the flasher's variant folder:

```
firmware/.pio/build/hc33/{bootloader,partitions,boot_app0,firmware}.bin
    → firmware/flasher/firmware/hc33-halow/
firmware/.pio/build/hc33-standard-wifi/{bootloader,partitions,boot_app0,firmware}.bin
    → firmware/flasher/firmware/hc33-wifi/
```

The flasher writes each at a fixed offset (matching `partitions/hc33.csv`) and
lets the user supply their own config blob, which it writes to the `config`
partition at `0x7EF000`.

---

## Runtime config (without recompiling)

The `config` flash partition (`type=data subtype=0x99`, at `0x7EF000`, 4 KB)
lets you set SSID/password/region/softAP **without a rebuild**. `config_load.cpp`
reads it at boot; if it's blank/corrupt it falls back to `config.h`.

```bash
cp config.local.ini.example config.local.ini   # then edit your values
pio run -e hc33 -t flashcfg                     # packs + writes the config partition
```

`scripts/pack_config.py` packs `config.local.ini` into the `hc33_config_v1`
binary blob. The browser flasher builds the same blob in `flasher/pack.js` —
**keep `pack.js` and `pack_config.py` byte-identical** when either changes.

---

## Partition table (`partitions/hc33.csv`)

Board-correct Heltec HT-HC33 layout (8 MB), owned in-repo so re-cloning
`ESP_HaLow` can't clobber it. Only change vs stock: `user_data` shrank 4 KB to
free space for the `config` partition.

| Name | Offset | Size | Purpose |
|---|---|---|---|
| `bootloader` | `0x000000` | — | 2nd-stage bootloader |
| `partitions` | `0x008000` | — | this table |
| `boot_app0` | `0x00e000` | 8 KB | OTA selector |
| `app0` / `app1` | `0x010000` / `0x390000` | 3.5 MB each | OTA app slots |
| `spiffs` / `fr` | `0x710000` / `0x7B0000` | — | filesystem |
| `key_data` | `0x7D0000` | 8 KB | MM6108 license (do not erase) |
| `config` | `0x7EF000` | 4 KB | runtime config blob |
| `coredump` | `0x7F0000` | 64 KB | crash dumps |

---

## Porting & reuse

The mower-specific logic lives in the **software** (PyMammotion + the
web-server), *not* the firmware. The HC33 firmware is a protocol-agnostic
**GATT-over-network pipe**: it connects to a BLE device's GATT and forwards raw
bytes both ways — each wire frame is exactly one characteristic write, each
notification is one wire frame (`main.cpp`: *"the HC33 has no protocol
knowledge"*). That makes three kinds of reuse straightforward.

### Other Mammotion models
No firmware change — PyMammotion already covers the wider Luba/Yuka range. The
proxy and range-extension are identical; you just point the web-server at that
model. Essentially free.

### Other BLE devices
The firmware carries no protocol knowledge — it is parameterised by three UUIDs
in `include/config.h`:
- `UUID_SERVICE` — matched during the scan and used to open the GATT service
- `UUID_WRITE_CHARACTERISTIC` — where outbound frames are written
- `UUID_NOTIFY_CHARACTERISTIC` — subscribed for inbound notifications

Point those at your device's GATT and the firmware bridges it as-is; the
device's actual protocol/codec then lives on the PC side (replace the
PyMammotion/BluFi framing with your own over the same length-prefixed socket).
One caveat: the scan currently matches the **strongest advertiser of
`UUID_SERVICE`** (the HC33 sits centimetres from the mower). For a non-unique
service UUID, match by MAC or device name instead — see `ble_central.cpp`.

### Other ESP32 boards (the standard-wifi build)
`env:hc33-standard-wifi` is a plain **NimBLE central + Wi-Fi STA + TCP server**:
none of the HaLow/Morse/BCF/custom-lwip machinery (all gated behind
`USE_STANDARD_WIFI`), and no board-specific GPIO wiring (the only pin pokes —
GPIO 6/7/9 — are the MM6108 reset, which is HaLow-only). Retargeting is mostly
PlatformIO config, not code:

- Set `board = <your board>` (e.g. `esp32dev`, `esp32-c3-devkitm-1`).
- Provide a **partition table for the target's flash size**.
  `partitions/hc33.csv` is an 8 MB layout (two 3.5 MB app slots + a 4 KB `config`
  partition at `0x7EF000`); a 4 MB board won't fit it — use a stock table and
  drop/move the `config` partition if you keep runtime config.
- Drop the HC33-specific flash/PSRAM flags (`flash_mode=dio`, `boot=qio`,
  `memory_type=qio_qspi`, no `BOARD_HAS_PSRAM`) — those only dodge the HC33's
  MSPI quirk. Use your board's defaults, and enable PSRAM if it actually has it.
- The chip **must have Bluetooth**: ESP32, ESP32-S3, ESP32-C3, ESP32-C6 all work;
  **ESP32-S2 does not** (no BLE).
- The `NimBLE-Arduino @ ^1.4.3` pin is for arduino-esp32 v2 (the pinned
  `espressif32@6.10.0`). On an arduino-esp32 v3 platform, bump NimBLE to `^2.0.0`
  and adjust the scan callback (see the note in `platformio.ini`).

What you **don't** get on a generic board: the HaLow long-range uplink and the
mower-facing softAP/NAPT — those are `env:hc33` (HaLow) features. You'd reach the
device over your normal Wi-Fi instead.

---

## Future directions

### Self-hosted camera (fully cloud-independent video)
The **control** path is already cloud-free (local BLE). The **camera**, however,
currently uses the mower's own feed via Mammotion's cloud (Agora) — the one part
of the stack that still depends on their infrastructure.

The HT-HC33 is itself a **Wi-Fi HaLow camera board**: it ships with an **OV3660**
sensor and an SD slot, and its variant
(`ESP_HaLow/variants/HT-HC33/pins_arduino.h`) defines the full DVP camera pinout
on GPIOs that don't clash with the MM6108 (HaLow) pins. So a mower-mounted,
self-hosted stream (e.g. the `esp32-camera` library + MJPEG) would make the whole
system independent of any cloud. Two things to reconcile first:

- **PSRAM.** Camera framebuffers need it, but the proxy firmware currently builds
  with PSRAM **disabled** — the MSPI-quirk workaround in `platformio.ini` (no
  `BOARD_HAS_PSRAM`, `memory_type=qio_qspi`). Heltec's own camera firmware uses
  the board's 8&nbsp;MB PSRAM, so a camera build must re-enable it with the correct
  `memory_type`; reconcile that note before turning the camera on.
- **Bandwidth.** On the HaLow long-range build the stream shares the ~10–12&nbsp;Mbps
  medium with control traffic — the same throughput budget behind the TX-wedge
  work. Keep it low-res / low-fps and test under sustained load. On the
  standard-wifi build this is a non-issue.

Note this would be a mower-mounted forward view — a complement to (not a
replacement for) the Luba's own gimbal camera.

---

## Troubleshooting / known build gotchas

Most of these are also explained inline in `platformio.ini`:

- **Deaf RX / short HaLow range** → wrong BCF. The HT-HC33 must link
  `-lbcf_mf08551` (Morse Micro reference design), **not** `-lbcf_HC01_V2_H`.
  The wrong BCF silently mis-calibrates the MM6108.
- **Standard-wifi build crashes at BLE bring-up** (`LoadProhibited` in
  `esp_bt_controller_init`) → the `pioarduino` platform installed by `env:hc33`
  globally claims the `espressif32` name, so `hc33-standard-wifi` silently
  builds against arduino-esp32 v3 / IDF 5.x, which NimBLE 1.4.3 can't handle.
  The env pins `espressif32@6.10.0` to prevent this — keep the pin.
- **MSPI corruption / boot loop on HaLow** → don't define `BOARD_HAS_PSRAM`.
  The HC33 has no accessible PSRAM; `psramInit()` would probe and corrupt the
  bus. The `qio_qspi` memory type keeps `psramInit()` a no-op stub.
- **HaLow C++ compile errors** (`C99 designator outside aggregate initializer`)
  → the `env:hc33` build forces `-std=gnu++2a` + `-fpermissive` for
  `mmwlan_regdb.h`. The `build_unflags` strips every other `-std=` first.
- **HaLow associates but no traffic / intermittent DNS** → HaLow-side issues
  (softAP/NAPT stalls, TX wedge, UDP NAPT timeout) are documented separately in
  `docs/` (Heltec NAPT handoff) and the deeper lib-rebuild history. The shipped
  `wifi-halow` tree already includes the release `libmorse.a`, NAPT-patched
  `liblwip.a`/`libesp_netif.a`, and the non-blocking TX fix.
- **`pio` picks the wrong serial port** → set `--upload-port` / `--monitor-port`
  or edit `upload_port` in `platformio.ini`.

For the standard-wifi bench build none of the HaLow-specific items apply — it's
the quickest path to a working proxy for testing the BLE + TCP path.
