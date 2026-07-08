"""Luba remote-control web server.

Browses to the page → drives the mower over WebSocket joystick + REST actions,
all routed through HC33ProxyTransport to the HC33 firmware over WiFi/HaLow.

Camera is optional and starts OFF.  Click "Start Camera" to:
  1. Lazily log into the Mammotion cloud (HTTP only — no MQTT, no device
     registration).
  2. Fetch a fresh Agora stream token.
  3. Send the explicit `device_agora_join_channel_with_position(1)` to the
     mower over BLE so it starts publishing.
  4. Return the token to the browser, which subscribes via Agora Web SDK.

Single-process, single-file.  Use uvicorn directly:

    uvicorn app:app --host 0.0.0.0 --port 8000 --reload

or just `python app.py` for the same with --reload disabled.
"""

from __future__ import annotations

import asyncio
import contextlib
import logging
import socket
import sys
import time
from pathlib import Path

# Must run before any asyncio loop is created.
if sys.platform == "win32":
    asyncio.set_event_loop_policy(asyncio.WindowsSelectorEventLoopPolicy())

# MUST precede every pymammotion import: it sets the app-level OAuth/Aliyun
# constants as env defaults, and pymammotion.const reads os.environ at import
# time.  Imported only for that import-time side effect.
import mammotion_creds  # noqa: F401,E402  (side-effecting; keep before pymammotion)

from fastapi import Body, FastAPI, HTTPException, WebSocket, WebSocketDisconnect
from fastapi.responses import FileResponse
from fastapi.staticfiles import StaticFiles

from pymammotion.aliyun.cloud_gateway import CloudIOTGateway
from pymammotion.data.model.device import MowingDevice
from pymammotion.device.handle import DeviceHandle
from pymammotion.http.http import MammotionHTTP
from pymammotion.transport.base import TransportAvailability
from pymammotion.utility.device_type import DeviceType

# Local module (ships with the web-server, not with PyMammotion): our TCP-over-
# HC33 transport that subclasses pymammotion's Transport.  Keeping it here lets
# us install a clean, unpatched upstream PyMammotion.
from hc33_proxy import HC33ProxyTransport

import persist
from discover import discover, poke_proxy

_LOGGER = logging.getLogger("luba_web")

# ── Mower configuration ──────────────────────────────────────────────────────
# The roster lives in mowers.toml (sibling to this file), normally written by
# the onboarding flow.  An empty/missing file means "not yet onboarded" — the
# server still starts, serves the onboarding page, and builds handles live once
# the user saves.  See persist.py for the schema and secrets handling.
#
# MOWERS is mutated in place (never rebound) so handlers holding the reference
# see live updates after onboarding/settings saves.
MOWERS: list[dict] = persist.load_mowers()

# Joystick → command mapping thresholds.  Inputs below DEAD_ZONE are treated
# as "centered" (= stop).  Forward/back wins over left/right when both axes
# are active (bang-bang cardinal motion for MVP — diagonal mixing later).
DEAD_ZONE = 0.15
# Optional ceiling on stick magnitude → command magnitude.  The mower expects
# the `linear` / `angular` arguments in [0.0, 1.0]; values > 1 produce huge
# internal speeds via `get_percent(value*100)`.  Keep <= 1.0.  Drop this below
# 1 if full deflection is too fast for comfort.
MAX_LINEAR  = 1.0
MAX_ANGULAR = 1.0

# ── Runtime state ────────────────────────────────────────────────────────────
class State:
    handles:    dict[str, DeviceHandle] = {}
    transports: dict[str, HC33ProxyTransport] = {}
    http:       MammotionHTTP | None = None     # lazy-built on first camera request / onboarding login
    cloud_client: CloudIOTGateway | None = None  # Aliyun gateway, cached across scans
    error_codes: dict | None = None             # cloud error-code table (code str -> ErrorInfo); None=unfetched, {}=unavailable
    # Onboarding-only: creds captured at the login step, used to write
    # secrets.toml on save.  Cleared after save.
    onboard_email:    str | None = None
    onboard_password: str | None = None
    # Per-mower auto-reconnect bookkeeping (see _watchdog / _auto_reconnect).
    auto_retrying:  dict[str, bool] = {}        # True while a retry sequence is mid-flight
    auto_gave_up:   dict[str, bool] = {}        # True after retries exhausted — manual reconnect required
    manual_op:      dict[str, bool] = {}        # True while /api/reconnect is running, suppresses watchdog
    watchdog_tasks: dict[str, asyncio.Task] = {}
    # Open joystick WebSockets, tracked so the lifespan shutdown can close them.
    # Without this, uvicorn hangs in "Waiting for connections to close" because
    # the joystick handler blocks forever in ws.receive_json() until the browser
    # tab closes — which Ctrl-C on Windows + selector loop can't interrupt.
    websockets: set[WebSocket] = set()


state = State()


# Backoff schedule for auto-reconnect on unexpected drops.  Five attempts total;
# total wall-clock window ≈ 48 s before giving up.  Once exhausted, the user
# must hit Reconnect — we don't pester a closed browser indefinitely.
AUTO_RECONNECT_DELAYS = [1, 2, 5, 10, 30]


async def _auto_reconnect(name: str) -> None:
    """Run the retry sequence for one mower after an unexpected transport drop."""
    state.auto_retrying[name] = True
    state.auto_gave_up[name]  = False
    t = state.transports[name]
    h = state.handles[name]
    try:
        for i, delay in enumerate(AUTO_RECONNECT_DELAYS, 1):
            await asyncio.sleep(delay)
            _LOGGER.info("auto-reconnect %s: attempt %d/%d", name, i, len(AUTO_RECONNECT_DELAYS))
            try:
                with contextlib.suppress(Exception):
                    await t.disconnect()
                await t.connect()
                await h.start()
                with contextlib.suppress(Exception):
                    await h.request_report_snapshot()
                _LOGGER.info("auto-reconnect %s: succeeded on attempt %d", name, i)
                return
            except Exception as exc:  # noqa: BLE001
                _LOGGER.warning("auto-reconnect %s: attempt %d failed: %s", name, i, exc)
        _LOGGER.error("auto-reconnect %s: gave up after %d attempts — manual Reconnect required",
                      name, len(AUTO_RECONNECT_DELAYS))
        state.auto_gave_up[name] = True
    finally:
        state.auto_retrying[name] = False


