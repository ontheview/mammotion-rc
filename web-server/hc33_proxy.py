"""HC33ProxyTransport — tunnels PyMammotion's BluFi BLE traffic over a TCP socket.

The BluFi codec runs on this side (via a real BleMessage wrapping a tiny shim
BleakClient that forwards write_gatt_char to TCP).  The other end of the socket
is a dumb byte pipe between TCP and the mower's GATT characteristics:

- examples/loopback_proxy.py for testing (Stage 3)
- ESP32 firmware running the same protocol over WiFi/HaLow (Stage 4+)

Wire format (each direction, length-prefixed binary frames):

    [2-byte BE length][payload]

Each frame in the TX direction is one BLE characteristic write; each frame in
the RX direction is one BLE notification.  Reassembly into a full BluFi payload
happens on this side via BleMessage.parseNotification.

This transport declares ``transport_type = TransportType.BLE`` so the rest of
PyMammotion's routing (active_transport, keepalive, ble_polling_loop) treats it
exactly like a local-bleak BLE link.

NOTE: this file used to live inside the PyMammotion package
(``pymammotion/transport/hc33_proxy.py``) as an untracked add-on.  It is
additive — nothing inside PyMammotion imports it — so it now ships with the
web-server instead, letting us ``pip install`` a clean, unpatched upstream
PyMammotion (0.8.9+).  It subclasses ``pymammotion.transport.base.Transport``;
keep it in sync if that base class's abstract surface changes.
"""

from __future__ import annotations

import asyncio
import logging
import time
from typing import TYPE_CHECKING, Any

from pymammotion.bluetooth.ble_message import BleMessage
from pymammotion.transport.base import (
    Transport,
    TransportAvailability,
    TransportError,
    TransportType,
)

if TYPE_CHECKING:
    from collections.abc import Awaitable, Callable

_logger = logging.getLogger(__name__)


class _TCPBleakClientShim:
    """Minimal BleakClient stand-in for BleMessage.

    BleMessage only calls ``client.write_gatt_char(uuid, data, response)`` and
    reads ``client.is_connected``.  We satisfy just those.
    """

    def __init__(self, transport: HC33ProxyTransport) -> None:
        self._transport = transport

    async def write_gatt_char(self, _uuid: str, data: bytes, _response: bool) -> None:
        await self._transport._send_frame(bytes(data))

    @property
    def is_connected(self) -> bool:
        return self._transport.is_connected


class HC33ProxyTransport(Transport):
    """Send PyMammotion BLE traffic through a TCP socket to an HC33 proxy."""

    on_message: Callable[[bytes], Awaitable[None]] | None = None

    def __init__(self, device_id: str, host: str, port: int) -> None:
        super().__init__()
        self._device_id = device_id
        self._host = host
        self._port = port
        self._reader: asyncio.StreamReader | None = None
        self._writer: asyncio.StreamWriter | None = None
        self._reader_task: asyncio.Task[None] | None = None
        self._message: BleMessage | None = None
        self._availability: TransportAvailability = TransportAvailability.DISCONNECTED
        self._connect_lock: asyncio.Lock = asyncio.Lock()
        self._send_lock: asyncio.Lock = asyncio.Lock()

    @property
    def transport_type(self) -> TransportType:
        return TransportType.BLE

    @property
    def is_connected(self) -> bool:
        return self._writer is not None and not self._writer.is_closing()

    @property
    def availability(self) -> TransportAvailability:
        return self._availability

    @property
    def is_usable(self) -> bool:
        return super().is_usable

    async def connect(self) -> None:
        async with self._connect_lock:
            if self.is_connected:
                return
            await self._notify_availability(TransportAvailability.CONNECTING)
            try:
                self._reader, self._writer = await asyncio.open_connection(self._host, self._port)
            except OSError as exc:
                await self._notify_availability(TransportAvailability.DISCONNECTED)
                raise TransportError(
                    f"HC33ProxyTransport connect to {self._host}:{self._port} failed: {exc}"
                ) from exc

            self._message = BleMessage(_TCPBleakClientShim(self))  # type: ignore[arg-type]
            self._reader_task = asyncio.create_task(self._reader_loop(), name="hc33-proxy-reader")
            await self._notify_availability(TransportAvailability.CONNECTED)
            _logger.info("HC33ProxyTransport connected to %s:%d", self._host, self._port)

    async def disconnect(self) -> None:
        if self._reader_task is not None and not self._reader_task.done():
            self._reader_task.cancel()
            try:
                await self._reader_task
            except (asyncio.CancelledError, Exception):
                pass
        self._reader_task = None
        if self._writer is not None:
            self._writer.close()
            try:
                await self._writer.wait_closed()
            except Exception:  # noqa: BLE001
                pass
        self._writer = None
        self._reader = None
        self._message = None
        await self._notify_availability(TransportAvailability.DISCONNECTED)

    async def send(self, payload: bytes, iot_id: str = "", firmware_version: str = "1.0.0.0") -> None:
        # ``firmware_version`` was added to Transport.send() in pymammotion 0.8.x;
        # accepted for signature compatibility but unused here (the HC33 wire
        # framing is version-independent).
        if not self.is_connected:
            await self.connect()
        if self._message is None:
            raise TransportError("HC33ProxyTransport.send: no BleMessage codec")
        self._last_send_monotonic = time.monotonic()
        async with self._send_lock:
            try:
                await self._message.post_custom_data_bytes(payload)
            except Exception as exc:
                await self._notify_availability(TransportAvailability.DISCONNECTED)
                raise TransportError(f"HC33ProxyTransport send failed: {exc}") from exc

    # ── internal ──────────────────────────────────────────────────────────────

    async def _send_frame(self, data: bytes) -> None:
        if self._writer is None or self._writer.is_closing():
            raise TransportError("HC33ProxyTransport: socket not connected")
        header = len(data).to_bytes(2, "big")
        self._writer.write(header + data)
        await self._writer.drain()

    async def _reader_loop(self) -> None:
        assert self._reader is not None
        try:
            while True:
                header = await self._reader.readexactly(2)
                length = int.from_bytes(header, "big")
                if length == 0:
                    continue
                data = await self._reader.readexactly(length)
                if self._message is None:
                    continue
                result = self._message.parseNotification(data)
                # result == 0  → complete frame, extract payload
                # result == 1  → fragment, more to come
                # result == 2  → duplicate sequence
                # result < 0   → parse error
                if result != 0:
                    continue
                payload = await self._message.parseBlufiNotifyData(return_bytes=True)
                self._message.clear_notification()
                if payload and self.on_message is not None:
                    await self.on_message(bytes(payload))
        except (
            asyncio.IncompleteReadError,
            ConnectionResetError,    # Windows: WinError 64 when HC33 FINs the socket
            ConnectionAbortedError,
            BrokenPipeError,
        ) as exc:
            _logger.info("HC33ProxyTransport: peer closed socket (%s)", type(exc).__name__)
        except asyncio.CancelledError:
            raise
        except Exception:  # noqa: BLE001
            _logger.exception("HC33ProxyTransport reader loop crashed")
        finally:
            await self._notify_availability(TransportAvailability.DISCONNECTED)

    async def _notify_availability(self, state: TransportAvailability) -> None:
        self._availability = state
        await self._fire_availability_listeners(state)
