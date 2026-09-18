// OV3660 camera + MJPEG HTTP server for the HT-HC33.
//
// Compiled only when ENABLE_CAMERA is defined (env:hc33-standard-wifi).  On
// env:hc33 the whole translation unit is empty, so the HaLow build is
// unaffected.
//
// WHY THIS DOESN'T RUN IN loop()
// ------------------------------
// setup() registers loopTask with the task watchdog at 30 s, panic+reboot
// (main.cpp).  loop() must therefore return promptly, every time.  An MJPEG
// response is unbounded -- it runs for as long as a client watches, which is
// minutes or hours.  Serving it from loop() would trip the TWDT and reboot
// the proxy, taking BLE control down with it.  Even a chunked, "one frame per
// loop()" design would inject JPEG capture plus a socket write (tens of ms)
// straight into the control path on every iteration.
//
// So the HTTP work lives in tasks owned by esp_http_server, and loop() only
// ever reads counters.
//
// TWO SERVERS, TWO PORTS
// ----------------------
// esp_http_server dispatches requests from a single task per instance, so a
// long-running /stream handler blocks every other URI on that instance.  The
// fix Espressif uses in its own camera example, and the one used here, is two
// instances:
//
//   port 80  ->  /jpg, /capture, /status   (short handlers, always responsive)
//   port 81  ->  /stream                   (long-lived, blocks only itself)
//
// Diagnostics therefore keep working while a stream is running, which is the
// whole point -- /status is how you find out why a stream misbehaves.
//
// Both instances are pinned to core 0 at low priority.  The Arduino loopTask
// (BLE + TCP proxy control path) runs on core 1, so camera work cannot preempt
// it; and at priority below the WiFi/BT stacks it cannot starve the radios
// either.  Control keeps precedence over video by construction.

#pragma once

#ifdef ENABLE_CAMERA

#include <stdbool.h>
#include <stdint.h>

// Ports the two HTTP instances listen on.  The web-server proxies these; the
// browser never talks to them directly (it is served over HTTPS and would
// refuse the mixed-content fetch).
#define CAMERA_HTTP_PORT    80
#define CAMERA_STREAM_PORT  81

// Runtime counters, sampled by the 1 Hz serial metrics line in loop() and
// served as JSON from /status.
struct CameraMetrics {
    float    fps;                // frames/s over the last sampling window
    uint32_t avg_frame_bytes;    // mean JPEG size over that window
    uint32_t frames_total;
    uint32_t capture_errors;     // esp_camera_fb_get() returned NULL
    uint32_t send_errors;        // httpd_resp_send_chunk() failed (client gone)
    uint8_t  stream_clients;     // currently connected /stream clients
    uint32_t free_psram;
    uint32_t free_heap;          // internal RAM
};

// Initialise the sensor (640x480 JPEG, 2 framebuffers in PSRAM) and start
// both HTTP instances.  Call after the network has an IP.  Returns false on
// any failure, logged; the proxy keeps working without video.
bool camera_stream_begin();

// Snapshot the counters.  Cheap, lock-free, safe to call from loop().
void camera_stream_metrics(CameraMetrics* out);

// Emit the 1 Hz metrics line on serial.  Rate-limits internally, so calling
// it every loop() iteration is fine.
void camera_stream_log_metrics();

#endif  // ENABLE_CAMERA