async def _watchdog(name: str) -> None:
    """Watch one transport for unexpected drops and fire the retry sequence.

    Triggers only on a CONNECTED → DISCONNECTED transition.  Skips while
    `manual_op` is set (the /api/reconnect handler owns the lifecycle in that
    window) and while a retry sequence is already running.
    """
    t = state.transports[name]
    last = t.availability
    ticks = 0
    while True:
        try:
            await asyncio.sleep(2.0)
        except asyncio.CancelledError:
            return
        try:
            current = t.availability
            unexpected_drop = (
                current == TransportAvailability.DISCONNECTED
                and last == TransportAvailability.CONNECTED
                and not state.manual_op.get(name, False)
                and not state.auto_retrying.get(name, False)
            )
            if unexpected_drop:
                _LOGGER.warning("watchdog %s: unexpected drop — starting auto-reconnect", name)
                asyncio.create_task(_auto_reconnect(name), name=f"auto-reconnect-{name}")
            last = current
            # Refresh the headlight state every ~6 s while connected.  The mower
            # auto-offs the light after a while and only a get_car_light response
            # updates lamp_info, so we actively re-probe; the answer lands async
            # and /api/status serves it on the next poll.  Piggybacking the 2 s
            # watchdog tick avoids a second timer.
            ticks += 1
            if current == TransportAvailability.CONNECTED and ticks % 3 == 0:
                h = state.handles.get(name)
                if h is not None:
                    with contextlib.suppress(Exception):
                        await h.send_raw(h.commands.get_car_light(1126))
            # Refresh the fault log every ~60 s (HA polls get_errors at the same
            # cadence).  The device only populates errors.err_code_list in reply
            # to get_error_code, so without this the buffer stays empty and no
            # fault ever shows.  ticks%30==1 → first poll ~2 s after connect, so
            # the status chip has data almost immediately.
            if current == TransportAvailability.CONNECTED and ticks % 30 == 1:
                h = state.handles.get(name)
                if h is not None:
                    with contextlib.suppress(Exception):
                        await h.send_raw(h.commands.get_error_code())
                        await h.send_raw(h.commands.get_error_timestamp())
        except Exception:  # noqa: BLE001
            _LOGGER.exception("watchdog %s tick crashed", name)


# ── Handle lifecycle helpers (shared by lifespan + onboarding apply) ──────────
async def _build_and_connect(cfg: dict) -> None:
    """Build a transport + handle for one mower, connect, and arm its watchdog.

    Connect failures are non-fatal: the handle/watchdog are still registered so
    a later Reconnect (or auto-reconnect) can recover without a server restart.
    """
    name = cfg["name"]
    transport = HC33ProxyTransport(device_id=name, host=cfg["hc33_host"], port=cfg["hc33_port"])
    handle = DeviceHandle(
        device_id=name,
        device_name=name,
        initial_device=MowingDevice(name=name),
        iot_id=cfg["iot_id"],
        ble_transport=transport,
        prefer_ble=True,
    )
    state.handles[name] = handle
    state.transports[name] = transport
    try:
        await transport.connect()
        await handle.start()
        _LOGGER.info("connected to HC33 for %s at %s:%d", name, cfg["hc33_host"], cfg["hc33_port"])
    except Exception:
        _LOGGER.exception("initial HC33 connect for %s failed — will retry on first command", name)
    state.watchdog_tasks[name] = asyncio.create_task(_watchdog(name), name=f"watchdog-{name}")


async def _teardown_all(*, close_ws: bool) -> None:
    """Tear down every handle/transport/watchdog.  Used on shutdown and before
    re-applying a new roster from onboarding/settings."""
    if close_ws:
        for ws in list(state.websockets):
            with contextlib.suppress(Exception):
                await ws.close(code=1001, reason="server reconfigured")
    for task in state.watchdog_tasks.values():
        task.cancel()
    for task in state.watchdog_tasks.values():
        with contextlib.suppress(asyncio.CancelledError, Exception):
            await task
    for h in state.handles.values():
        with contextlib.suppress(Exception):
            await h.stop()
    for t in state.transports.values():
        with contextlib.suppress(Exception):
            await t.disconnect()
    state.watchdog_tasks.clear()
    state.handles.clear()
    state.transports.clear()
    state.auto_retrying.clear()
    state.auto_gave_up.clear()
    state.manual_op.clear()


async def _apply_mowers(new_mowers: list[dict]) -> None:
    """Swap the live roster: tear down current handles, rebuild from
    *new_mowers*, and update the module-level MOWERS in place."""
    await _teardown_all(close_ws=True)
    MOWERS[:] = new_mowers
    for cfg in MOWERS:
        await _build_and_connect(cfg)


def _served_port(default: int = 8443) -> int:
    """Port uvicorn was told to serve on, parsed from the launch command
    (`... --port 8443`), so the startup banner can show a real URL.  Falls back
    to the run-server default when not present on the command line."""
    argv = sys.argv
    if "--port" in argv:
        try:
            return int(argv[argv.index("--port") + 1])
        except (ValueError, IndexError):
            pass
    return default


def _primary_lan_ip() -> "str | None":
    """Best-effort primary LAN IPv4 (the egress source IP).  A UDP connect()
    sends no packet — it just makes the OS pick the outbound interface — then we
    read back the local address.  Returns None if it can't be determined."""
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        try:
            s.connect(("8.8.8.8", 80))
            return s.getsockname()[0]
        except OSError:
            return None


