# Mammotion Remote Controller (BLE proxy)

Turn a Mammotion robot mower into what it should always have been: remotely
controllable over the internet, with a live camera, and Wi-Fi coverage extended
across your whole property — using a Heltec HC33 (ESP32-S3 + Wi-Fi HaLow) as a
Bluetooth-over-network proxy, and PyMammotion driving it from a web page.

> 📷 For photos of the hardware setup and screenshots of the web UI, see
> https://ontheview.com.au/&lt;placeholder&gt;

## Layout

| Path | What it is |
|---|---|
| `firmware/` | HC33 firmware — BLE-over-network proxy + Wi-Fi HaLow bridge. See [`firmware/README.md`](firmware/README.md). |
| `web-server/` | FastAPI control server + browser UI (joystick, camera, status). See [`web-server/README.md`](web-server/README.md). |
| `firmware/flasher/` | Browser-based (Web Serial) firmware flasher. |
| `docs/` | Engineering notes (HaLow, NAPT, SDK migration). |

## Hardware

Two ways to set it up:

- **Simple & low-cost (~$60 AUD):** the HC33 joins your existing home Wi-Fi —
  full control and camera anywhere your Wi-Fi already reaches. Ideal for
  normal-sized yards.
- **Long range (~$180 AUD):** add the HD01 HaLow access point (and the HaLow
  antenna) to push coverage out to 1–2 km across a large property. Same web
  control and camera — HaLow just adds reach.

