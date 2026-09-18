#include "camera_stream.h"

#ifdef ENABLE_CAMERA

#include <Arduino.h>
#include <esp_camera.h>
#include <esp_http_server.h>
#include <esp_log.h>

#include "camera_pins.h"
#include "log.h"

static const char* TAG = "cam";

// ── multipart/x-mixed-replace scaffolding ───────────────────────────────────
#define PART_BOUNDARY "hc33frame"
static const char* STREAM_CONTENT_TYPE =
    "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char* STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char* STREAM_PART_HDR =
    "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

static httpd_handle_t s_httpd  = nullptr;   // port 80: /jpg, /capture, /status
static httpd_handle_t s_stream = nullptr;   // port 81: /stream

// Counters.  Aligned 32-bit reads/writes are atomic on the S3, and these are
// statistics rather than control state, so no lock is taken -- a torn sample
// would only skew one metrics line.
static volatile uint32_t s_frames_total   = 0;
static volatile uint32_t s_bytes_total    = 0;
static volatile uint32_t s_capture_errors = 0;
static volatile uint32_t s_send_errors    = 0;
static volatile uint8_t  s_stream_clients = 0;

// Window state for the FPS/average calculation.
static uint32_t s_last_log_ms     = 0;
static uint32_t s_last_frames     = 0;
static uint32_t s_last_bytes      = 0;
static float    s_fps             = 0.0f;
static uint32_t s_avg_frame_bytes = 0;

// Only one concurrent /stream client is accepted.  With fb_count=2 a second
// reader just contends for the same two buffers: both streams stutter and the
// capture path spends its time waiting instead of producing. Refusing the
// second client keeps the first one smooth, which matters more for a driving
// view than serving two degraded ones.  Raise fb_count before raising this.
static const uint8_t MAX_STREAM_CLIENTS = 1;

// ── camera ──────────────────────────────────────────────────────────────────
static bool camera_init() {
    camera_config_t config = {};

    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer   = LEDC_TIMER_0;

    config.pin_d0 = Y2_GPIO_NUM;
    config.pin_d1 = Y3_GPIO_NUM;
    config.pin_d2 = Y4_GPIO_NUM;
    config.pin_d3 = Y5_GPIO_NUM;
    config.pin_d4 = Y6_GPIO_NUM;
    config.pin_d5 = Y7_GPIO_NUM;
    config.pin_d6 = Y8_GPIO_NUM;
    config.pin_d7 = Y9_GPIO_NUM;

    config.pin_xclk  = XCLK_GPIO_NUM;
    config.pin_pclk  = PCLK_GPIO_NUM;
    config.pin_vsync = VSYNC_GPIO_NUM;
    config.pin_href  = HREF_GPIO_NUM;

    config.pin_sccb_sda = SIOD_GPIO_NUM;
    config.pin_sccb_scl = SIOC_GPIO_NUM;

    config.pin_pwdn  = PWDN_GPIO_NUM;
    config.pin_reset = RESET_GPIO_NUM;

    // Conservative settings, unchanged from the validated camera-test run:
    // 640x480 JPEG, quality 12, two framebuffers in PSRAM.  No tuning yet.
    config.xclk_freq_hz = 20000000;
    config.pixel_format = PIXFORMAT_JPEG;
    config.frame_size   = FRAMESIZE_SVGA;
    config.jpeg_quality = 12;
    config.fb_count     = 2;
    config.fb_location  = CAMERA_FB_IN_PSRAM;
    config.grab_mode    = CAMERA_GRAB_LATEST;

    if (!psramFound()) {
        ESP_LOGE(TAG, "no PSRAM — camera needs memory_type=qio_opi + BOARD_HAS_PSRAM");
        return false;
    }

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_camera_init failed: 0x%x", err);
        return false;
    }

    sensor_t* s = esp_camera_sensor_get();
    if (!s) {
        ESP_LOGE(TAG, "esp_camera_sensor_get returned NULL");
        return false;
    }
    ESP_LOGI(TAG, "sensor PID 0x%04X%s", s->id.PID,
             s->id.PID == OV3660_PID ? " (OV3660)" : " (NOT an OV3660)");

    // OV3660 orientation + image tuning for the installed HC33 camera.
    // Keep the validated sensor orientation correction, but avoid the reference
    // example's washed-out look: neutral brightness, a little more contrast,
    // and slightly stronger saturation.
    if (s->id.PID == OV3660_PID) {
        s->set_vflip(s, 1);
        s->set_brightness(s, 0);
        s->set_contrast(s, 2);
        s->set_saturation(s, 2);
	s->set_sharpness(s, 1);
    }
    return true;
}