# ── Lifespan: build BLE-only handles, connect transports ─────────────────────
@contextlib.asynccontextmanager
async def lifespan(app: FastAPI):
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(name)s: %(message)s")
    if not MOWERS:
        _LOGGER.info("no mowers configured — starting in onboarding mode")
    for cfg in MOWERS:
        await _build_and_connect(cfg)

    # Uvicorn logs its bind address as https://0.0.0.0:PORT — that's "listen on
    # every interface", not a URL you can open.  Print the actual LAN address so
    # users have something browsable to copy/paste.
    ip = _primary_lan_ip() or "<this-machine-ip>"
    _LOGGER.info(
        "web UI ready — open  https://%s:%d/  from any device on this LAN "
        "(Uvicorn's 0.0.0.0 line above is the bind address, not a URL to open)",
        ip, _served_port(),
    )

    yield

    await _teardown_all(close_ws=True)


app = FastAPI(lifespan=lifespan)


# ── Authentication: signed cookie + HTML login form ───────────────────────────
# A pure-ASGI middleware so it covers EVERY route — pages, REST, static mount,
# and the joystick WebSocket — in one place.  Disabled (pass-through) when no
# password is set, so an un-onboarded install isn't accidentally locked out.
# Use over TLS only (run-server.bat serves https on 8443).
#
# Session = a signed cookie.  When it's missing/invalid, browser navigations are
# redirected to an HTML login form (/login) rather than the native HTTP Basic
# dialog — the form integrates with Chrome/iOS password managers (save +
# autofill), which the Basic dialog never did, especially on iOS where a
# self-signed cert stops the cookie persisting across restarts.  A `Basic`
# Authorization header is still accepted for scripted clients but never
# advertised (no 401 challenge), so it won't pop a browser dialog.
#
# WebSocket gotcha: browsers (notably Chrome) do NOT attach an `Authorization`
# header to a `wss://` upgrade, so gating the socket on Basic alone closes the
# joystick.  The cookie covers it: browsers DO send cookies on same-origin WS
# upgrades, and the page only loads when authenticated, so the cookie is present.
import base64
import hashlib
import hmac
import secrets as _secrets
from urllib.parse import parse_qs

from starlette.requests import Request
from starlette.responses import HTMLResponse, PlainTextResponse, RedirectResponse

_WEB_USER, _WEB_PASS = persist.load_web_auth()
_AUTH_COOKIE = "luba_auth"
# Persist the auth cookie so a first Basic login is remembered across browser
# restarts (iOS Chrome/Safari drop session cookies on close, re-prompting Basic).
_AUTH_COOKIE_MAX_AGE = 90 * 24 * 3600  # 90 days
# Cookie value derived from the password (HMAC, not the password itself).  Stable
# across restarts so an open tab's cookie keeps working; rotates if you change
# the password.  None when auth is disabled.
_WS_TOKEN = (
    hmac.new(_WEB_PASS.encode("utf-8"), b"luba-ws-auth", hashlib.sha256).hexdigest()
    if _WEB_PASS else None
)


def _set_auth_cookie(resp) -> None:
    """Attach the persistent auth cookie to a Starlette response.  No Secure flag
    on purpose — a self-signed cert is a non-secure context, which can stop the
    browser storing a Secure cookie; the value is an HMAC, not the password."""
    resp.set_cookie(
        _AUTH_COOKIE,
        _WS_TOKEN or "",
        max_age=_AUTH_COOKIE_MAX_AGE,
        httponly=True,
        samesite="strict",
        path="/",
    )


class BasicAuthMiddleware:
    def __init__(self, app, username: str, password: str | None, token: str | None):
        self.app = app
        self.username = username
        self.password = password
        self.token = token

    async def __call__(self, scope, receive, send):
        if not self.password or scope["type"] not in ("http", "websocket"):
            await self.app(scope, receive, send)
            return
        # The login form + its POST handler must be reachable without auth, else
        # the unauthenticated redirect below would loop.
        if scope["type"] == "http" and scope.get("path") == "/login":
            await self.app(scope, receive, send)
            return
        headers = dict(scope.get("headers") or [])
        ok = self._basic_ok(headers) or self._cookie_ok(headers)

        if scope["type"] == "websocket":
            if ok:
                await self.app(scope, receive, send)
            else:
                # Reject the handshake before accept → uvicorn returns HTTP 403.
                await send({"type": "websocket.close", "code": 1008})
            return

        if not ok:
            # Browser navigations → the login form.  API/asset/XHR → a plain 401
            # with NO WWW-Authenticate header, so no native Basic dialog pops;
            # the client treats it as "session expired" and can send the user to
            # /login.  (On first load the GET / navigation is what gets gated, so
            # the SPA never even boots unauthenticated.)
            accept = headers.get(b"accept", b"")
            if scope.get("method") == "GET" and b"text/html" in accept:
                resp = RedirectResponse("/login", status_code=303)
            else:
                resp = PlainTextResponse("Authentication required", status_code=401)
            await resp(scope, receive, send)
            return

        # Authenticated HTTP: drop the auth cookie so the WS upgrade can carry it.
        # NOTE: Secure flag omitted on purpose — self-signed certs make the origin
        # a non-secure context, which can stop the browser storing a Secure cookie.
        # The token is an HMAC (not the password); served HTTPS-only, so low risk.
        cookie = (
            f"{_AUTH_COOKIE}={self.token}; Path=/; Max-Age={_AUTH_COOKIE_MAX_AGE}; "
            "HttpOnly; SameSite=Strict"
        ).encode("latin-1")

        async def send_with_cookie(message):
            if message["type"] == "http.response.start":
                message.setdefault("headers", []).append((b"set-cookie", cookie))
            await send(message)

        await self.app(scope, receive, send_with_cookie)

    def _basic_ok(self, headers: dict) -> bool:
        raw = headers.get(b"authorization", b"")
        if not raw.startswith(b"Basic "):
            return False
        try:
            user, _, pw = base64.b64decode(raw[6:]).decode("utf-8").partition(":")
        except Exception:  # noqa: BLE001 — any malformed header = unauthorized
            return False
        # Constant-time compares to avoid leaking length/contents via timing.
        return (
            _secrets.compare_digest(user, self.username)
            and _secrets.compare_digest(pw, self.password)
        )

    def _cookie_ok(self, headers: dict) -> bool:
        raw = headers.get(b"cookie", b"").decode("latin-1")
        for part in raw.split(";"):
            k, _, v = part.strip().partition("=")
            if k == _AUTH_COOKIE:
                return _secrets.compare_digest(v, self.token or "")
        return False


