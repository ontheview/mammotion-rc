# Third-Party Notices

This project (the "Mammotion Remote Controller" / BLE proxy) is licensed under
**GPL-3.0-or-later** — see [`LICENSE`](LICENSE). It builds on, links, or
redistributes the third-party components listed below, each under its own
license. This file is informational; the authoritative terms are each project's
own license text.

> **The prebuilt firmware images** distributed via the browser flasher
> (`firmware/flasher/`) embed the **proprietary Morse Micro** components listed
> in section 3. Those images are therefore distributed under the **combination**
> of GPL-3.0 (this project + Heltec ESP_HaLow) **and** the Morse Micro Binary
> Distribution License — **not GPL-3.0 alone** — and may be redistributed only
> for use with Morse Micro Wi-Fi HaLow hardware (e.g. the Heltec HC33).

---

## 1. GPL-3.0 upstreams (why this project is GPL-3.0)

| Component | License | Where used |
|---|---|---|
| [PyMammotion](https://github.com/mikey0000/PyMammotion) | GPL-3.0 | Web-server imports it to talk to the mower |
| [Heltec ESP_HaLow](https://github.com/HelTecAutomation/ESP_HaLow) | GPL-3.0 | Firmware build framework (arduino-esp32 fork + `wifi-halow` wrappers) |

Because the web-server links PyMammotion and the firmware links the ESP_HaLow
framework, both parts are combined works under GPL-3.0.

## 2. Permissive dependencies (GPL-compatible)

**Web-server (Python):**

| Component | License |
|---|---|
| FastAPI | MIT |
| Starlette | BSD-3-Clause |
| Uvicorn | BSD-3-Clause |
| Pydantic | MIT |
| cryptography | Apache-2.0 OR BSD-3-Clause |
| bleak, aiohttp, anyio, and other transitive deps | MIT / Apache-2.0 / BSD (permissive) |

**Firmware (C/C++):**

| Component | License |
|---|---|
| [NimBLE-Arduino](https://github.com/h2zero/NimBLE-Arduino) | Apache-2.0 |
| ESP-IDF / arduino-esp32 core | Apache-2.0 (some components LGPL-2.1 / BSD) |
| lwIP | BSD-3-Clause |
| Morse [`mm-iot-esp32`](https://github.com/MorseMicro/mm-iot-esp32) SDK wrapper sources | Multi-licensed: Apache-2.0, BSD-3-Clause, MIT, Zlib, GPL-2.0-or-later, GPL-3.0-or-later (see that SDK's `LICENSES/` folder) |

**Browser flasher (`firmware/flasher/`, vendored into the page — not CDN-loaded):**

| Component | License |
|---|---|
| [esptool-js](https://github.com/espressif/esptool-js) `0.5.4` (vendored at `firmware/flasher/vendor/esptool-js-0.5.4.js`) | Apache-2.0 |
| [pako](https://github.com/nodeca/pako) (bundled inside the esptool-js file) | MIT AND Zlib |

## 3. Proprietary — Morse Micro Wi-Fi HaLow firmware & driver

The following are **proprietary binaries**, **not** open source, licensed under
the **Morse Micro Binary Distribution License (BDL)** — see
[`LICENSES/LicenseRef-MorseMicroBDL.txt`](mm-iot-esp32/LICENSES/LicenseRef-MorseMicroBDL.txt)
in the `mm-iot-esp32` SDK:

- `libmorse.a` — Morse Micro HaLow host driver / supplicant
- `libmm6108.a`, `mm6108*.mbin` — MM6108 chip firmware and loader
- `libbcf_mf08551.a` — board RF calibration (BCF) for the HT-HC33

**BDL terms that matter for redistribution:**

- May be reproduced and distributed **complete, unmodified, and as provided by
  Morse Micro**, **solely for use with hardware containing a Morse Micro Wi-Fi
  HaLow chip** (the HC33's MM6108 qualifies).
- Must be distributed **with a copy of the BDL** and **all copyright notices
  retained**.
- **No** reverse-engineering, modification, sublicensing, sale, or transfer of
  the license; export restrictions apply.

When combined into the firmware image, these blobs function as the chip's
enabling firmware/driver and are treated as **System Libraries** under GPLv3 §1
(analogous to a GPL operating system shipping proprietary device firmware). This
project does not relicense them and claims no rights over them.

For any use beyond redistribution with Morse HaLow hardware — or for certainty
about linked-binary redistribution — contact Morse Micro for a commercial
license (see `LICENSES/LicenseRef-MorseMicroCommercial.txt`).

## 4. Runtime / CDN components (not bundled, not redistributed)

Loaded by the browser at runtime from third-party CDNs; **not** included in this
repository and therefore not relicensed by it:

| Component | License / Terms | Notes |
|---|---|---|
| [Agora RTC SDK (Web)](https://www.agora.io/) | Proprietary — Agora Terms of Service | Loaded from `download.agora.io`; carries the mower camera stream |
| [nipplejs](https://github.com/yoannmoinet/nipplejs) | MIT | Loaded from jsDelivr; on-screen joystick |

---

*This notice is provided for convenience and is not legal advice. Each
component's own license governs. If in doubt — particularly regarding the Morse
Micro BDL and redistribution of the compiled firmware — seek confirmation from
the respective rights holder.*