// ── handlers ────────────────────────────────────────────────────────────────
static esp_err_t jpg_handler(httpd_req_t* req) {
    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) {
        s_capture_errors++;
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=hc33.jpg");
    // Snapshots are diagnostic: never let a proxy or browser serve a stale one.
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t rc = httpd_resp_send(req, (const char*)fb->buf, fb->len);

    s_frames_total++;
    s_bytes_total += fb->len;
    esp_camera_fb_return(fb);
    return rc;
}

static esp_err_t status_handler(httpd_req_t* req) {
    CameraMetrics m;
    camera_stream_metrics(&m);

    char buf[320];
    int n = snprintf(buf, sizeof(buf),
        "{\"fps\":%.1f,\"avg_frame_bytes\":%u,\"frames_total\":%u,"
        "\"capture_errors\":%u,\"send_errors\":%u,\"stream_clients\":%u,"
        "\"free_psram\":%u,\"free_heap\":%u}",
        m.fps, (unsigned)m.avg_frame_bytes, (unsigned)m.frames_total,
        (unsigned)m.capture_errors, (unsigned)m.send_errors,
        (unsigned)m.stream_clients,
        (unsigned)m.free_psram, (unsigned)m.free_heap);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, buf, n);
}

static esp_err_t stream_handler(httpd_req_t* req) {
    if (s_stream_clients >= MAX_STREAM_CLIENTS) {
        ESP_LOGW(TAG, "refusing second /stream client");
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "text/plain");
        return httpd_resp_send(req, "camera busy: one stream client at a time", -1);
    }

    esp_err_t rc = httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
    if (rc != ESP_OK) return rc;
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    // The web-server proxies this over its own HTTPS origin, so CORS is not
    // strictly needed -- set it anyway so the endpoint stays usable directly
    // from a plain-HTTP debug page on the LAN.
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    s_stream_clients++;
    ESP_LOGI(TAG, "/stream client connected (%u active)", s_stream_clients);

    char part_hdr[64];
    while (true) {
        camera_fb_t* fb = esp_camera_fb_get();
        if (!fb) {
            s_capture_errors++;
            // A NULL frame is usually transient (driver requeuing). Pause
            // briefly and retry rather than dropping a client that is fine.
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        int hlen = snprintf(part_hdr, sizeof(part_hdr), STREAM_PART_HDR,
                            (unsigned)fb->len);

        rc = httpd_resp_send_chunk(req, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY));
        if (rc == ESP_OK) rc = httpd_resp_send_chunk(req, part_hdr, hlen);
        if (rc == ESP_OK) rc = httpd_resp_send_chunk(req, (const char*)fb->buf, fb->len);

        if (rc == ESP_OK) {
            s_frames_total++;
            s_bytes_total += fb->len;
        }
        esp_camera_fb_return(fb);

        if (rc != ESP_OK) {
            // Client went away (tab closed, proxy dropped). Normal, not a fault.
            s_send_errors++;
            break;
        }

        // Yield so the WiFi/lwIP tasks on this core get scheduled between
        // frames even when the link is fast enough to never block us.
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    if (s_stream_clients) s_stream_clients--;
    ESP_LOGI(TAG, "/stream client gone (%u active)", s_stream_clients);
    return ESP_OK;
}