app.add_middleware(
    BasicAuthMiddleware, username=_WEB_USER, password=_WEB_PASS, token=_WS_TOKEN
)


# Never let the browser serve stale UI/API from cache.  iOS Safari/Chrome in
# particular cling to old app.js/index.html and silently break after an update
# (e.g. empty mower list until you open an incognito tab).
@app.middleware("http")
async def _no_store(request, call_next):
    response = await call_next(request)
    response.headers["Cache-Control"] = "no-store"
    return response
if _WEB_PASS:
    _LOGGER.info("web UI protected by HTTP Basic (username=%r)", _WEB_USER)
else:
    _LOGGER.warning(
        "no web password set (LUBA_WEB_PASSWORD env or web_password in "
        "secrets.toml) — the web UI is UNPROTECTED"
    )


# ── Static files ─────────────────────────────────────────────────────────────
HERE = Path(__file__).parent
app.mount("/static", StaticFiles(directory=HERE / "static"), name="static")


def _is_onboarded() -> bool:
    """Onboarded once at least one mower is configured."""
    return bool(MOWERS)


def _login_html(error: bool = False) -> str:
    """Standalone (inline-styled, no /static deps) sign-in page.  A real <form>
    so Chrome / iOS password managers offer to save and later autofill it."""
    err = '<p class="err">Wrong username or password.</p>' if error else ""
    user = _WEB_USER or ""
    return f"""<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Luba Remote — Sign in</title>
<style>
  body {{ margin:0; min-height:100vh; display:flex; align-items:center;
         justify-content:center; background:#111; color:#eee;
         font-family:system-ui,-apple-system,sans-serif; }}
  form {{ background:#1b1b1b; padding:28px 26px; border-radius:12px;
          width:min(90vw,320px); box-shadow:0 6px 24px rgba(0,0,0,.5); }}
  h1 {{ margin:0 0 18px; font-size:20px; font-weight:600; }}
  label {{ display:block; font-size:13px; color:#aaa; margin:12px 0 4px; }}
  input {{ width:100%; box-sizing:border-box; padding:10px; font-size:16px;
           border:1px solid #333; border-radius:8px; background:#222; color:#eee; }}
  button {{ width:100%; margin-top:20px; padding:11px; font-size:16px; border:0;
            border-radius:8px; background:#2dd47a; color:#063; font-weight:600; }}
  .err {{ color:#f77; font-size:13px; margin:0 0 8px; }}
</style>
</head>
<body>
<form method="post" action="/login" autocomplete="on">
  <h1>Luba Remote</h1>
  {err}
  <label for="u">Username</label>
  <input id="u" name="username" value="{user}" autocomplete="username"
         autocapitalize="none" autocorrect="off" spellcheck="false" required>
  <label for="p">Password</label>
  <input id="p" name="password" type="password" autocomplete="current-password"
         required>
  <button type="submit">Sign in</button>
</form>
</body>
</html>"""


@app.get("/login")
async def login_form(request: Request):
    # Auth disabled, or already signed in → straight to the app.
    if not _WEB_PASS:
        return RedirectResponse("/", status_code=303)
    tok = request.cookies.get(_AUTH_COOKIE)
    if tok and _secrets.compare_digest(tok, _WS_TOKEN or ""):
        return RedirectResponse("/", status_code=303)
    return HTMLResponse(_login_html())


@app.post("/login")
async def login_submit(request: Request):
    # Parse the urlencoded form by hand to avoid a python-multipart dependency.
    data = parse_qs((await request.body()).decode("utf-8", "replace"))
    user = (data.get("username", [""])[0]).strip()
    pw = data.get("password", [""])[0]
    if (
        _WEB_PASS
        and _secrets.compare_digest(user, _WEB_USER)
        and _secrets.compare_digest(pw, _WEB_PASS)
    ):
        resp = RedirectResponse("/", status_code=303)
        _set_auth_cookie(resp)
        return resp
    return HTMLResponse(_login_html(error=True), status_code=401)


@app.get("/")
async def root():
    # First run (no roster yet) → send the user straight to onboarding.
    if not _is_onboarded():
        return FileResponse(HERE / "static" / "onboarding.html")
    return FileResponse(HERE / "static" / "index.html")


@app.get("/onboarding")
async def onboarding_page():
    """The onboarding/settings page.  Reachable any time (e.g. the Settings
    link in the main UI) to re-scan or edit the roster."""
    return FileResponse(HERE / "static" / "onboarding.html")


# ── Helpers ──────────────────────────────────────────────────────────────────
def _cfg(name: str) -> dict:
    cfg = next((m for m in MOWERS if m["name"] == name), None)
    if cfg is None:
        raise HTTPException(404, f"unknown mower {name!r}")
    return cfg


def _handle(name: str) -> DeviceHandle:
    h = state.handles.get(name)
    if h is None:
        raise HTTPException(404, f"unknown mower {name!r}")
    return h


async def _ensure_http() -> MammotionHTTP:
    """Return a logged-in MammotionHTTP, logging in from stored secrets if
    needed.  Used by the camera and by the onboarding scan (settings path)."""
    if state.http is not None and state.http.login_info is not None:
        return state.http
    email, password = persist.load_secrets()
    if not email or not password:
        raise HTTPException(
            503,
            "Cloud login required but no credentials are configured. "
            "Run onboarding to set your Mammotion email/password.",
        )
    http = MammotionHTTP()
    resp = await http.login_v2(email, password)
    if resp.code != 0:
        raise HTTPException(502, f"Mammotion login failed: {resp.msg!r}")
    state.http = http
    return http


# ── REST: list, action, camera start/stop ────────────────────────────────────
@app.get("/api/mowers")
async def list_mowers():
    return [
        {"name": m["name"], "nickname": m.get("nickname"), "camera": m["iot_id"] is not None}
        for m in MOWERS
    ]


