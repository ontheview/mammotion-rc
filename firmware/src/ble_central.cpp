#include "ble_central.h"
#include <esp_log.h>
#include <cctype>
#include <cstring>
#include "log.h"
#include "config.h"

static const char* TAG = "ble_central";

BleCentral* BleCentral::s_instance_ = nullptr;

// ── Scan callbacks (NimBLE 1.4.x style) ──────────────────────────────────────
namespace {

// True if a BLE advertised name looks like a Mammotion RTK base station
// (e.g. "RBSA09VVGPL", "RTKBAU241424235").  Case-insensitive prefix match.
// RTK bases advertise the same 0xFFFF service UUID as a mower, so the UUID
// filter alone can't tell them apart; this lets us reject them by name.
bool ble_name_is_rtk(const std::string& name) {
    auto has_prefix_ci = [&](const char* p) {
        const size_t n = std::strlen(p);
        if (name.size() < n) return false;
        for (size_t i = 0; i < n; ++i)
            if (std::toupper((unsigned char)name[i]) != p[i]) return false;
        return true;
    };
    return has_prefix_ci("RBS") || has_prefix_ci("RTK");
}

struct ScanCb : public NimBLEAdvertisedDeviceCallbacks {
    // Auto-discovery: instead of matching a hardcoded MAC, keep the strongest
    // advertiser of the Mammotion service UUID seen during the scan window.
    bool          have_candidate = false;
    int           best_rssi      = -127;
    NimBLEAddress best_addr       { };
    uint8_t       best_addr_type = BLE_ADDR_PUBLIC;
    std::string   best_name;

    void reset() {
        have_candidate = false;
        best_rssi      = -127;
        best_name.clear();
    }

    void onResult(NimBLEAdvertisedDevice* d) override {
        const int rssi = d->getRSSI();
        const bool advertises_svc =
            d->isAdvertisingService(NimBLEUUID(UUID_SERVICE));
        std::string name = d->getName();
#if BLE_SCAN_VERBOSE
        // Print every advertisement so we can see what the HC33 is hearing and,
        // crucially, whether the mower actually advertises UUID_SERVICE.
        ESP_LOGI(TAG, "  adv: %s rssi=%d svc=%d name=%s",
                 d->getAddress().toString().c_str(), rssi, (int)advertises_svc,
                 name.empty() ? "(no name)" : name.c_str());
#endif
        // Filter by service UUID only — no name/MAC filter.  0xFFFF is NOT
        // unique to Mammotion, but the HC33 is physically <10 cm from the mower
        // so the mower's RSSI dominates any nearby BluFi-using device by tens of
        // dB.  Picking the strongest advertiser is therefore reliable, and it
        // auto-supports any current/future Mammotion model without an allowlist.
        // See memory: hc33-runtime-config-plan.
        if (!advertises_svc) return;
        // Reject RTK base stations: they advertise the same 0xFFFF UUID as a
        // mower, so a base mounted near the HC33 could out-shout a weak mower
        // and get bonded by mistake.  Their advert name starts with RBS/RTK;
        // skip those.  No-name advertisers still pass (can't classify by name).
        if (ble_name_is_rtk(name)) {
            ESP_LOGI(TAG, "scan: skipping RTK base %s rssi=%d name=%s",
                     d->getAddress().toString().c_str(), rssi, name.c_str());
            return;
        }
        if (have_candidate && rssi <= best_rssi) return;

        best_rssi      = rssi;
        best_addr      = d->getAddress();
        best_addr_type = d->getAddressType();
        best_name      = name;
        have_candidate = true;
        ESP_LOGI(TAG, "scan: candidate %s rssi=%d (type=%u) name=%s",
                 best_addr.toString().c_str(), rssi, (unsigned)best_addr_type,
                 best_name.empty() ? "(no name)" : best_name.c_str());

        // A radio glued to the mower can't be out-shouted by anything in the
        // environment.  Once we hear one this strong, stop early rather than
        // burning the rest of the window — gives sub-second reconnects.
        if (rssi >= MOWER_RSSI_STRONG_DBM) {
            NimBLEDevice::getScan()->stop();
        }
    }
};
}  // namespace