// ── lifecycle ───────────────────────────────────────────────────────────────
bool camera_stream_begin() {
    if (!camera_init()) return false;

    // Instance 1 — short handlers on port 80.
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port   = CAMERA_HTTP_PORT;
    cfg.ctrl_port     = 32080;
    cfg.core_id       = 0;   // keep off core 1, where loopTask drives BLE/TCP
    cfg.task_priority = 1;   // below the WiFi/BT stacks so radios never starve
    cfg.stack_size    = 4096;
    cfg.max_uri_handlers = 4;
    cfg.lru_purge_enable = true;

    if (httpd_start(&s_httpd, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start(:%d) failed", CAMERA_HTTP_PORT);
        return false;
    }

    httpd_uri_t u_jpg     = { "/jpg",     HTTP_GET, jpg_handler,    nullptr };
    httpd_uri_t u_capture = { "/capture", HTTP_GET, jpg_handler,    nullptr };
    httpd_uri_t u_status  = { "/status",  HTTP_GET, status_handler, nullptr };
    httpd_register_uri_handler(s_httpd, &u_jpg);
    httpd_register_uri_handler(s_httpd, &u_capture);
    httpd_register_uri_handler(s_httpd, &u_status);

    // Instance 2 — the blocking stream, on its own port and its own task so
    // it cannot wedge the handlers above.  ctrl_port MUST differ from the
    // first instance's: esp_http_server binds it internally, and a collision
    // makes the second httpd_start() fail with a confusing socket error.
    httpd_config_t scfg = HTTPD_DEFAULT_CONFIG();
    scfg.server_port   = CAMERA_STREAM_PORT;
    scfg.ctrl_port     = 32081;
    scfg.core_id       = 0;
    scfg.task_priority = 1;
    scfg.stack_size    = 4096;
    scfg.max_uri_handlers = 1;
    scfg.lru_purge_enable = true;

    if (httpd_start(&s_stream, &scfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start(:%d) failed", CAMERA_STREAM_PORT);
        httpd_stop(s_httpd);
        s_httpd = nullptr;
        return false;
    }

    httpd_uri_t u_stream = { "/stream", HTTP_GET, stream_handler, nullptr };
    httpd_register_uri_handler(s_stream, &u_stream);

    s_last_log_ms = millis();
    ESP_LOGI(TAG, "camera ready — snapshot :%d/jpg  stream :%d/stream",
             CAMERA_HTTP_PORT, CAMERA_STREAM_PORT);
    return true;
}

void camera_stream_metrics(CameraMetrics* out) {
    if (!out) return;
    out->fps             = s_fps;
    out->avg_frame_bytes = s_avg_frame_bytes;
    out->frames_total    = s_frames_total;
    out->capture_errors  = s_capture_errors;
    out->send_errors     = s_send_errors;
    out->stream_clients  = s_stream_clients;
    out->free_psram      = ESP.getFreePsram();
    out->free_heap       = ESP.getFreeHeap();
}

void camera_stream_log_metrics() {
    uint32_t now = millis();
    uint32_t elapsed = now - s_last_log_ms;
    if (elapsed < 1000) return;

    uint32_t frames = s_frames_total - s_last_frames;
    uint32_t bytes  = s_bytes_total  - s_last_bytes;

    s_fps = (elapsed > 0) ? (frames * 1000.0f / elapsed) : 0.0f;
    s_avg_frame_bytes = frames ? (bytes / frames) : 0;

    s_last_log_ms = now;
    s_last_frames = s_frames_total;
    s_last_bytes  = s_bytes_total;

    // Only chatter while something is actually happening; an idle camera
    // shouldn't bury the BLE/TCP log lines.
    if (frames == 0 && s_stream_clients == 0) return;

    ESP_LOGI(TAG,
             "fps=%.1f avg=%uB clients=%u frames=%u errs=%u/%u "
             "psram=%uKB heap=%uKB",
             s_fps, (unsigned)s_avg_frame_bytes, (unsigned)s_stream_clients,
             (unsigned)s_frames_total,
             (unsigned)s_capture_errors, (unsigned)s_send_errors,
             (unsigned)(ESP.getFreePsram() / 1024),
             (unsigned)(ESP.getFreeHeap() / 1024));
}

#endif  // ENABLE_CAMERA