# Action name → (command method, kwargs).  Zero-arg commands have empty kwargs.
_ACTIONS = {
    "pause":      ("pause_execute_task",   {}),
    "resume":     ("resume_execute_task",  {}),
    "dock":       ("return_to_dock",       {}),
    "undock":     ("leave_dock",           {}),
    "stop":       ("stop_and_not_save_task", {}),
    "light-on":   ("set_car_manual_light", {"manual_ctrl": True}),
    "light-off":  ("set_car_manual_light", {"manual_ctrl": False}),
}


@app.post("/api/action/{name}/{action}")
async def post_action(name: str, action: str):
    h = _handle(name)
    entry = _ACTIONS.get(action)
    if entry is None:
        raise HTTPException(400, f"unknown action {action!r}")
    method, kwargs = entry
    cmd_bytes = getattr(h.commands, method)(**kwargs)
    await h.send_raw(cmd_bytes)
    # For a headlight toggle, immediately request a read-back instead of waiting
    # for the next ~6 s watchdog probe.  The response lands async and a fast
    # client-side poll (see action() in app.js) picks up the confirmed state —
    # so the toggle reflects real success/failure within ~1-2 s.
    if action in ("light-on", "light-off"):
        with contextlib.suppress(Exception):
            await h.send_raw(h.commands.get_car_light(1126))
    return {"ok": True}


def _current_orientation(h) -> "int | None":
    """Mower heading in degrees (0-359, geographic north = 0), or None if the
    mower hasn't reported a location yet.  Populated from RptDevLocation.real_toward
    by pymammotion (device.location.orientation)."""
    try:
        return int(h.snapshot.raw.location.orientation) % 360
    except (AttributeError, TypeError, ValueError):
        return None


# report_data.dev.sys_status (pymammotion WorkMode enum) → short human label for
# the UI status chip.  Only the values a running mower actually reports are
# spelled out; anything else falls back to "Mode <n>".  There is no "docked"
# mode — a mower sitting on the dock reports Charging (15) or, once full,
# Standby (11).
_MOWER_STATUS_LABELS = {
    0:  "Idle",
    1:  "Online",
    2:  "Offline",
    8:  "Disabled",
    10: "Starting up",
    11: "Standby",
    13: "Mowing",
    14: "Returning home",
    15: "Charging",
    16: "Updating",
    17: "Locked",
    19: "Paused",
    20: "Manual control",
    22: "Update complete",
    23: "Update failed",
    31: "Editing map",
    32: "Editing map",
    34: "Editing map",
    35: "Editing map",
    36: "Editing map",
    37: "Location error",
    38: "Off boundary",
    39: "Paused (charging)",
}

# A blocking fault stops the mower and drops it into Paused (MODE_PAUSE), so the
# UI only treats a code from the error log as a *live* fault in this state.  In
# any other state (Idle, Mowing, Returning, Charging, …) a code still sitting in
# the history ring is stale — the mower has moved on.
_MODE_PAUSE = 19


def _sys_status(h) -> "int | None":
    """The mower's raw sys_status (WorkMode) from the last report, or None."""
    try:
        return int(h.snapshot.raw.report_data.dev.sys_status)
    except (AttributeError, TypeError, ValueError):
        return None


def _last_error(h) -> int:
    """The mower's most-recent fault code (0 if none / not polled yet).

    errors.err_code_list is a 10-slot history ring paired with
    err_code_list_time; the entry with the newest timestamp is HA's "last
    error".  Populated by the watchdog's periodic get_error_code poll.  Codes
    are signed on the wire (e.g. -2801); the sign is normalised at lookup."""
    try:
        errors = h.snapshot.raw.errors
        pairs = [
            (int(t), int(c))
            for c, t in zip(errors.err_code_list, errors.err_code_list_time)
            if int(c) != 0
        ]
    except (AttributeError, TypeError, ValueError):
        return 0
    if not pairs:
        return 0
    return max(pairs)[1]  # code with the newest timestamp


async def _error_table() -> dict:
    """Cloud-fetched {code: ErrorInfo} table used to turn numeric mower fault
    codes into the same human text the Mammotion app / HA shows.  The device
    only sends numeric codes over BLE; the text lives in a CSV the cloud serves
    (MammotionHTTP.get_all_error_codes).  Fetched once and cached; returns {}
    (and caches it) when cloud login isn't configured or the fetch fails, so the
    UI degrades to bare numeric codes rather than erroring."""
    if state.error_codes is not None:
        return state.error_codes
    try:
        http = await _ensure_http()
        state.error_codes = await http.get_all_error_codes()
    except Exception as exc:  # no cloud creds, network blip, or API change
        _LOGGER.info("error-code table unavailable, showing numeric codes: %s", exc)
        state.error_codes = {}
    return state.error_codes


def _lookup_error(code: int, table: dict) -> "str | None":
    """HA-style human fault string for a code, e.g.
    "mcu: Lift sensor triggered, Please handle promptly", or None if code is 0.
    Format = "<module>: <en_implication>, <en_solution>" with empty parts
    dropped; falls back to "E<code>" if the row has no English text.  Codes are
    signed on the wire (e.g. -2801) while table keys are positive, so try abs()."""
    if not code:
        return None
    info = table.get(str(code)) or table.get(str(abs(code)))
    if info is None:
        return f"E{code}"
    module = (getattr(info, "module", "") or "").strip()
    impl = (getattr(info, "en_implication", "") or "").strip()
    soln = (getattr(info, "en_solution", "") or "").strip()
    body = ", ".join(p for p in (impl, soln) if p)
    if not body:
        return f"E{code}"
    return f"{module}: {body}" if module else body


