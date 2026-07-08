#include "tcp_proxy.h"
#include <esp_log.h>
#include "log.h"
#include "config.h"

static const char* TAG = "tcp_proxy";

TcpProxy::TcpProxy(BleLink& ble)
    : ble_(ble), server_(TCP_PORT) {
    notify_mutex_ = xSemaphoreCreateMutex();

    // Pushed onto notify_queue_ from the NimBLE host task; drained in loop().
    ble_.set_notify_callback([this](const uint8_t* data, size_t len) {
        if (!notify_mutex_) return;
        xSemaphoreTake(notify_mutex_, portMAX_DELAY);
        notify_queue_.emplace(data, data + len);
        xSemaphoreGive(notify_mutex_);
    });
}

void TcpProxy::begin() {
    server_.begin();
    server_.setNoDelay(true);
    ESP_LOGI(TAG, "listening on :%d", TCP_PORT);
}

void TcpProxy::loop() {
    if (!client_.connected()) {
        if (ble_open_) {
            on_client_disconnected_();
        }
        NetClient incoming = server_.accept();
        if (incoming) {
            client_ = incoming;
            client_.setNoDelay(true);
            on_client_connected_();
        }
        return;
    }

    // Idle watchdog — silent network failure won't trigger TCP FIN/RST for
    // a long time on its own; this is the safety stop.
    if ((uint32_t)(millis() - last_rx_ms_) > CLIENT_IDLE_TIMEOUT_MS) {
        ESP_LOGW(TAG, "no TCP frame in %u ms — dropping client (safety stop)",
                 (unsigned)CLIENT_IDLE_TIMEOUT_MS);
        client_.stop();
        return;
    }

    poll_socket_();
    drain_notify_queue_();
}

void TcpProxy::on_client_connected_() {
    IPAddress ip = client_.remoteIP();
    ESP_LOGI(TAG, "client connected from %s — opening BLE",
             ip.toString().c_str());
    rx_state_ = RxState::WAIT_LEN;
    rx_len_   = 0;
    rx_have_  = 0;
    rx_buf_.clear();
    write_stalled_ = false;
    last_rx_ms_ = millis();   // arm idle watchdog
    // Mark BLE owned by this client BEFORE attempting the connect, so the
    // failure path also runs disconnect_mower() — otherwise a half-initialised
    // NimBLE client/characteristic state from a failed attempt sticks around
    // until reboot and every subsequent reconnect attempt silently re-fails.
    ble_open_ = true;
    if (!ble_.connect_mower()) {
        ESP_LOGE(TAG, "BLE connect failed — dropping client");
        ble_.disconnect_mower();   // tear down any partial state
        ble_open_ = false;
        client_.stop();
        return;
    }
    ESP_LOGI(TAG, "BLE up — piping bytes");
}

void TcpProxy::on_client_disconnected_() {
    ESP_LOGI(TAG, "client gone — closing BLE");
    ble_.disconnect_mower();
    ble_open_ = false;
    if (notify_mutex_) {
        xSemaphoreTake(notify_mutex_, portMAX_DELAY);
        std::queue<std::vector<uint8_t>> empty;
        notify_queue_.swap(empty);
        xSemaphoreGive(notify_mutex_);
    }
}

