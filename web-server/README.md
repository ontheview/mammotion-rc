# Luba Remote web server

FastAPI app that drives a Luba 2X mower through the HC33 BLE proxy + the
existing PyMammotion library.  Browse to it from any device on the same LAN
to drive the mower with an on-screen joystick and watch the camera feed.

## Layout

- `app.py` — single-file FastAPI app: lifespan builds the `DeviceHandle`s and
  connects each `HC33ProxyTransport`; REST + WebSocket endpoints route
  commands; server-side joystick dead-man (500 ms) stops the mower if the
  browser stalls.
- `static/` — single-page browser UI (HTML + JS + CSS), AgoraRTC + nipplejs
  loaded from CDN.

## Install

One command sets everything up. The only prerequisite is Python 3.13 or 3.14
already installed (PyMammotion 0.8.x requires `>=3.13,<3.15`):

```
python install.py          # Windows  (or: py -3.14 install.py)
python3 install.py         # Linux / macOS
```

It creates a virtualenv (`./.venv`), `pip install`s PyMammotion 0.8.9 +
FastAPI/uvicorn (our HC33 transport ships in this repo — `hc33_proxy.py` — so
upstream PyMammotion is used unpatched), generates a self-signed TLS certificate,
prompts for a web-UI login password (and, optionally, your Mammotion account),
then writes `run-server.bat` / `run-server.sh` with every path baked in — nothing
to hand-edit. It is idempotent: re-run it any time to repair or upgrade.

Scriptable / non-interactive, and bring-your-own-cert:

```
python install.py --non-interactive --web-password 's3cret'
python install.py --cert-mode existing --cert /path/fullchain.pem --key /path/privkey.pem
```

- **Self-signed cert** (default) is generated cross-platform by `gen_cert.py`
  (no `openssl`); its SANs cover localhost, this host's name, and its LAN IP.
  Regenerate after a LAN-IP change with `install.py --force-cert`.
- **Publicly-trusted cert:** obtain it with `renew-cert.ps1` (Windows) or
  certbot/acme.sh, then `install.py --cert-mode existing --cert … --key …`.

See `python install.py --help` for all flags (venv location, host/port, etc.).

## Remote access & trusted certificate