@app.get("/api/status/{name}")
async def status(name: str):
    """Two-layer view of the link to a mower.

    `availability` reports the TCP/HaLow socket to the HC33 (transport layer).
    `mower_silent_s` reports how long since pymammotion last decoded a LubaMsg
    from the mower itself (BLE layer) — None means we've never heard from it
    since the handle was started, large numbers mean the BLE side is wedged
    even though the TCP socket is healthy.
    """
    _cfg(name)
    t = state.transports[name]
    h = state.handles[name]
    last = h.last_report_at  # monotonic seconds, 0.0 if never
    silent_s = None if last == 0.0 else max(0.0, time.monotonic() - last)
    # Battery telemetry from the last decoded report.  Same access path handle.py
    # uses in _device_mode(); stays None until the mower has reported once.
    battery = None
    charging = None
    try:
        dev = h.snapshot.raw.report_data.dev
        battery = int(dev.battery_val)
        charging = int(dev.charge_state) != 0
    except (AttributeError, TypeError, ValueError):
        pass
    # Headlight state.  The watchdog re-probes get_car_light(1126) every ~6 s,
    # so this tracks the firmware's auto-off.  Stays None until the mower has
    # answered at least once, so the UI keeps its optimistic guess rather than
    # snapping the toggle to a stale default right after connect.
    light_on = None
    try:
        light_on = bool(h.snapshot.raw.mower_state.lamp_info.manual_light)
    except AttributeError:
        pass
    # Mower work-state ("Mowing", "Charging", …) from sys_status, + the most
    # recent fault from the polled error log (see _last_error).  The error log is
    # a *history* ring — the live toapp_err_code push doesn't cross the HC33
    # proxy — so an old code persists there long after the mower recovers.  Only
    # surface it as a live fault when the mower is Paused (MODE_PAUSE): a blocking
    # fault stops the mower into that state, whereas in Idle/Mowing/Returning/
    # Charging/etc. a lingering code is stale history, not a current problem.
    # Fault text comes from the cloud table, only looked up when a code is present
    # so a healthy mower never triggers a cloud login.
    sys_status = _sys_status(h)
    mower_state_label = (
        None if sys_status is None
        else _MOWER_STATUS_LABELS.get(sys_status, f"Mode {sys_status}")
    )
    err_code = _last_error(h) if sys_status == _MODE_PAUSE else 0
    mower_error = _lookup_error(err_code, await _error_table()) if err_code else None
    # While the auto-reconnect watchdog is mid-retry the underlying transport
    # flickers DISCONNECTED↔CONNECTING; report a steady "connecting" to the UI
    # so the badge doesn't strobe red between attempts.
    if state.auto_retrying.get(name, False):
        availability = "connecting"
    else:
        availability = t.availability.name.lower()  # connected | connecting | disconnected
    return {
        "connected":      t.is_connected,
        "availability":   availability,
        "mower_silent_s": silent_s,
        "battery":        battery,    # 0..100, or None if not reported yet
        "charging":       charging,   # True while on the dock charging
        "light_on":       light_on,   # headlight state, or None if not probed yet
        "status":         mower_state_label,  # "Mowing"/"Charging"/… or None
        "error":          mower_error,        # human fault text, or None if no fault
        "auto_retrying":  state.auto_retrying.get(name, False),
        "auto_gave_up":   state.auto_gave_up.get(name, False),
        "orientation":    _current_orientation(h),  # heading in degrees, or None
    }


@app.get("/api/heading/{name}")
async def heading(name: str):
    """Lightweight heading poll for the camera compass overlay (~1 Hz while the
    camera is open).  Kept separate from /api/status so the fast poll stays cheap."""
    _cfg(name)
    h = state.handles[name]
    return {"orientation": _current_orientation(h)}


@app.post("/api/reconnect/{name}")
async def reconnect(name: str):
    """Re-run the lifespan connect for one mower.

    Useful when the HC33 was unreachable at startup (lifespan caught the
    exception, leaving the handle's queue/keepalive uninitialized) or when
    the link wedges and we want to force a clean re-handshake.  Both calls
    are idempotent.
    """
    _cfg(name)
    t = state.transports[name]
    h = state.handles[name]
    # manual_op suppresses the watchdog while we deliberately bounce the socket;
    # auto_gave_up is cleared so future drops can re-arm the retry sequence.
    state.manual_op[name] = True
    try:
        with contextlib.suppress(Exception):
            await t.disconnect()
        await t.connect()
        await h.start()
        # Probe the mower so /api/status starts seeing a non-null mower_silent_s
        # within a few seconds.  No-op if the BLE stream is already running.
        with contextlib.suppress(Exception):
            await h.request_report_snapshot()
        state.auto_gave_up[name] = False
        _LOGGER.info("reconnect for %s succeeded", name)
        return {"ok": True}
    finally:
        state.manual_op[name] = False


@app.post("/api/camera/{name}/start")
async def camera_start(name: str):
    cfg = _cfg(name)
    if cfg["iot_id"] is None:
        raise HTTPException(
            400,
            f"Mower {name!r} has no iot_id configured in mowers.toml — camera is unavailable.",
        )
    h = _handle(name)
    http = await _ensure_http()
    is_yuka = DeviceType.is_yuka(name)
    sub = await http.get_stream_subscription(cfg["iot_id"], is_yuka)
    if sub.code != 0 or sub.data is None:
        raise HTTPException(502, f"stream-subscription failed: {sub.msg!r}")
    # Diagnostic: is the channel encrypted this session?  If openEncrypt flips to
    # 1, an unconfigured web client receives undecryptable RTP (packets arrive,
    # zero frames assemble) while the native app — which holds the key — is fine.
    _LOGGER.info(
        "stream-subscription: openEncrypt=%r license=%r cameras=%r uid=%r channel=%r",
        sub.data.openEncrypt,
        sub.data.license,
        [(c.cameraId, bool(c.token)) for c in sub.data.cameras],
        sub.data.uid,
        sub.data.channelName,
    )
    # New-firmware Luba 2X doesn't auto-publish — send the explicit join over BLE.
    cmd = h.commands.device_agora_join_channel_with_position(enter_state=1)
    await h.send_raw(cmd)
    # Kick a continuous report stream so the camera compass gets live heading
    # (RptDevLocation) at ~1 Hz.  Stopped again in camera_stop.
    with contextlib.suppress(Exception):
        await h.send_raw(h.commands.get_report_cfg(count=0))
    d = sub.data
    return {
        "appid":       d.appid,
        "channelName": d.channelName,
        "token":       d.token,
        "uid":         d.uid,
    }


