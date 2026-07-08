// BLE central wrapper for the Mammotion mower's GATT.
//
// Two responsibilities:
//   1. At boot, do a one-shot scan to cache the mower's BLE address+type so
//      subsequent connect() calls don't pay the 15 s scan tax.
//   2. Expose lazy connect / disconnect tied to the lifecycle of a single
//      external consumer (the TCP client in tcp_proxy.cpp) — never pre-connect.
//
// The mower drops idle GATT links after ~15 s of no traffic, so the consumer
// must drive connect() right before its first write and disconnect_mower()
// when the client goes away.  See memory: luba-2x-ble-idle-timeout.

#pragma once

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <string>
#include "ble_link.h"

class BleCentral : public BleLink {
public:
    // BLE init at boot.  Does NOT scan — connect_mower() scans on demand
    // so the TCP server can bind before BLE startup time.
    bool begin() override;

    bool is_connected() const override;

    // Lazy connect.  Uses cached address if available, falls back to a fresh
    // scan if the cached connect fails.  Returns true on success.
    bool connect_mower() override;

    void disconnect_mower() override;

    // Write *data* to the Mammotion write characteristic (with response).
    // Returns true on success.  Caller is responsible for fragmentation —
    // each call is one ATT write.
    bool write(const uint8_t* data, size_t len) override;

    // Register a callback for inbound notifications on the notify
    // characteristic.  The callback fires from the NimBLE host task —
    // hand-off to the main loop via a queue before doing anything blocking.
    void set_notify_callback(NotifyCallback cb) override;

    // Name advertised by the bonded mower (e.g. "Luba-XXXXXXXX").  Captured
    // from the BLE advertisement payload during scan_for_mower_; empty until
    // first successful scan and empty whenever the chosen advertiser had no
    // name field.
    std::string get_bonded_name() const override { return bonded_name_; }

private:
    NimBLEClient*               client_      = nullptr;
    NimBLERemoteCharacteristic* write_char_  = nullptr;
    NimBLERemoteCharacteristic* notify_char_ = nullptr;

    NimBLEAddress mower_addr_      { };
    uint8_t       mower_addr_type_ = BLE_ADDR_PUBLIC;
    bool          addr_known_      = false;
    std::string   bonded_name_;

    NotifyCallback notify_cb_;

    // Internal: scan up to *timeout_ms* for the strongest advertiser of
    // UUID_SERVICE (no MAC/name filter — see ble_central.cpp ScanCb).  Updates
    // mower_addr_ / mower_addr_type_ / addr_known_ on success.
    bool scan_for_mower_(uint32_t timeout_ms);

    // NimBLE notify trampoline → bound notify_cb_.
    static void notify_trampoline_(NimBLERemoteCharacteristic* chr,
                                   uint8_t* data, size_t len, bool is_notify);

    // Set during begin() so notify_trampoline_ can find us.
    static BleCentral* s_instance_;
};
