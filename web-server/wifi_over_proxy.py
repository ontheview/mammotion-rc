"""Drive the mower's WiFi commands over the HC33 BLE-proxy.

Same transport path as the web server (HC33ProxyTransport -> HC33 firmware over
TCP), but standalone so you can poke the WiFi commands by hand.

IMPORTANT: the HC33 proxy accepts ONE TCP client and the mower has ONE BLE slot,
so STOP the web server (or at least that mower's connection) before running this,
or they'll fight over the link.

Two families of actions:

  * Mammotion-proto actions (list/info/enable/...) send Mammotion protobuf over
    BluFi's "custom data" channel (subtype 19).  On this firmware the WiFi
    read-back ones (list/info) get no answer — see the memory note.

  * NATIVE BluFi actions (scan/wifi-status/ble-version) send stock ESP-BluFi
    control frames (CTRL subtype 9 / 5 / 7) directly through the codec, the same
    way the Mammotion app provisions WiFi during onboarding.  This is the probe
    for "does the firmware actually implement native BluFi?".  All three are
    read-only control frames with no payload, so they change nothing on the mower.

Usage (run from the web-server dir, using the same venv as run-server.bat):

    python wifi_over_proxy.py --host 192.168.1.42 --action scan          # native
    python wifi_over_proxy.py --host 192.168.1.42 --action wifi-status   # native
    python wifi_over_proxy.py --host 192.168.1.42 --action list          # proto
    python wifi_over_proxy.py --host 192.168.1.42 --action connect --ssid MyNetwork

--host is the HC33's IP (the hc33_host from secrets.toml); --port defaults to 9876.
Add --raw to log every TCP frame + BluFi reassembly result.  Responses arrive as
async notifications, so the script waits --wait seconds after sending.

Reminder: the Mammotion-proto connect/reconnect only work for an SSID the mower
already remembers (DrvWifiSet has no password field).  Joining a NEW network would
need the native BluFi set-SSID/set-password/connect frames, which this script does
NOT send (write path is deliberately out of scope here).
"""

from __future__ import annotations

import argparse
import asyncio
import logging
import sys

import betterproto2

if sys.platform == "win32":
    asyncio.set_event_loop_policy(asyncio.WindowsSelectorEventLoopPolicy())

from pymammotion.data.model.device import MowingDevice
from pymammotion.device.handle import DeviceHandle
from pymammotion.transport.base import TransportAvailability

# Local module (ships with the web-server, not with PyMammotion).
from hc33_proxy import HC33ProxyTransport

_LOGGER = logging.getLogger("wifi_over_proxy")

# close_clear_connect_current_wifi status codes (proto WifiConfType).
_STATUS = {"disconnect": 0, "forget": 1, "connect": 2, "reconnect": 3}

# Native BluFi CTRL subtypes (read-only, no payload) → what we probe with.
#   5 = get WiFi status  → reply DATA subtype 15
#   7 = get version      → reply DATA subtype 16
#   9 = get WiFi list    → reply DATA subtype 17 (the "scan" the app shows)
_NATIVE_CTRL = {"wifi-status": 5, "ble-version": 7, "scan": 9}

# BluFi DATA subtypes we know how to label on the way back.
_DATA_WIFI_STATUS = 15
_DATA_VERSION = 16
_DATA_WIFI_LIST = 17
_DATA_ERROR = 18
_DATA_CUSTOM = 19  # Mammotion protobuf channel


def _parse_blufi_wifi_list(data: bytes) -> list[tuple[int, str]]:
    """Decode an ESP-BluFi WiFi-list (subtype 17) payload.

    Format is a back-to-back sequence of records:
        [len:1][rssi:1][ssid: len-1 bytes]
    where ``len`` counts the rssi byte plus the SSID bytes.  rssi is a signed int8.
    """
    aps: list[tuple[int, str]] = []
    i, n = 0, len(data)
    while i < n:
        rec_len = data[i]
        i += 1
        if rec_len < 1 or i + rec_len > n:
            break
        rec = data[i : i + rec_len]
        i += rec_len
        rssi_raw = rec[0]
        rssi = rssi_raw - 256 if rssi_raw > 127 else rssi_raw
        ssid = rec[1:].decode("utf-8", "replace")
        aps.append((rssi, ssid))
    return aps


def _fmt_ip(v: int) -> str:
    """Format a proto fixed32 IP as dotted-quad (big-endian: high byte first)."""
    v &= 0xFFFFFFFF
    return ".".join(str((v >> (8 * i)) & 0xFF) for i in range(3, -1, -1))