// ── Lifecycle ────────────────────────────────────────────────────────────────

bool BleCentral::begin() {
    s_instance_ = this;
    NimBLEDevice::init(DEVICE_NAME);
    NimBLEDevice::setMTU(517);
    // Deliberately *don't* scan at boot — blocking up to SCAN_TIMEOUT_MS here
    // would keep the TCP server from binding and looking dead to clients.
    // connect_mower() scans on demand when the first TCP client arrives.
    return true;
}

bool BleCentral::is_connected() const {
    return client_ != nullptr && client_->isConnected();
}

bool BleCentral::connect_mower() {
    if (is_connected()) return true;

    // Always recreate the client.  NimBLE-Arduino retains GAP/GATT state on a
    // client object from a prior failed connect; reusing it can deadlock the
    // next attempt (silent re-fail until reboot).  Free everything that points
    // into the old client first so we don't keep dangling characteristic ptrs.
    if (client_ != nullptr) {
        write_char_  = nullptr;
        notify_char_ = nullptr;
        NimBLEDevice::deleteClient(client_);
        client_ = nullptr;
    }

    // Try cached address first.  If we never scanned at boot, rescan now.
    if (!addr_known_) {
        ESP_LOGI(TAG, "no cached address — scanning before connect");
        if (!scan_for_mower_(SCAN_TIMEOUT_MS)) {
            ESP_LOGE(TAG, "scan failed; cannot connect");
            return false;
        }
    }

    client_ = NimBLEDevice::createClient();
    client_->setConnectTimeout(BLE_CONNECT_TIMEOUT_MS / 1000);

    ESP_LOGI(TAG, "connecting to %s ...", mower_addr_.toString().c_str());
    bool ok = client_->connect(mower_addr_, mower_addr_type_);
    if (!ok) {
        // Address may have aged out.  One retry with a fresh scan.
        ESP_LOGW(TAG, "cached connect failed; rescanning");
        addr_known_ = false;
        if (!scan_for_mower_(SCAN_TIMEOUT_MS)) {
            ESP_LOGE(TAG, "rescan failed");
            return false;
        }
        ok = client_->connect(mower_addr_, mower_addr_type_);
    }
    if (!ok) {
        ESP_LOGE(TAG, "BLE connect failed");
        return false;
    }

    NimBLERemoteService* svc = client_->getService(NimBLEUUID(UUID_SERVICE));
    if (svc == nullptr) {
        ESP_LOGE(TAG, "service %s not found", UUID_SERVICE);
        client_->disconnect();
        return false;
    }

    write_char_  = svc->getCharacteristic(NimBLEUUID(UUID_WRITE_CHARACTERISTIC));
    notify_char_ = svc->getCharacteristic(NimBLEUUID(UUID_NOTIFY_CHARACTERISTIC));
    if (write_char_ == nullptr || notify_char_ == nullptr) {
        ESP_LOGE(TAG, "write/notify characteristic missing");
        client_->disconnect();
        return false;
    }

    if (!notify_char_->canNotify()) {
        ESP_LOGE(TAG, "notify char doesn't support notify");
        client_->disconnect();
        return false;
    }

    if (!notify_char_->subscribe(true, notify_trampoline_, true)) {
        ESP_LOGE(TAG, "subscribe to notify failed");
        client_->disconnect();
        return false;
    }

    ESP_LOGI(TAG, "BLE connected, MTU=%d", client_->getMTU());
    return true;
}

void BleCentral::disconnect_mower() {
    if (notify_char_ != nullptr) {
        notify_char_->unsubscribe();
        notify_char_ = nullptr;
    }
    write_char_ = nullptr;
    if (client_ != nullptr && client_->isConnected()) {
        client_->disconnect();
    }
}

