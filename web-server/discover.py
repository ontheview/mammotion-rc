"""UDP broadcast discovery of HC33 proxies on the local network.

Server-side mirror of firmware/src/discovery.cpp.  We broadcast a probe and
every HC33 on the segment replies (unicast) with chip_id / variant / proxy
port / bonded mower name.  The onboarding flow pairs a discovered proxy with a
cloud mower by matching bonded_name == device_name.

Wire protocol (ASCII, UDP, port 9878):
    probe (we send, broadcast):  b"HC33-DISCOVER?1"
    reply (HC33 sends, unicast): b"HC33-PROXY chip_id=A49DF4 variant=wifi "
                                 b"proxy_port=9876 bonded_name=Luba-XXXXXXXX"

bonded_name == "none" means the HC33 hasn't bonded to a mower yet (no BLE scan
since boot — connect a client to its proxy_port once to trigger it).
"""

from __future__ import annotations

import asyncio
import contextlib
import socket
import time

HC33_DISCOVERY_PORT = 9878
PROBE = b"HC33-DISCOVER?1"
REPLY_PREFIX = b"HC33-PROXY"


def _broadcast_targets() -> list[str]:
    """Directed subnet broadcasts for each local IPv4 interface, plus the
    limited broadcast as a fallback.

    Directed broadcast (e.g. 192.168.1.255) is the address with a real chance
    of crossing an L2 bridge to the HaLow side; 255.255.255.255 is link-local
    and mostly useful on the wifi build.  Tries netifaces for accurate
    per-interface broadcast addresses; degrades gracefully if it isn't
    installed.
    """
    targets = {"255.255.255.255"}
    try:
        import netifaces  # optional dep

        for iface in netifaces.interfaces():
            for info in netifaces.ifaddresses(iface).get(netifaces.AF_INET, []):
                bcast = info.get("broadcast")
                if bcast:
                    targets.add(bcast)
    except Exception:
        # Fallback: derive a /24 directed broadcast from our primary IP.
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            s.connect(("8.8.8.8", 80))  # no packets sent; just picks the route
            ip = s.getsockname()[0]
            s.close()
            targets.add(ip.rsplit(".", 1)[0] + ".255")
        except Exception:
            pass
    return sorted(targets)


def discover_blocking(timeout: float = 2.0) -> list[dict[str, str]]:
    """Broadcast a probe and collect replies for *timeout* seconds.

    Returns a list of dicts: {chip_id, variant, proxy_port, bonded_name, ip}.
    Deduplicated by chip_id (a proxy may answer multiple broadcast targets).
    Blocking — call via discover() from async code.
    """
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    sock.bind(("", 0))
    sock.settimeout(0.3)

    for target in _broadcast_targets():
        try:
            sock.sendto(PROBE, (target, HC33_DISCOVERY_PORT))
        except OSError:
            pass  # down/unsuitable iface — ignore

    found: dict[str, dict[str, str]] = {}
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            data, (ip, _port) = sock.recvfrom(512)
        except socket.timeout:
            continue
        except OSError:
            break
        if not data.startswith(REPLY_PREFIX):
            continue
        try:
            tokens = data.decode("utf-8", "replace").split()[1:]
            kv = dict(tok.split("=", 1) for tok in tokens if "=" in tok)
        except ValueError:
            continue
        kv["ip"] = ip
        found[kv.get("chip_id", ip)] = kv

    sock.close()
    return list(found.values())


async def discover(timeout: float = 2.0) -> list[dict[str, str]]:
    """Async wrapper — runs the blocking discovery on a worker thread."""
    return await asyncio.to_thread(discover_blocking, timeout)


async def poke_proxy(host: str, port: int, hold: float = 0.3, timeout: float = 2.0) -> bool:
    """Open a brief TCP connection to a proxy to trigger its lazy BLE scan.

    The HC33 only scans for its mower once a TCP client connects (connect_mower
    in ble_central.cpp), and caches the mower's BLE-advertised name from that
    scan.  A freshly-booted proxy therefore reports bonded_name=none until
    something connects.  We connect, hold briefly so the firmware enters the
    scan, then close — sending no bytes, so the wire-protocol parser never sees
    a frame.  The scan runs to completion regardless of our early close, so the
    name is cached and a follow-up broadcast picks it up.
    """
    try:
        _reader, writer = await asyncio.wait_for(
            asyncio.open_connection(host, port), timeout
        )
    except Exception:
        return False
    try:
        await asyncio.sleep(hold)
    finally:
        writer.close()
        with contextlib.suppress(Exception):
            await writer.wait_closed()
    return True


if __name__ == "__main__":
    for p in discover_blocking():
        print(p)