| Part | What it's for | Approx. |
|---|---|---|
| [Heltec HC33](https://www.aliexpress.com/item/1005009242817119.html) | The on-mower brain — HaLow radio + Bluetooth + Wi-Fi | $45 AUD |
| [Heltec HD01 Wi-Fi HaLow AP](https://www.aliexpress.com/item/1005010018577892.html) *(optional — HaLow only)* | Long-range access point at the house — skip it to run over your existing Wi-Fi | $120 AUD |
| [915 MHz antenna + U.FL→SMA cable](https://www.aliexpress.com/item/1005008844803230.html) *(optional — HaLow only)* | HaLow antenna for the HC33 — only needed for the long-range HaLow setup | $5 AUD |
| [90° elbow USB-C to USB-C cable](https://www.aliexpress.com/item/1005008784687533.html) | Neat power connection to the mower | $4 AUD |
| [Project box](https://www.aliexpress.com/item/1005003422036596.html) | Weather-resistant enclosure | $4.50 AUD |

**Total — over existing Wi-Fi / with HaLow long range: ~$60 / ~$180 AUD.**

You'll also need a PC, or any other Python-capable device, on the same network.

## Install

> ⚠️ **Use a secondary Mammotion account — never your main one.** This project
> signs in to the Mammotion cloud with your account credentials. Logging in from
> here can **sign your phone app out**, and automated/heavy use risks the account
> being **temporarily blocked**. Set up a dedicated account first (see step 0).

0. **(Recommended) Create a separate Mammotion account and share your mower with
   it.** In the Mammotion app, register a *new* account under a different email,
   then from your **main** account share the mower to it (in the app: your device
   → **Share** / **Device sharing** → add the new account). Use this secondary
   account's credentials in step 6 — that way, if it gets signed out or blocked,
   your main account and app login are unaffected.

1. **Install Python 3.13 or 3.14.** PyMammotion requires `>=3.13,<3.15` — get it
   from [python.org](https://www.python.org/downloads/) or your package manager.

2. **Clone the repo:**
   ```
   git clone https://github.com/ontheview/mammotion-rc.git
   cd mammotion-rc
   ```

3. **Run the installer:**
   ```
   cd web-server
   python install.py        # Windows  (or: py -3.14 install.py)
   python3 install.py       # Linux / macOS
   ```
   It creates a virtualenv, installs PyMammotion + the web server, generates a
   self-signed TLS certificate, and prompts you for a **web-UI login password**
   (and, optionally, your Mammotion account). It writes `run-server.bat` /
   `run-server.sh` with every path baked in. Safe to re-run any time.

4. **Flash the firmware** onto the HC33 — easiest with the browser flasher, no
   toolchain required. In order:
   1. On Windows, **install the SiLabs CP210x USB driver** once, from
      [SiLabs' download page](https://www.silabs.com/software-and-tools/usb-to-uart-bridge-vcp-drivers?tab=downloads).
      (On Linux/macOS the CP210x driver is already in the kernel — nothing to install.)
   2. **Plug the HC33 into your PC** with a USB-C cable.
   3. **Confirm a new serial port appeared:**
      - **Windows:** Device Manager → *Ports (COM & LPT)* → a new
        *Silicon Labs CP210x … (COMx)* entry.
      - **Linux:** `ls /dev/ttyUSB*` (or run `dmesg | tail` right after plugging
        in) — a new `/dev/ttyUSB0` shows up. If access is denied later, add your
        user to the `dialout` group: `sudo usermod -aG dialout $USER` (re-login).
      - **macOS:** `ls /dev/tty.usbserial-*` (or `/dev/tty.SLAB_USBtoUART`).
   4. Open the flasher in a **Chromium-based browser** (Chrome / Edge / Brave):
      **https://ontheview.github.io/mammotion-rc/** — pick the variant
      (**standard Wi-Fi** or **HaLow**), enter your Wi-Fi network details, and
      click Install.
   5. When done, plug the HC33 into the mower's USB-C port and **power the mower
      on**, so the proxy can bond to it over Bluetooth.

5. **Run the web server:**
   ```
   run-server.bat           # Windows
   ./run-server.sh          # Linux / macOS
   ```
   It prints a browsable `https://<your-machine-ip>:8443/` URL — open that from
   any device on the same LAN. (Ignore the `0.0.0.0` address Uvicorn logs — that's
   the bind-all address, not something you can open. The self-signed certificate
   also triggers a one-time browser security warning; accept it.)

6. **First connect — automatic onboarding.** You land on the onboarding page.
   Enter your **Mammotion account** email/password (the **secondary** account
   from step 0, not your main one); the server looks up the
   mowers on your account, broadcasts a UDP probe across your LAN, and
   **auto-pairs each mower with the matching HC33 proxy it finds** — writing
   `mowers.toml` and bringing everything live, no restart. (A proxy that hasn't
   seen its mower yet shows `bonded_name=none` — power the mower on and re-scan.)

## Using the web UI

Once connected, the browser page gives you:

- **Drive** — an on-screen joystick for live manual driving, with a server-side
  dead-man stop: the mower halts the instant you release the joystick or the
  connection drops.
- **Job control** — **Pause**, **Resume**, **Go Home** (return to the dock) and
  **Undock**, plus a big **STOP**.
- **Camera** — **Start / Stop** the live video feed from the mower.
- **Light** — toggle the mower's headlight on/off.
- **Compass** — a live heading readout so you know which way the mower is facing
  (handy when driving from the camera view).
- **Status** — current work mode, battery level, and any **fault / error**
  message, shown inline.
- **Mower picker & ⚙ Settings** — switch between mowers, or re-open onboarding to
  re-scan or edit.

> 🗺️ **Planned:** a Google Maps view as an alternative to the camera, showing the
> mower's exact position on the map in real time.

## Remote access (over the internet)

By default the web server is only reachable on your **local network**. To drive
the mower while you're away from home, expose it beyond the LAN with one of:

- **VPN (recommended)** — Tailscale, WireGuard, etc. Nothing is exposed to the
  public internet; you join your home network remotely and reach the server at
  its normal LAN address. Safest, and no router changes.
- **Reverse tunnel** — Cloudflare Tunnel or ngrok. Gives you a public HTTPS
  hostname with no router changes; works even behind CGNAT.
- **Port forwarding + dynamic DNS** — forward the server's port (default
  `8443`) on your router and use a DDNS hostname. Simplest, but it exposes the
  service directly to the internet.

> 🔒 If you don't set up a password, anyone who can reach the page can **drive
> your mower**. Keep the web-UI login enabled (you set it during `install.py`),
> and prefer a VPN or tunnel over raw port forwarding.

> 📷 **HTTPS is required for the camera.** The Agora video feed only works over a
> secure (HTTPS) connection — browsers block the WebRTC stream on plain HTTP.
> `install.py` serves HTTPS by default; for remote access use a **publicly
> trusted certificate** so browsers (and the camera) don't reject it.

Tunnel setup and how to obtain a trusted certificate are in
[`web-server/README.md`](web-server/README.md).

## Building from source

Most people never need this — the browser flasher ships prebuilt firmware and
`install.py` handles the server. Build from source only if you're modifying the
firmware.

See [`firmware/README.md`](firmware/README.md) for the complete build
instructions — prerequisites, the three build paths, and the HaLow library setup.

## Want to test it with a different ESP32 board?

You don't need an HC33 to try it. The firmware has a **standard-Wi-Fi build**
(`env:hc33-standard-wifi`) that's a plain BLE-central + Wi-Fi + TCP proxy, with
all the HaLow/Morse hardware bits gated off — so it runs on a generic ESP32
dev board over your normal 2.4 GHz Wi-Fi. The basics:

- **Any BLE-capable ESP32 works** — ESP32, ESP32-S3, ESP32-C3, ESP32-C6. The
  **ESP32-S2 does not** (it has no Bluetooth).
- Set `board = <your board>` in `firmware/platformio.ini`, use a partition
  table that fits its flash, and build the `hc33-standard-wifi` env.
- Put your 2.4 GHz SSID/password in `firmware/include/config.h`.

You lose only the HaLow long-range uplink and the mower-facing softAP — you'd
reach the device over your normal Wi-Fi instead. Full details (partition sizing,
flash flags, NimBLE version) are in
[**firmware/README.md → Other ESP32 boards**](firmware/README.md#other-esp32-boards-the-standard-wifi-build).

## Acknowledgements

A huge thank you to **Michael ([@mikey0000](https://github.com/mikey0000)), the
author of [PyMammotion](https://github.com/mikey0000/PyMammotion)** — this
project simply would not exist without his work. PyMammotion does all the heavy
lifting of speaking the mower's protocol; everything here is built on top of it.
If this project is useful to you, please go star PyMammotion too. 🙏

## Licensing

This project is licensed under **[GPL-3.0-or-later](LICENSE)**. That's required,
not chosen: it builds on two GPL-3.0 dependencies — **PyMammotion** (the
web-server) and **Heltec ESP_HaLow** (the firmware framework).

It also incorporates third-party components under their own licenses, including
**proprietary Morse Micro Wi-Fi HaLow firmware/driver blobs** under the Morse
Micro Binary Distribution License. **The prebuilt firmware binaries are therefore
not pure GPL** — they contain proprietary components that may be redistributed
only for use with Morse Micro HaLow hardware (e.g. the HC33), complete and
unmodified, with the BDL text retained.

Runtime browser components (Agora RTC SDK, nipplejs) are loaded from their CDNs
and are not bundled or relicensed by this project.

See **[THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md)** for the full breakdown.

> Not legal advice. For certainty about redistributing the compiled firmware
> (which embeds the Morse Micro blobs), confirm with Morse Micro.

## Contact

Questions, bugs, or ideas? Please
**[open a GitHub issue](../../issues)** — that's the best place to reach me
about this project.

## Disclaimer

Independent community project — not affiliated with, endorsed by, or supported by
Mammotion or Heltec. "Mammotion" and "Luba" are trademarks of their respective
owners. Use at your own risk, including compliance with local radio regulations.