@app.post("/api/camera/{name}/stop")
async def camera_stop(name: str):
    h = _handle(name)
    cmd = h.commands.device_agora_join_channel_with_position(enter_state=0)
    await h.send_raw(cmd)
    # Stop the continuous report stream started in camera_start.
    with contextlib.suppress(Exception):
        await h.send_raw(h.commands.get_report_cfg_stop())
    return {"ok": True}


@app.post("/api/camera/{name}/refresh")
async def camera_refresh(name: str):
    """Ask the mower's encoder to emit a fresh keyframe (IDR).

    A browser that subscribes a beat after the mower starts publishing misses
    the initial keyframe; H.264 then can't decode anything until the next IDR,
    which the Luba won't send on its own.  The browser calls this once it is
    actually subscribed (and again on RECV_VIDEO_DECODE_FAILED) to unstick it.
    """
    h = _handle(name)
    cmd = h.commands.refresh_fpv()
    await h.send_raw(cmd)
    return {"ok": True}


# ── Onboarding / settings ─────────────────────────────────────────────────────
@app.get("/api/onboard/status")
async def onboard_status():
    """What the onboarding page needs to decide its initial state.

    The login step is shown on first run (not yet onboarded) regardless of
    whether EMAIL/PASSWORD env vars exist — setup should be explicit.  Once a
    roster is saved (onboarded), the page skips login and the server logs in
    from secrets.toml / env on demand.  `suggested_email` pre-fills the form
    when we can detect a likely address (from env or a prior secrets.toml).
    """
    email, _pw = persist.load_secrets()
    return {
        "onboarded":       _is_onboarded(),
        "logged_in":       state.http is not None and state.http.login_info is not None,
        "suggested_email": email or "",
        "mower_count":     len(MOWERS),
    }


@app.post("/api/onboard/login")
async def onboard_login(payload: dict = Body(...)):
    """Validate Mammotion credentials and cache the session.  Creds are held in
    memory until /api/onboard/save persists them to secrets.toml."""
    email = (payload.get("email") or "").strip()
    password = payload.get("password") or ""
    if not email or not password:
        raise HTTPException(400, "email and password are required")
    http = MammotionHTTP()
    try:
        resp = await http.login_v2(email, password)
    except Exception as exc:  # noqa: BLE001
        return {"ok": False, "error": f"login error: {exc}"}
    if resp.code != 0:
        return {"ok": False, "error": resp.msg or "login failed"}
    state.http = http
    state.cloud_client = None  # force a fresh Aliyun handshake for this session
    state.onboard_email = email
    state.onboard_password = password
    return {"ok": True}


async def _cloud_mowers() -> list[dict]:
    """Account's mowers (RTK base stations filtered out).

    Primary source is the Aliyun binding list (`list_binding_by_account`) — the
    authoritative list for Luba/Yuka devices, where `/device-server/v1/device/list`
    routinely comes back empty.  Falls back to that endpoint if the gateway
    handshake yields nothing.  Each device carries name + iot_id + product_key,
    so RTK filtering uses both.
    """
    http = await _ensure_http()
    # (device_name, iot_id, product_key, nickname).  nickname is the friendly
    # name set in the Mammotion app; None when unset or via the fallback path.
    devices: list[tuple[str, str, str, str | None]] = []
    gateway_error: str | None = None

    try:
        cloud = state.cloud_client
        if cloud is None:
            # One-time Aliyun handshake (region → connect → oauth → aep →
            # session).  Needs the MAMMOTION_OAUTH2_* / ALIYUN_* constants from
            # the PyMammotion env/.env; raises clearly if they're missing.
            cloud = CloudIOTGateway(http)
            # get_region + login_by_oauth authenticate to Aliyun with
            # login_info.authorization_code as a THIRD_AUTHCODE.  That code is
            # short-lived: when state.http was minted earlier (e.g. at camera
            # start) the cached code may have expired by scan time, and Aliyun
            # then rejects the region lookup with HTTP 500
            # "openId isEmpty,result_code=100002".  Re-mint a fresh auth code so
            # the handshake always carries a live one; this also refreshes the
            # access token first if it's near expiry.  Non-fatal — if the refresh
            # itself fails we fall through and let the handshake surface the error.
            try:
                await http.refresh_authorization_token()
            except Exception as exc:  # noqa: BLE001
                _LOGGER.warning("auth-code refresh before Aliyun handshake failed: %s", exc)
            login_info = http.login_info
            country = login_info.userInformation.domainAbbreviation
            if cloud.region_response is None:
                await cloud.get_region(country)
            await cloud.connect()
            await cloud.login_by_oauth(country)
            await cloud.aep_handle()
            await cloud.session_by_auth_code()
            state.cloud_client = cloud
        await cloud.list_binding_by_account()
        resp = cloud.devices_by_account_response
        if resp and resp.data and resp.data.data:
            for d in resp.data.data:
                devices.append((d.device_name, d.iot_id, d.product_key or "", d.nick_name or None))
    except Exception as exc:  # noqa: BLE001
        gateway_error = str(exc)
        _LOGGER.warning("Aliyun device enumeration failed: %s", exc)

    if not devices:
        # Fallback: owned device-server list (name + iot_id, no product_key).
        try:
            resp = await http.get_user_device_list()
            for d in (resp.data or []):
                devices.append((d.device_name, d.iot_id, "", None))
        except Exception as exc:  # noqa: BLE001
            _LOGGER.warning("device-server enumeration failed: %s", exc)

    if not devices and gateway_error:
        raise HTTPException(502, f"Could not list account devices: {gateway_error}")

    out: list[dict] = []
    seen: set[str] = set()
    for name, iot_id, pk, nickname in devices:
        if not name or name in seen:
            continue
        if DeviceType.is_rtk(name, pk):
            continue  # skip RTK base stations
        seen.add(name)
        out.append({"name": name, "iot_id": iot_id or None, "nickname": nickname})
    return out