void TcpProxy::poll_socket_() {
    while (true) {
        if (rx_state_ == RxState::WAIT_LEN) {
            if (client_.available() < 2) return;
            uint8_t hdr[2];
            client_.read(hdr, 2);
            rx_len_ = ((uint16_t)hdr[0] << 8) | (uint16_t)hdr[1];
            if (rx_len_ == 0) continue;           // skip empty frames
            if (rx_len_ > MAX_FRAME_LEN) {
                ESP_LOGW(TAG, "frame too large (%u > %u) — disconnect",
                         (unsigned)rx_len_, (unsigned)MAX_FRAME_LEN);
                client_.stop();
                return;
            }
            rx_buf_.assign(rx_len_, 0);
            rx_have_  = 0;
            rx_state_ = RxState::READ_DATA;
        }

        // READ_DATA — accumulate until full frame
        if (rx_state_ == RxState::READ_DATA) {
            if (rx_have_ < rx_len_) {
                int avail = client_.available();
                if (avail <= 0) return;
                int want = rx_len_ - rx_have_;
                int to_read = (avail < want) ? avail : want;
                int got = client_.read(rx_buf_.data() + rx_have_, to_read);
                if (got <= 0) return;
                rx_have_ += (uint16_t)got;
            }
            if (rx_have_ < rx_len_) return;
            // Full frame ready → hand to the lossless BLE writer.
            rx_state_ = RxState::WRITE_PENDING;
            write_pending_since_ms_ = millis();
        }

        // WRITE_PENDING — push the held frame to BLE, never dropping it.
        if (rx_state_ == RxState::WRITE_PENDING) {
            if (!try_write_pending_()) {
                // TX buffers still full.  Keep the frame and STOP reading the
                // socket so TCP backpressures the server; we retry on the next
                // loop() tick (which re-pets the task watchdog).  A genuinely
                // dead link is caught by CLIENT_IDLE_TIMEOUT_MS, which then
                // drops the client and rebuilds BLE — resyncing the sequence.
                return;
            }
            last_rx_ms_ = millis();
            rx_state_ = RxState::WAIT_LEN;
            rx_len_   = 0;
            rx_have_  = 0;
        }
    }
}

// Try to write the currently-held frame (rx_buf_, rx_len_) to the mower's GATT.
// Returns true once accepted; false means the BLE controller's TX buffers are
// full right now (ENOMEM) — the caller holds the frame and retries later.  A
// failure here is buffer pressure, not RF loss, so retrying after a short yield
// (which lets the NimBLE host task service a connection event and free mbufs)
// is the correct remedy.  We deliberately do NOT drop: see BLE_WRITE_* in
// config.h and memory hc33-tcp-wire-protocol.
bool TcpProxy::try_write_pending_() {
    // If the BLE link itself is down there's nothing to drain — bail to the
    // disconnect path immediately rather than holding the frame for 30 s.
    if (!ble_.is_connected()) {
        ESP_LOGW(TAG, "BLE link down while writing — dropping client to force resync");
        client_.stop();
        return false;
    }
    for (int attempt = 0; attempt < BLE_WRITE_RETRY_BURST; attempt++) {
        if (ble_.write(rx_buf_.data(), rx_len_)) {
            if (write_stalled_) {
                ESP_LOGI(TAG, "BLE write recovered after %ums backpressure",
                         (unsigned)(millis() - write_pending_since_ms_));
                write_stalled_ = false;
            }
            return true;
        }
        delay(BLE_WRITE_RETRY_DELAY_MS);   // yield so a connection event can drain TX
    }
    // Still blocked after a burst.  Warn once, then let loop() come back to us.
    uint32_t stalled_for = millis() - write_pending_since_ms_;
    if (!write_stalled_ && stalled_for > BLE_WRITE_STALL_LOG_MS) {
        ESP_LOGW(TAG, "BLE write backpressured %ums (TX buffers full) — holding frame, not dropping",
                 (unsigned)stalled_for);
        write_stalled_ = true;
    }
    return false;
}

void TcpProxy::drain_notify_queue_() {
    while (true) {
        std::vector<uint8_t> frame;
        if (!notify_mutex_) return;
        xSemaphoreTake(notify_mutex_, portMAX_DELAY);
        if (notify_queue_.empty()) {
            xSemaphoreGive(notify_mutex_);
            return;
        }
        frame = std::move(notify_queue_.front());
        notify_queue_.pop();
        xSemaphoreGive(notify_mutex_);

        send_frame_(frame.data(), frame.size());
    }
}

void TcpProxy::send_frame_(const uint8_t* data, size_t len) {
    if (!client_.connected()) return;
    uint8_t hdr[2] = {
        (uint8_t)((len >> 8) & 0xFF),
        (uint8_t)(len & 0xFF),
    };
    client_.write(hdr, 2);
    client_.write(data, len);
}
