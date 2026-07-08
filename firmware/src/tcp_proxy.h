// Single-client TCP server that pipes length-prefixed binary frames between
// the socket and BleCentral.
//
// Wire format (both directions): [2-byte BE length N][N bytes payload].
// Each frame is exactly one BLE GATT operation.  See memory:
// hc33-tcp-wire-protocol.
//
// Lifecycle (mirrors examples/loopback_proxy.py):
//   - listen on TCP_PORT
//   - when a client connects: BleCentral::connect_mower(); start piping
//   - when client disconnects (any reason): BleCentral::disconnect_mower()
//   - serve at most one client at a time; new connections during an active
//     session are accepted and immediately closed (or we could reject; current
//     code closes the prior client first — see code for actual behavior).

#pragma once

#include <Arduino.h>
#include <vector>
#include <queue>
#include "ble_link.h"
#include "net_compat.h"

class TcpProxy {
public:
    explicit TcpProxy(BleLink& ble);

    // Bind the listening socket.  Call after WiFi is connected.
    void begin();

    // Drive the state machine; call from Arduino loop().
    void loop();

private:
    enum class RxState {
        WAIT_LEN,
        READ_DATA,
        WRITE_PENDING,   // full frame received, waiting for BLE TX to accept it
    };

    BleLink&     ble_;
    NetServer    server_;
    NetClient    client_;
    bool         ble_open_     = false;

    RxState              rx_state_   = RxState::WAIT_LEN;
    uint16_t             rx_len_     = 0;   // expected frame length once known
    uint16_t             rx_have_    = 0;   // bytes accumulated so far
    std::vector<uint8_t> rx_buf_;

    // Idle-watchdog: millis() of the last fully-received frame from the client.
    // If now - last_rx_ms_ exceeds CLIENT_IDLE_TIMEOUT_MS we treat the client
    // as dead and drop the socket; the existing disconnect path then unwinds
    // the BLE link as a safety stop.
    uint32_t last_rx_ms_ = 0;

    // Backpressure bookkeeping for the lossless BLE write path.  While a frame
    // can't be handed to BLE (TX buffers full) we hold it in rx_buf_ and keep
    // re-trying instead of dropping it — dropping would break the BluFi send
    // sequence and desync the mower.  write_pending_since_ms_ marks when the
    // current frame started waiting; write_stalled_ gates the one-shot warning.
    uint32_t write_pending_since_ms_ = 0;
    bool     write_stalled_          = false;

    // Notify frames pushed from NimBLE host task → drained in loop().
    SemaphoreHandle_t              notify_mutex_ = nullptr;
    std::queue<std::vector<uint8_t>> notify_queue_;

    void on_client_connected_();
    void on_client_disconnected_();
    void poll_socket_();
    bool try_write_pending_();   // attempt the held frame; false = still backpressured
    void drain_notify_queue_();
    void send_frame_(const uint8_t* data, size_t len);
};
