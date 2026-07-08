#include "config_load.h"
#include "config.h"

#include <Arduino.h>          // Serial.printf for the source: line (see log_resolved)
#include <esp_log.h>
#include <esp_partition.h>
#include <cstring>

static const char* TAG = "config_load";

// Resolved config for this boot.  config_load() is the only writer.
hc33_config_v1 g_cfg = {};

// ── helpers ──────────────────────────────────────────────────────────────────

// strlcpy-equivalent that does NOT depend on the toolchain providing strlcpy
// (IDF 4.4 / GCC 8.4 in env:hc33-standard-wifi doesn't always expose it).
// Always NUL-terminates dst.  Returns nothing — we don't need the length.
static void copy_str(char* dst, size_t dst_sz, const char* src) {
    if (dst_sz == 0) return;
    size_t i = 0;
    for (; i < dst_sz - 1 && src[i] != '\0'; ++i) dst[i] = src[i];
    dst[i] = '\0';
}

// Plain zlib CRC32 (poly 0xEDB88320, init 0xFFFFFFFF, final XOR 0xFFFFFFFF).
// Self-implemented rather than using esp_rom_crc32_le so the init/xor handling
// is unambiguous and matches Python's zlib.crc32 and the browser JS exactly.
// 206-byte CRC region at boot → ~1700 bit ops, negligible.
static uint32_t crc32_zlib(const uint8_t* data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b) {
            uint32_t mask = -(int32_t)(crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

// Mask all but first/last char of a password for logging.  "" stays "".
static void mask_pw(char* out, size_t out_sz, const char* in) {
    size_t n = strlen(in);
    if (n == 0)                   { copy_str(out, out_sz, "");          return; }
    if (n <= 2)                   { copy_str(out, out_sz, "**");        return; }
    // out_sz is small (we only log it).  Cap stars at 8 to keep logs readable.
    size_t stars = n - 2; if (stars > 8) stars = 8;
    size_t i = 0;
    if (i < out_sz - 1) out[i++] = in[0];
    for (size_t s = 0; s < stars && i < out_sz - 1; ++s) out[i++] = '*';
    if (i < out_sz - 1) out[i++] = in[n - 1];
    out[i] = '\0';
}

static void log_resolved(const char* source) {
    char up_pw[16], ap_pw[16];
    mask_pw(up_pw, sizeof(up_pw), g_cfg.uplink_pass);
    mask_pw(ap_pw, sizeof(ap_pw), g_cfg.softap_pass);
    // Use Serial.printf rather than ESP_LOGI: pioarduino 51.x silently
    // filters INFO-level ESP_LOG output from this TU at runtime (cause
    // unclear; [I][main] still works).  Verified empirically across boots.
    Serial.printf("[config_load] source: %s\n", source);
    Serial.printf("[config_load]   uplink_ssid='%s' uplink_pass='%s' region='%s'\n",
                  g_cfg.uplink_ssid, up_pw, g_cfg.region);
    Serial.printf("[config_load]   softap_ssid='%s' softap_pass='%s' softap_channel=%u\n",
                  g_cfg.softap_ssid, ap_pw, (unsigned)g_cfg.softap_channel);
    Serial.flush();
}

// Populate g_cfg from the compile-time #defines in config.h.  Called on any
// problem reading the partition (blank, foreign magic, version mismatch, CRC
// fail, read error, partition not in table) so the firmware always boots with
// SOMETHING usable.  This is what env:hc33's `pio run -t upload` lands on
// every time — the dev path.
static void load_defaults() {
    g_cfg.magic   = HC33_CONFIG_MAGIC;
    g_cfg.version = HC33_CONFIG_VERSION;
    copy_str(g_cfg.uplink_ssid, sizeof(g_cfg.uplink_ssid), WIFI_SSID);
    copy_str(g_cfg.uplink_pass, sizeof(g_cfg.uplink_pass), WIFI_PASS);
    copy_str(g_cfg.region,      sizeof(g_cfg.region),      WIFI_REGION_HALOW);
    copy_str(g_cfg.softap_ssid, sizeof(g_cfg.softap_ssid), AP_SSID);
    copy_str(g_cfg.softap_pass, sizeof(g_cfg.softap_pass), AP_PASSWORD);
    g_cfg.softap_channel = AP_CHANNEL;
    g_cfg.crc32 = 0;   // unused once resolved
}

// ── public ───────────────────────────────────────────────────────────────────

void config_load() {
    // Start with compiled defaults — every failure path below leaves these in
    // place, so we never have to remember to populate g_cfg before returning.
    load_defaults();

    const esp_partition_t* p = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA,
        static_cast<esp_partition_subtype_t>(HC33_CONFIG_SUBTYPE),
        "config");
    if (p == nullptr) {
        Serial.printf("[config_load] no `config` partition in table — using defaults\n");
        log_resolved("defaults (no partition)");
        return;
    }

    hc33_config_v1 blob = {};
    esp_err_t err = esp_partition_read(p, 0, &blob, sizeof(blob));
    if (err != ESP_OK) {
        Serial.printf("[config_load] partition read failed: %s — using defaults\n",
                      esp_err_to_name(err));
        log_resolved("defaults (read error)");
        return;
    }

    if (blob.magic != HC33_CONFIG_MAGIC) {
        // Unflashed (all 0xFF) or some other tool's blob.  Expected state on
        // a freshly-flashed dev board.
        Serial.printf("[config_load] partition blank/foreign (magic=0x%08x) — using defaults\n",
                      (unsigned)blob.magic);
        log_resolved("defaults (no valid blob)");
        return;
    }

    if (blob.version != HC33_CONFIG_VERSION) {
        Serial.printf("[config_load] version %u != %u — using defaults\n",
                      (unsigned)blob.version, (unsigned)HC33_CONFIG_VERSION);
        log_resolved("defaults (version mismatch)");
        return;
    }

    // CRC covers everything before the crc32 field.  offsetof on a packed POD
    // is well-defined here.
    const size_t crc_len = offsetof(hc33_config_v1, crc32);
    uint32_t want = crc32_zlib(reinterpret_cast<const uint8_t*>(&blob), crc_len);
    if (want != blob.crc32) {
        Serial.printf("[config_load] CRC mismatch (blob=0x%08x calc=0x%08x) — using defaults\n",
                      (unsigned)blob.crc32, (unsigned)want);
        log_resolved("defaults (crc mismatch)");
        return;
    }

    // Defensive NUL-termination: the writer always pads with zeros but a
    // corrupted-but-CRC-aliased blob could theoretically be missing terminators.
    blob.uplink_ssid[sizeof(blob.uplink_ssid) - 1] = '\0';
    blob.uplink_pass[sizeof(blob.uplink_pass) - 1] = '\0';
    blob.region     [sizeof(blob.region)      - 1] = '\0';
    blob.softap_ssid[sizeof(blob.softap_ssid) - 1] = '\0';
    blob.softap_pass[sizeof(blob.softap_pass) - 1] = '\0';

    g_cfg = blob;

    // Sanity-check the channel: a partition built by a buggy packer (or an
    // INI with "softap_channel = 0") would leave us with an unusable AP.
    // 1-14 is the IEEE 802.11 b/g/n range (14 only in Japan); anything else
    // gets the compiled AP_CHANNEL default.
    if (g_cfg.softap_channel < 1 || g_cfg.softap_channel > 14) {
        Serial.printf("[config_load] softap_channel=%u out of range — using compiled default %u\n",
                      (unsigned)g_cfg.softap_channel, (unsigned)AP_CHANNEL);
        g_cfg.softap_channel = AP_CHANNEL;
    }

    log_resolved("config partition");
}
