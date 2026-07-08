// Abstract interface for the "BLE side" of the HC33 proxy.
//
// Two planned implementations:
//   - BleCentral  (Mode 1): NimBLE central running on the S3 chip itself.
//                          Used when the HC33 talks directly to the mower's
//                          GATT via its onboard 2.4 GHz radio.
//   - BleUart     (Mode 2): byte-pipe over UART to an external ESP32-C3
//                          SuperMini, which owns the BLE central.  Used when
//                          the S3 needs its 2.4 GHz radio for a softAP and
//                          BLE coexistence is unacceptable.
//
// TcpProxy depends only on this interface — it doesn't care whether the
// bytes are reaching the mower's GATT directly or hopping through a UART.

#pragma once

#include <Arduino.h>
#include <functional>
#include <string>

class BleLink {
public:
    using NotifyCallback = std::function<void(const uint8_t* data, size_t len)>;

    virtual ~BleLink() = default;

    // One-shot init at boot.  Returns true on success.
    virtual bool begin() = 0;

    virtual bool is_connected() const = 0;

    // Lazy connect — called when a TCP client arrives.
    virtual bool connect_mower() = 0;

    virtual void disconnect_mower() = 0;

    // Write one BLE characteristic operation worth of bytes.  Returns true on success.
    virtual bool write(const uint8_t* data, size_t len) = 0;

    // Register a callback for inbound notifications.  The callback runs from
    // whatever task the implementation uses (NimBLE host task for Mode 1, UART
    // RX task for Mode 2) — caller is responsible for any cross-task hand-off.
    virtual void set_notify_callback(NotifyCallback cb) = 0;

    // The BLE-advertised name of the currently-bonded mower (e.g.
    // "Luba-XXXXXXXX").  Empty string until a successful scan picked a
    // candidate.  Used by mDNS advertisement to pair LAN-discovered proxies
    // with cloud-side device names.  Default empty so implementations that
    // can't surface this (e.g. a future BleUart that delegates scanning to an
    // external chip) compile without changes.
    virtual std::string get_bonded_name() const { return {}; }
};