def _parse_blufi_wifi_status(data: bytes) -> str:
    """Decode an ESP-BluFi WiFi connection-state report (subtype 15).

    Layout: [opmode:1][sta_conn_state:1][softap_conn_count:1] then a TLV list of
    "extra info" entries [type:1][len:1][value:len].  We surface the STA SSID/BSSID
    so we can see which AP the mower is actually associated to.
    """
    if len(data) < 3:
        return f"(short) raw={data.hex()}"
    opmodes = {0: "NULL", 1: "STA", 2: "SoftAP", 3: "STA+SoftAP"}
    opmode, sta_state, softap_cnt = data[0], data[1], data[2]
    parts = [
        f"opmode={opmodes.get(opmode, opmode)}",
        f"sta={'CONNECTED' if sta_state == 0 else f'not-connected({sta_state})'}",
        f"softap_clients={softap_cnt}",
    ]
    i, n = 3, len(data)
    while i + 2 <= n:
        t, ln = data[i], data[i + 1]
        i += 2
        v = data[i : i + ln]
        i += ln
        if t == 0x01 and len(v) == 6:
            parts.append("bssid=" + ":".join(f"{b:02x}" for b in v))
        elif t == 0x02:
            parts.append("SSID=" + v.decode("utf-8", "replace"))
        elif t == 0x04:
            parts.append("softap_ssid=" + v.decode("utf-8", "replace"))
        else:
            parts.append(f"tlv{t}={v.hex()}")
    return "  ".join(parts)


class _NativeBluFiTransport(HC33ProxyTransport):
    """HC33ProxyTransport that (a) can send native BluFi control frames and
    (b) splits incoming frames: BluFi custom-data (subtype 19) goes to the normal
    on_message/protobuf path, while native subtypes (15/16/17/18 and any ctrl
    frame) are decoded and logged here instead of being fed to the protobuf
    handler (which would choke on them).
    """

    _verbose = False
    native_reply_seen = False
    rx_frames = 0  # count of ALL TCP frames received — the true liveness signal

    async def send_native_ctrl(self, subtype: int) -> None:
        """Send a native BluFi CTRL frame with no payload (e.g. 9 = get wifi list)."""
        if self._message is None:
            raise RuntimeError("transport not connected")
        type_val = self._message.getTypeValue(0, subtype)  # 0 = CTRL package type
        async with self._send_lock:
            # encrypt=False, checksum=False, require_ack=False, data=None
            await self._message.post(False, False, False, type_val, None)

    def _on_native_frame(self, pkg: int, sub: int, data: bytes) -> None:
        self.native_reply_seen = True
        if pkg == 1 and sub == _DATA_WIFI_LIST:
            aps = _parse_blufi_wifi_list(data)
            _LOGGER.info("*** BluFi WiFi SCAN LIST — %d AP(s):", len(aps))
            for rssi, ssid in sorted(aps, key=lambda a: a[0], reverse=True):
                _LOGGER.info("       %4d dBm   %s", rssi, ssid)
        elif pkg == 1 and sub == _DATA_WIFI_STATUS:
            _LOGGER.info("*** BluFi WiFi STATUS: %s", _parse_blufi_wifi_status(data))
        elif pkg == 1 and sub == _DATA_VERSION:
            _LOGGER.info("*** BluFi VERSION (subtype 16) raw=%s", data.hex())
        elif pkg == 1 and sub == _DATA_ERROR:
            _LOGGER.info("*** BluFi ERROR report (subtype 18) raw=%s", data.hex())
        else:
            _LOGGER.info(
                "*** native BluFi frame pkg=%d sub=%d raw=%s", pkg, sub, data.hex()
            )

    async def _reader_loop(self) -> None:  # noqa: C901
        assert self._reader is not None
        try:
            while True:
                header = await self._reader.readexactly(2)
                length = int.from_bytes(header, "big")
                if length == 0:
                    continue
                data = await self._reader.readexactly(length)
                self.rx_frames += 1
                if self._message is None:
                    continue
                result = self._message.parseNotification(data)
                if self._verbose:
                    _LOGGER.info(
                        "RAW frame len=%d parseNotification=%d first-bytes=%s",
                        length, result, data[:8].hex(),
                    )
                # 0=complete, 1=fragment(more), 2=duplicate, <0=parse error
                if result != 0:
                    continue
                pkg = self._message.notification.getPkgType()
                sub = self._message.notification.getSubType()
                payload = await self._message.parseBlufiNotifyData(return_bytes=True)
                self._message.clear_notification()
                if pkg == 1 and sub == _DATA_CUSTOM:
                    # Mammotion protobuf over custom-data → normal handler.
                    if payload and self.on_message is not None:
                        await self.on_message(bytes(payload))
                else:
                    # Native BluFi frame — decode here, keep it away from protobuf.
                    self._on_native_frame(pkg, sub, bytes(payload) if payload else b"")
        except asyncio.IncompleteReadError:
            _LOGGER.info("reader: peer closed socket")
        except asyncio.CancelledError:
            raise
        except Exception:  # noqa: BLE001
            _LOGGER.exception("reader loop crashed")
        finally:
            await self._notify_availability(TransportAvailability.DISCONNECTED)