The server is LAN-only by default. For reaching it from outside your network,
see the [Remote access overview](../README.md#remote-access-over-the-internet)
in the root README (VPN / reverse tunnel / port forwarding). Two web-server
specifics below.

### A hostname for your home (Dynamic DNS)

If you go the **port-forwarding** route, you reach the server at your home's
public IP — but most ISPs hand out a **dynamic** IP that changes periodically,
so you can't rely on a bare address. Use **Dynamic DNS** to get a stable
hostname (e.g. `home.example.com`) that automatically follows your changing IP:

- Most routers have a built-in DDNS client (Settings → Dynamic DNS) supporting
  providers like No-IP, DuckDNS, Cloudflare, or your own domain's host.
- Point a hostname at your connection; the router keeps its record updated.
- That hostname is what you browse to **and** what the TLS certificate below is
  issued for — they must match, or browsers reject the cert.

(VPN and reverse-tunnel setups don't need this — Tailscale gives you a name, and
a tunnel provider hands you a hostname.)

### Why a publicly-trusted certificate

The default self-signed cert works perfectly well — including the camera. The
Agora video feed is WebRTC, which needs a **secure context (HTTPS)**, and a
self-signed cert satisfies that once the browser has accepted it. So the *only*
reason to bother with a publicly-trusted cert is convenience: browsers stop
showing the security warning on every visit (and you avoid having to click
through it on each new device).

All the options below use the ACME **DNS-01** challenge, which proves domain
control by writing a DNS TXT record instead of opening ports 80/443 — ideal when
only `8443` is forwarded. DNS-01 is provider-agnostic: any DNS host with an ACME
plugin works (Posh-ACME supports ~100; certbot/acme.sh have their own).

### Example: Windows + cPanel DNS (included script)

`renew-cert.ps1` is an **example script from my own setup** — it obtains and
auto-renews a **Let's Encrypt** cert via DNS-01 using the **cPanel** Posh-ACME
plugin. It's cPanel-specific *only* in the plugin it calls; the overall approach
is the same for any provider (see below). One-time:

1. `Install-Module -Name Posh-ACME -Scope CurrentUser`
2. Edit the `EDIT THESE` block (domain, contact email, cPanel URL / user / API
   token).
3. Run it once interactively, then schedule it (see the notes at the bottom of
   the script) so renewals stay automatic.

It writes `cert.pem` / `key.pem` to the folder the server already loads them
from — no `install.py` re-run needed.

**Different DNS provider on Windows?** Keep the script but swap the plugin:
change `-Plugin cPanel` and the `$pArgs` hashtable to your provider's Posh-ACME
plugin and its arguments (`Get-PAPlugin` lists them).

### Linux / macOS (any provider)

Use `certbot` or `acme.sh` with the DNS-01 plugin for your DNS host, then hand
the files to the server:

```
python install.py --cert-mode existing \
  --cert /etc/letsencrypt/live/<domain>/fullchain.pem \
  --key  /etc/letsencrypt/live/<domain>/privkey.pem
```

Note uvicorn does **not** hot-reload certificates — restart the server after a
renewal.

## Configuration

### First run — automatic onboarding

> ⚠️ **Use a secondary Mammotion account, never your main one.** Signing in here
> can log your phone app out, and heavy/automated use risks the account being
> temporarily blocked. Create a separate Mammotion account and share your mower
> with it from the app (your device → **Share** / **Device sharing**), then use
> that account's credentials below.

Start the server with no `mowers.toml` and browse to it: you land on the
onboarding page (`/onboarding`).  It:

1. Asks for your Mammotion email/password (stored in `secrets.toml`, gitignored).
2. Looks up the mowers on your account (RTK base stations are filtered out)
   and their `iot_id`s.
3. Broadcasts a UDP probe on the LAN; every HC33 replies with its `chip_id`,
   `variant`, proxy port, and the BLE name of the mower it's bonded to.
4. Auto-pairs each cloud mower with the proxy whose `bonded_name` matches, then
   writes `mowers.toml` and brings the handles up live — no restart.

A proxy showing `bonded_name=none` hasn't seen its mower yet: power on the
mower and re-scan.  Cloud mowers with no proxy can be added by typing a host
manually.  The **⚙ Settings** link in the main UI re-opens this page any time
to re-scan or edit.

> Discovery needs the firmware built with `discovery.cpp` (UDP responder on
> port 9878).  It works on both the HaLow and standard-wifi variants.

### Schema (written by onboarding; hand-editable)

`mowers.toml` — roster, safe to commit:

```toml
[[mower]]
name      = "Luba-XXXXXXXX"
hc33_host = "192.168.1.54"
hc33_port = 9876                              # optional, default 9876
iot_id    = "YOUR_IOT_ID"      # optional — omit to disable camera
```

`secrets.toml` — account creds, **gitignored**, used for cloud login (camera)
and the ~monthly token refresh:

```toml
email    = "you@example.com"
password = "..."
```

Credentials still fall back to `EMAIL` / `PASSWORD` env vars (and the parent
PyMammotion `.env`) if `secrets.toml` is absent, so existing setups keep
working.

### Web UI password (HTTP Basic)

The whole UI (and the joystick WebSocket) can be put behind an HTTP Basic login.
Add **`web_password`** (and optionally **`web_username`**) to `secrets.toml` —
note these are *separate* keys, not part of the Mammotion `email`/`password`:

```toml
email    = "you@example.com"
password = "..."
web_username = "admin"          # optional, defaults to "luba"
web_password = "your-secret"  # required to enable the gate
```

- Only `web_password` is required; omitting `web_username` defaults it to `luba`.
- Values are TOML basic strings (double quotes); escape any `"` or `\`.
- These keys are **preserved across onboarding re-saves** — re-running onboarding
  won't wipe your web login.
- Leaving `web_password` unset/empty runs the server **unprotected** (it logs a
  warning on startup). When set, the log shows
  `web UI protected by HTTP Basic (username='admin')`.

Alternatively set them via environment variables, which **override** the file —
note the env names differ from the TOML keys:

| purpose       | `secrets.toml` key | environment variable |
|---------------|--------------------|----------------------|
| username      | `web_username`     | `LUBA_WEB_USERNAME`  |
| password      | `web_password`     | `LUBA_WEB_PASSWORD`  |

> After changing the password, **hard-refresh** the browser tab (Ctrl-Shift-R, or
> pull-to-refresh on mobile). The authenticated page load plants an auth cookie
> that the joystick WebSocket needs — browsers don't forward Basic credentials to
> `wss://` upgrades, so without the fresh load the joystick won't connect.

The app-level `MAMMOTION_OAUTH2_*` / `ALIYUN_*` constants (which identify the
Mammotion *app*, not you) are **baked into `mammotion_creds.py`**, so no `.env`
is required for cloud login or device enumeration.  A real OS environment
variable still overrides them — handy if Mammotion ever rotates the app keys
(symptom: login fails with "Client id or secret error"); update
`mammotion_creds.py` in that case.

## Running

After `install.py`, just start the generated launcher (no venv activation — it
calls the venv's Python by absolute path):

```
run-server.bat        # Windows (double-click or from a terminal)
./run-server.sh       # Linux / macOS
```

It serves HTTPS on `0.0.0.0:8443` by default. Browse to
<https://your-laptop-ip:8443/> from a phone on the same WiFi (accept the
self-signed-cert prompt the first time).

For development with auto-reload you can bypass the launcher:

```
.venv/bin/python -m uvicorn app:app --host 0.0.0.0 --port 8443 \
  --ssl-keyfile key.pem --ssl-certfile cert.pem --reload
```

## What works

- Drag the joystick anywhere on its zone to drive.  Releasing centres it →
  server emits `stop_and_not_save_task`.  Forward/back is **inverted** in
  the firmware to match this Luba 2X's wiring.
- **Start Camera** lazily logs into the Mammotion cloud, fetches an Agora
  stream token, sends `device_agora_join_channel_with_position(1)` over the
  BLE proxy, then joins the channel from the browser.
- **Stop Camera** sends `…with_position(0)` and leaves the Agora channel.
  The cloud login stays cached so re-starting the camera is fast.
- Pause / Resume / Go Home / STOP REST buttons.

## Safety

Two watchdogs cooperate:

1. **Server-side joystick dead-man** (`app.py::deadman()`): if the WebSocket
   stops receiving frames for 500 ms, emit `stop_and_not_save_task`.
2. **Firmware-side TCP idle watchdog** (`firmware/src/tcp_proxy.cpp`): if
   no TCP frame arrives at the HC33 for 30 s, the firmware closes the socket
   which drops the BLE link — final safety stop for silent network failures.