@app.get("/api/onboard/scan")
async def onboard_scan():
    """Enumerate account mowers + discover proxies on the LAN, then propose a
    pairing by matching proxy bonded_name == cloud device_name.

    Requires a cloud session: established via /api/onboard/login on first run,
    or auto-logged-in from secrets.toml on the settings path.
    """
    cloud = await _cloud_mowers()
    proxies = await discover(timeout=2.5)

    # The HC33's BLE scan is lazy — bonded_name stays "none" until a TCP client
    # connects (which triggers connect_mower → the scan).  For any proxy that
    # reported "none", open a brief TCP connection to kick the scan, give it a
    # moment to find + cache the mower's name, then re-broadcast.  This makes
    # first-run onboarding work without the user manually connecting first.
    unbonded = [
        p for p in proxies
        if not p.get("bonded_name") or p["bonded_name"] == "none"
    ]
    if unbonded:
        await asyncio.gather(*(
            poke_proxy(p["ip"], int(p.get("proxy_port", 9876))) for p in unbonded
        ))
        # Scan early-exits sub-second on the strong RSSI of a glued-on mower;
        # 4 s covers the GATT-connect tail before the name is readable.
        await asyncio.sleep(4.0)
        proxies = await discover(timeout=2.5)

    # Index bonded proxies by the mower name they're attached to.  "none" still
    # means unbonded after the poke — mower off or out of range.
    bonded = {
        p["bonded_name"]: p
        for p in proxies
        if p.get("bonded_name") and p["bonded_name"] != "none"
    }

    matched: list[dict] = []
    unmatched_mowers: list[dict] = []
    used_chips: set[str] = set()
    for m in cloud:
        p = bonded.get(m["name"])
        if p:
            matched.append({
                "name":      m["name"],
                "nickname":  m.get("nickname"),
                "iot_id":    m["iot_id"],
                "hc33_host": p["ip"],
                "hc33_port": int(p.get("proxy_port", 9876)),
                "chip_id":   p.get("chip_id"),
                "variant":   p.get("variant"),
            })
            used_chips.add(p.get("chip_id"))
        else:
            unmatched_mowers.append(m)

    unmatched_proxies = [p for p in proxies if p.get("chip_id") not in used_chips]
    return {
        "matched":           matched,
        "unmatched_mowers":  unmatched_mowers,   # cloud mowers with no proxy found
        "unmatched_proxies": unmatched_proxies,  # proxies with no/none/foreign bond
    }


@app.post("/api/onboard/save")
async def onboard_save(payload: dict = Body(...)):
    """Persist secrets.toml + mowers.toml and bring the new roster up live.

    Body: {"mowers": [{name, nickname?, iot_id?, hc33_host, hc33_port?}, ...],
            "email"?: str, "password"?: str}
    Credentials fall back to the ones captured at /api/onboard/login.
    """
    rows = payload.get("mowers") or []
    norm: list[dict] = []
    for m in rows:
        name = (m.get("name") or "").strip()
        host = (m.get("hc33_host") or "").strip()
        if not name or not host:
            raise HTTPException(400, f"mower entry missing name or hc33_host: {m!r}")
        norm.append({
            "name":      name,
            "nickname":  ((m.get("nickname") or "").strip() or None),
            "hc33_host": host,
            "hc33_port": int(m.get("hc33_port") or 9876),
            "iot_id":    (m.get("iot_id") or None),
        })
    if not norm:
        raise HTTPException(400, "no mowers to save")

    email = (payload.get("email") or state.onboard_email or "").strip()
    password = payload.get("password") or state.onboard_password or ""
    if email and password:
        persist.save_secrets(email, password)

    persist.save_mowers(norm)
    await _apply_mowers(norm)

    # Clear the in-memory onboarding creds now that they're on disk.
    state.onboard_email = None
    state.onboard_password = None
    return {"ok": True, "count": len(norm)}


# ── WebSocket: joystick + server-side dead-man ───────────────────────────────
@app.websocket("/ws/joystick/{name}")
async def joystick_ws(ws: WebSocket, name: str):
    await ws.accept()
    h = state.handles.get(name)
    if h is None:
        await ws.close(code=1003, reason="unknown mower")
        return
    state.websockets.add(ws)

    stopped = True   # nothing in motion yet
    lock = asyncio.Lock()

    async def send_stop():
        nonlocal stopped
        async with lock:
            if not stopped:
                await h.send_raw(h.commands.stop_and_not_save_task())
                stopped = True

    try:
        while True:
            data = await ws.receive_json()
            # data: {"x": -1..1, "y": -1..1, "force": 0..1}
            x = float(data.get("x", 0.0))
            y = float(data.get("y", 0.0))
            force = float(data.get("force", 0.0))

            # In nipplejs, y is positive UP (joystick pushed away from user).
            # Stick forward → move_forward; stick back → move_back.  Names match
            # actual mower motion — the Stage-1 "inversion" memory was wrong.
            if force < DEAD_ZONE:
                await send_stop()
                continue

            stopped = False
            async with lock:
                if abs(y) >= abs(x):
                    speed = min(abs(y), MAX_LINEAR)
                    if y > 0:
                        await h.send_raw(h.commands.move_forward(linear=speed))
                    else:
                        await h.send_raw(h.commands.move_back(linear=speed))
                else:
                    speed = min(abs(x), MAX_ANGULAR)
                    if x > 0:
                        await h.send_raw(h.commands.move_right(angular=speed))
                    else:
                        await h.send_raw(h.commands.move_left(angular=speed))

    except WebSocketDisconnect:
        _LOGGER.info("joystick ws disconnected for %s", name)
    finally:
        state.websockets.discard(ws)
        # Always stop on disconnect — explicit safety in case the browser closed
        # mid-motion without sending a stick-release frame.
        with contextlib.suppress(Exception):
            await h.send_raw(h.commands.stop_and_not_save_task())


# ── Entry point for `python app.py` ──────────────────────────────────────────
if __name__ == "__main__":
    import uvicorn
    # timeout_graceful_shutdown caps how long uvicorn waits for in-flight
    # connections (incl. open WebSockets that other shutdown paths missed)
    # before closing them.  Without it, Ctrl-C on Windows can wedge forever.
    uvicorn.run(app, host="0.0.0.0", port=8000, timeout_graceful_shutdown=2)