bool BleCentral::write(const uint8_t* data, size_t len) {
    if (write_char_ == nullptr) return false;
    // Write-without-response when the characteristic supports it.  The Arduino
    // loopTask (which is what calls into here via TcpProxy::loop) is registered
    // with the IDF task watchdog at a 5 s timeout; write-with-response can
    // block this task waiting for the mower's ATT-level reply, which under
    // Agora-camera load routinely exceeds 5 s and reboots the chip.  BluFi
    // has its own sequence/ACK protocol on top, so we don't need ATT-level
    // confirmation for correctness.
    const bool needs_response = !write_char_->canWriteNoResponse();
    uint32_t t0 = millis();
    bool ok = write_char_->writeValue(data, len, needs_response);
    uint32_t dt = millis() - t0;
    if (dt > 500) {
        ESP_LOGW(TAG, "writeValue blocked for %ums (resp=%d, len=%u, ok=%d) — close to watchdog",
                 (unsigned)dt, (int)needs_response, (unsigned)len, (int)ok);
    }
    return ok;
}

void BleCentral::set_notify_callback(NotifyCallback cb) {
    notify_cb_ = std::move(cb);
}

// ── Internal ─────────────────────────────────────────────────────────────────

bool BleCentral::scan_for_mower_(uint32_t timeout_ms) {
    // NimBLE retains the callback pointer past the synchronous scan->start()
    // return; deferred host-task work touches it during scan teardown.  Stack
    // lifetime is unsafe — heap-allocate once and reuse across rescans.
    static ScanCb* cb = nullptr;
    if (cb == nullptr) {
        cb = new ScanCb();
    }
    cb->reset();
    NimBLEScan* scan = NimBLEDevice::getScan();
    scan->setAdvertisedDeviceCallbacks(cb, true);
    // Active scan: many devices put their service UUIDs in the scan response,
    // not the primary adv payload.  Without this the isAdvertisingService()
    // filter would never match the mower.
    scan->setActiveScan(true);
    scan->setInterval(100);
    scan->setWindow(100);
    ESP_LOGI(TAG, "scanning %u ms for a device advertising %s ...",
             (unsigned)timeout_ms, UUID_SERVICE);

    // NimBLE-Arduino 1.4.x: scan->start() is async — it kicks off the scan and
    // returns immediately.  Unlike the old MAC match we do NOT bail on first
    // hit: run the whole window so RSSI ranking sees every advertiser.  The
    // callback stops the scan early only when it hears a very-strong candidate
    // (one attached to the mower), so the common reconnect case is sub-second.
    scan->start(timeout_ms / 1000, nullptr, false);
    uint32_t deadline = millis() + timeout_ms;
    while (scan->isScanning() && (int32_t)(deadline - millis()) > 0) {
        delay(50);
    }
    scan->stop();
    scan->clearResults();

    if (cb->have_candidate) {
        mower_addr_      = cb->best_addr;
        mower_addr_type_ = cb->best_addr_type;
        bonded_name_     = cb->best_name;
        addr_known_      = true;
        ESP_LOGI(TAG, "scan: selected %s (rssi=%d, type=%u, name=%s)",
                 mower_addr_.toString().c_str(), cb->best_rssi,
                 (unsigned)mower_addr_type_,
                 bonded_name_.empty() ? "(no name)" : bonded_name_.c_str());
        return true;
    }
    ESP_LOGW(TAG, "no device advertising %s found in scan", UUID_SERVICE);
    return false;
}

void BleCentral::notify_trampoline_(NimBLERemoteCharacteristic* /*chr*/,
                                    uint8_t* data, size_t len, bool /*is_notify*/) {
    if (s_instance_ && s_instance_->notify_cb_) {
        s_instance_->notify_cb_(data, len);
    }
}
