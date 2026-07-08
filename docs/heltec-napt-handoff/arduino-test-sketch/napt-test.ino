/*
 * Minimal Arduino test sketch for verifying NAPT on HT-HC33.
 *
 * Flow:
 *   1. Brings up HaLow STA (associates to your existing HaLow AP)
 *   2. Brings up 2.4 GHz softAP "NaptTest" on 192.168.4.0/24
 *   3. Enables NAPT on the AP netif
 *   4. Sets HaLow as default route
 *   5. Prints status every 5 s
 *
 * Test by connecting a phone/laptop to "NaptTest" (password: napt-test-1234)
 * and trying to reach the LAN side / browse the internet.  All traffic
 * should egress via the HaLow uplink.
 *
 * Requires liblwip.a + libesp_netif.a rebuilt with the NAPT sdkconfig items
 * (see ../sdkconfig.napt-additions) and the two header patches (see
 * ../patches/).
 *
 * Edit HALOW_SSID / HALOW_PASS / HALOW_REGION to match your HaLow AP.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <esp_netif.h>
#include <esp_netif_net_stack.h>
#include <lwip/netif.h>
#include <dhcpserver/dhcpserver.h>

#include "halow_init.h"  // Heltec's HaLow bring-up helper
#include "mmwlan.h"
#include "mmipal.h"

/* === EDIT THESE === */
#define HALOW_SSID      "YourHaLowSSID"
#define HALOW_PASS      "YourHaLowPassword"
#define HALOW_REGION    "AU"           // or "US", "EU", etc.

#define AP_SSID         "NaptTest"
#define AP_PASSWORD     "napt-test-1234"
#define AP_CHANNEL      6
#define AP_IP           "192.168.4.1"
#define AP_GATEWAY      "192.168.4.1"
#define AP_NETMASK      "255.255.255.0"
#define UPSTREAM_DNS    "192.168.1.1"   // your HaLow side's router or 8.8.8.8

/* === Implementation === */
static const char* TAG = "napt-test";

static bool halow_associate() {
    Serial.printf("[%s] HaLow init for region %s ...\n", TAG, HALOW_REGION);
    while (!halow_init(HALOW_REGION)) {
        Serial.printf("[%s] HaLow init returned false — retrying in 5s\n", TAG);
        delay(5000);
    }

    struct mmwlan_sta_args sta_args = MMWLAN_STA_ARGS_INIT;
    strncpy((char*)sta_args.ssid, HALOW_SSID, sizeof(sta_args.ssid) - 1);
    sta_args.ssid_len = strlen((const char*)sta_args.ssid);
    strncpy(sta_args.passphrase, HALOW_PASS, sizeof(sta_args.passphrase) - 1);
    sta_args.passphrase_len = strlen(sta_args.passphrase);
    sta_args.security_type = WIFI_SECURITY_HALOW;

    enum mmwlan_status st = mmwlan_sta_enable(&sta_args, nullptr);
    if (st != MMWLAN_SUCCESS) {
        Serial.printf("[%s] mmwlan_sta_enable rc=%d\n", TAG, (int)st);
        return false;
    }
    uint32_t t = millis();
    while (mmwlan_get_sta_state() != MMWLAN_STA_CONNECTED) {
        if (millis() - t > 30000) {
            Serial.printf("[%s] HaLow associate timeout\n", TAG);
            return false;
        }
        delay(200);
    }
    Serial.printf("[%s] HaLow associated\n", TAG);
    return true;
}

static bool softap_with_napt() {
    if (!WiFi.mode(WIFI_AP)) {
        Serial.printf("[%s] WiFi.mode(WIFI_AP) failed\n", TAG);
        return false;
    }

    esp_netif_t* ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (ap_netif != nullptr) {
        dhcps_offer_t offer_dns = OFFER_DNS;
        esp_netif_dhcps_option(ap_netif, ESP_NETIF_OP_SET,
                               ESP_NETIF_DOMAIN_NAME_SERVER,
                               &offer_dns, sizeof(offer_dns));

        esp_netif_dns_info_t dns_info = {};
        dns_info.ip.u_addr.ip4.addr = ipaddr_addr(UPSTREAM_DNS);
        dns_info.ip.type = ESP_IPADDR_TYPE_V4;
        esp_netif_set_dns_info(ap_netif, ESP_NETIF_DNS_MAIN, &dns_info);
    }

    IPAddress ip, gw, mask;
    ip.fromString(AP_IP);
    gw.fromString(AP_GATEWAY);
    mask.fromString(AP_NETMASK);
    WiFi.softAPConfig(ip, gw, mask);

    if (!WiFi.softAP(AP_SSID, AP_PASSWORD, AP_CHANNEL)) {
        Serial.printf("[%s] softAP failed\n", TAG);
        return false;
    }
    Serial.printf("[%s] softAP up: SSID=%s IP=%s\n",
                  TAG, AP_SSID, WiFi.softAPIP().toString().c_str());

    /* Enable NAPT on the AP netif */
    esp_err_t err = esp_netif_napt_enable(ap_netif);
    if (err != ESP_OK) {
        Serial.printf("[%s] esp_netif_napt_enable failed: %s (0x%x)\n",
                      TAG, esp_err_to_name(err), err);
        return false;
    }
    Serial.printf("[%s] NAPT enabled on AP netif\n", TAG);

    /* HaLow as default route */
    struct netif* halow_netif = mmipal_get_lwip_netif();
    netif_set_default(halow_netif);
    Serial.printf("[%s] default route set to HaLow netif\n", TAG);

    return true;
}

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("\n=== NAPT test sketch ===");

    if (!halow_associate()) {
        Serial.printf("[%s] HaLow setup failed — halting\n", TAG);
        while (true) delay(1000);
    }

    char ip_str[48] = {0};
    uint32_t t = millis();
    while (!halow_get_ip(ip_str, sizeof(ip_str))) {
        if (millis() - t > 30000) {
            Serial.printf("[%s] HaLow IP timeout — halting\n", TAG);
            while (true) delay(1000);
        }
        delay(500);
    }
    Serial.printf("[%s] HaLow IP: %s\n", TAG, ip_str);

    if (!softap_with_napt()) {
        Serial.printf("[%s] softAP+NAPT setup failed — halting\n", TAG);
        while (true) delay(1000);
    }

    Serial.printf("[%s] Ready.  Associate a client to '%s' and test\n",
                  TAG, AP_SSID);
}

void loop() {
    delay(5000);
    Serial.printf("[%s] HaLow RSSI=%ddBm  stations=%u\n",
                  TAG, (int)mmwlan_get_rssi(),
                  (unsigned)WiFi.softAPgetStationNum());
}
