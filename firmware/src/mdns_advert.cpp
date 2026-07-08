#include "mdns_advert.h"

#include <Arduino.h>
#include <ESPmDNS.h>
#include <esp_log.h>
#include <esp_mac.h>

#include "config.h"

static const char* TAG = "mdns";

#ifdef USE_STANDARD_WIFI
static const char* kVariant = "wifi";
#else
static const char* kVariant = "halow";
#endif

// Last bonded_name value pushed into the TXT record.  Kept here (not in
// BleCentral) so re-publishing is idempotent and doesn't touch BLE state.
static std::string s_last_bonded_name;
static bool        s_mdns_up = false;

// Build the "hc33-XXXXXX" hostname / chip_id from the last 3 bytes of the
// base MAC.  Stable across reboots, unique per chip.  We don't use the WiFi
// MAC directly because on the HaLow build the relevant interface MAC is
// MM6108-side, not the S3's; the base MAC is the only thing both builds
// agree on.  esp_efuse_mac_get_default() exists in both IDF 4.4 (standard-
// wifi build) and IDF 5.x (HaLow build) — preferred over esp_read_mac to
// avoid an extra header diff between the two envs.
static void chip_id_hex(char out[7]) {
    uint8_t mac[6] = {0};
    esp_efuse_mac_get_default(mac);
    snprintf(out, 7, "%02X%02X%02X", mac[3], mac[4], mac[5]);
}

bool mdns_advert_begin() {
    char chip_id[7];
    chip_id_hex(chip_id);

    char hostname[16];
    snprintf(hostname, sizeof(hostname), "hc33-%s", chip_id);

    if (!MDNS.begin(hostname)) {
        ESP_LOGE(TAG, "MDNS.begin(%s) failed", hostname);
        return false;
    }
    MDNS.setInstanceName(hostname);

    if (!MDNS.addService("hc33proxy", "tcp", TCP_PORT)) {
        ESP_LOGE(TAG, "addService(_hc33proxy._tcp) failed");
        return false;
    }
    MDNS.addServiceTxt("hc33proxy", "tcp", "chip_id", (const char*)chip_id);
    MDNS.addServiceTxt("hc33proxy", "tcp", "variant", kVariant);
    // Register bonded_name as a non-empty placeholder, NOT "".  An empty
    // value makes IDF store a value-less (boolean) TXT item; a later
    // mdns_service_txt_item_set with a real string then doesn't reliably
    // flip it to a key=value item, so the name never appears.  Seed with a
    // sentinel and overwrite it once the BLE scan has a real name.
    MDNS.addServiceTxt("hc33proxy", "tcp", "bonded_name", "none");

    s_mdns_up          = true;
    s_last_bonded_name = "none";
    // Serial.printf + flush, not ESP_LOGI: under UART overrun the [I] lines
    // get dropped (observed staircased output); these need to survive for
    // field diagnosis of discovery problems.
    Serial.printf("[mdns] advertising %s._hc33proxy._tcp.local:%d (variant=%s)\n",
                  hostname, TCP_PORT, kVariant);
    Serial.flush();
    return true;
}

void mdns_advert_refresh(const std::string& bonded_name) {
    if (!s_mdns_up) return;
    const std::string v = bonded_name.empty() ? "none" : bonded_name;
    if (v == s_last_bonded_name) return;

    // mdns_service_txt_item_set (via addServiceTxt) replaces a TXT key's
    // value when called again with the same key, and IDF re-announces the
    // service so fresh queries see the new value.  The const-char* overload
    // returns void, so there's no status to capture here.
    MDNS.addServiceTxt("hc33proxy", "tcp", "bonded_name", v.c_str());
    s_last_bonded_name = v;
    Serial.printf("[mdns] bonded_name TXT set -> %s\n", v.c_str());
    Serial.flush();
}