async def main(args: argparse.Namespace) -> None:
    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
    )

    transport = _NativeBluFiTransport(
        device_id=args.device_name, host=args.host, port=args.port
    )
    transport._verbose = args.raw
    handle = DeviceHandle(
        device_id=args.device_name,
        device_name=args.device_name,
        initial_device=MowingDevice(name=args.device_name),
        ble_transport=transport,
        prefer_ble=True,
    )

    await transport.connect()
    await handle.start()
    _LOGGER.info("connected to HC33 proxy at %s:%d", args.host, args.port)

    # Tap the Mammotion protobuf stream so proto-action replies are visible too.
    # Also record that *any* protobuf telemetry arrived → the BLE link is alive.
    seen = {"proto": False}

    async def _on_incoming(msg: object) -> None:
        seen["proto"] = True
        try:
            sub, sub_val = betterproto2.which_one_of(msg, "LubaSubMsg")
            if sub == "net":
                net_sub, payload = betterproto2.which_one_of(msg.net, "NetSubType")
                if net_sub == "toapp_networkinfo_rsp":
                    _LOGGER.info(
                        "*** CONNECTED WiFi: ssid=%r  rssi=%s dBm  ip=%s  gw=%s  mac=%s",
                        getattr(payload, "wifi_ssid", "?"),
                        getattr(payload, "wifi_rssi", "?"),
                        _fmt_ip(getattr(payload, "ip", 0)),
                        _fmt_ip(getattr(payload, "gateway", 0)),
                        getattr(payload, "wifi_mac", "?"),
                    )
                else:
                    _LOGGER.info("<<< net.%s: %s", net_sub, payload)
            else:
                _LOGGER.info("<<< %s", sub)
        except Exception as e:  # noqa: BLE001
            _LOGGER.info("incoming-tap (undecoded): %s", e)

    _sub = handle.broker.subscribe_unsolicited(_on_incoming)  # keep ref alive

    action = args.action
    if action in _NATIVE_CTRL:
        # Liveness/wake first: get_report_cfg(count=0) subscribes the mower to its
        # continuous status stream — this is what makes it emit pept/sys/net.  If
        # that stream flows but the native frame gets no reply, that's a *clean*
        # negative for native BluFi (not just an idle mower).
        _LOGGER.info("liveness ping: proto get_report_cfg(count=0) — start report stream...")
        await handle.send_raw(handle.commands.get_report_cfg(count=0))
        await asyncio.sleep(3.0)
        subtype = _NATIVE_CTRL[action]
        _LOGGER.info(
            "sending NATIVE BluFi CTRL subtype %d (%s)...", subtype, action
        )
        await transport.send_native_ctrl(subtype)
    else:
        c = handle.commands
        # Every proto net query/action is only serviced inside an active
        # todev_ble_sync session (the app keeps this heartbeat alive; our proxy
        # path doesn't). Prime it first — this is what made netinfo start replying.
        _LOGGER.info("priming BLE session: todev_ble_sync(3) x3 ...")
        for _ in range(3):
            await handle.send_raw(handle.commands.send_todev_ble_sync(sync_type=3))
            await asyncio.sleep(0.8)
        if action == "list":
            cmd = c.get_record_wifi_list()
        elif action == "info":
            cmd = c.wifi_connectinfo_update()
        elif action == "netinfo":
            cmd = c.get_device_network_info()
        elif action == "enable":
            cmd = c.set_device_wifi_enable_status(new_wifi_status=True)
        elif action == "disable":
            cmd = c.set_device_wifi_enable_status(new_wifi_status=False)
        else:  # connect / reconnect / disconnect / forget
            if not args.ssid:
                _LOGGER.error("--ssid is required for action %r", action)
                await handle.stop()
                await transport.disconnect()
                return
            cmd = c.close_clear_connect_current_wifi(
                ssid=args.ssid, status=_STATUS[action]
            )
        _LOGGER.info("sending action=%r ssid=%r", action, args.ssid)
        await handle.send_raw(cmd)

    _LOGGER.info("waiting %.1fs for the response frames...", args.wait)
    await asyncio.sleep(args.wait)

    if action in _NATIVE_CTRL:
        _LOGGER.info("(received %d TCP frame(s) total this run)", transport.rx_frames)
        if transport.native_reply_seen:
            _LOGGER.info(
                "RESULT: got a native BluFi reply — firmware SPEAKS native BluFi."
            )
        elif seen["proto"]:
            _LOGGER.info(
                "RESULT: link is ALIVE (proto telemetry flowed) but NO native reply "
                "to CTRL subtype %d — this native command is likely NOT implemented. "
                "(CTRL-9 scan IS confirmed working, so the firmware implements a "
                "SUBSET of native BluFi — the onboarding flow, not the query commands.)",
                _NATIVE_CTRL[action],
            )
        elif transport.rx_frames > 0:
            _LOGGER.info(
                "RESULT: HC33 sent %d frame(s) but none decoded as proto telemetry or "
                "a native reply — unexpected. Re-run with --raw to see them.",
                transport.rx_frames,
            )
        else:
            _LOGGER.info(
                "RESULT: TCP pipe was SILENT (0 frames in %.0fs) — the HC33 received no "
                "BLE notifications at all. Either the mower is idle/asleep or the "
                "HC33<->mower BLE link is down. Wake the mower / confirm the proxy has "
                "the mower connected, then re-run.",
                args.wait,
            )

    await handle.stop()
    await transport.disconnect()
    _LOGGER.info("done")


_ACTION_HELP = """\
actions:
  native BluFi control frames (raw ESP-BluFi; only the onboarding subset is
  implemented by the mower firmware):
    scan          scan for nearby APs -> prints "BluFi WiFi SCAN LIST" (ssid+rssi).
                  CONFIRMED working. Read-only.
    wifi-status   native get-wifi-status (CTRL-5). NOT implemented by the firmware
                  (no reply) -- use 'netinfo' instead.
    ble-version   native get-version (CTRL-7). NOT implemented (no reply).

  Mammotion-proto over BluFi custom-data (ALL of these need an active
  todev_ble_sync session -- the script primes it automatically before sending):
    netinfo       read the CURRENT connection -> prints "CONNECTED WiFi"
                  (ssid, rssi, ip, gateway, mac). This is the reliable status read.
    list          request remembered-network list (toapp_ListUpload). Often unanswered.
    info          request current-connection msg (toapp_WifiMsg). Often unanswered.
    enable        turn the mower's WiFi radio ON.
    disable       turn the mower's WiFi radio OFF.
    connect       connect to a REMEMBERED network now (needs --ssid; no password).
                  Runtime only -- reverts to the higher-priority saved net on reboot.
    reconnect     reconnect to --ssid (remembered).
    disconnect    disconnect from --ssid.
    forget        forget/remove --ssid from the saved list (needs --ssid).

  Note: none of these join a NEW network with a password -- that needs native
  BluFi provisioning (set-SSID DATA-2 + set-password DATA-3 + connect CTRL-3),
  which this script does not send.
"""

_EPILOG = _ACTION_HELP + """
examples:
  # is the mower on the softAP or the site AP right now?
  python wifi_over_proxy.py --host 192.168.1.87 --action netinfo --wait 10
  # what APs can it see?
  python wifi_over_proxy.py --host 192.168.1.87 --action scan --wait 15
  # push it onto its own softAP (remembered net, no password; not durable)
  python wifi_over_proxy.py --host 192.168.1.87 --action connect --ssid LubaAP1

operational notes:
  * STOP the web server first. The HC33 accepts one TCP client and the server's
    auto-reconnect will fight this script for the slot (symptom: 0 TCP frames).
  * --host is the HC33's IP (hc33_host in secrets.toml); one mower per proxy.
  * Add --raw to log every TCP frame + BluFi reassembly result for debugging.
"""


def cli() -> None:
    p = argparse.ArgumentParser(
        description="Drive a Luba mower's WiFi commands over the HC33 BLE proxy.",
        epilog=_EPILOG,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    p.add_argument("--host", required=True,
                   help="HC33 proxy IP (the hc33_host from secrets.toml)")
    p.add_argument("--port", type=int, default=9876,
                   help="HC33 proxy TCP port (default: 9876)")
    p.add_argument("--device-name", default="Luba-BLE",
                   help="internal handle name; any string (default: Luba-BLE)")
    p.add_argument(
        "--action",
        required=True,
        metavar="ACTION",
        choices=[
            # native BluFi control frames (read-only probes)
            "scan", "wifi-status", "ble-version",
            # Mammotion-proto over custom-data
            "list", "info", "netinfo", "enable", "disable",
            "connect", "reconnect", "disconnect", "forget",
        ],
        help="what to do; see the 'actions' section below for each choice",
    )
    p.add_argument("--ssid",
                   help="target SSID; REQUIRED for connect/reconnect/disconnect/forget")
    p.add_argument("--wait", type=float, default=5.0,
                   help="seconds to wait for async responses after sending (default: 5.0)")
    p.add_argument("--raw", action="store_true",
                   help="log every raw TCP frame + BluFi reassembly result (debug)")
    asyncio.run(main(p.parse_args()))


if __name__ == "__main__":
    cli()
