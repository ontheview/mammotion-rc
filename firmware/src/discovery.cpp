#include "discovery.h"

#include <Arduino.h>
#include <esp_mac.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lwip/sockets.h>
#include <cerrno>
#include <cstring>

#ifdef USE_STANDARD_WIFI
static const char* kVariant = "wifi";
#else
static const char* kVariant = "halow";
#endif

// Probe prefix the server sends.  We match only the prefix so a version
// suffix (e.g. "HC33-DISCOVER?1") can evolve without breaking older firmware.
static const char kProbe[] = "HC33-DISCOVER?";

static uint16_t                      s_proxy_port = 0;
static std::function<std::string()>  s_bonded_provider;

// "A49DF4" from the last 3 bytes of the base MAC — same id the mDNS path uses
// so a proxy is identified consistently regardless of discovery mechanism.
static void chip_id_hex(char out[7]) {
    uint8_t mac[6] = {0};
    esp_efuse_mac_get_default(mac);
    snprintf(out, 7, "%02X%02X%02X", mac[3], mac[4], mac[5]);
}

static void discovery_task(void* /*arg*/) {
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        Serial.printf("[discovery] socket() failed errno=%d — responder off\n", errno);
        Serial.flush();
        vTaskDelete(nullptr);
        return;
    }
    int yes = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));

    struct sockaddr_in bind_addr{};
    bind_addr.sin_family      = AF_INET;
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    bind_addr.sin_port        = htons(HC33_DISCOVERY_PORT);
    if (bind(sock, (struct sockaddr*)&bind_addr, sizeof(bind_addr)) < 0) {
        Serial.printf("[discovery] bind(:%d) failed errno=%d — responder off\n",
                      HC33_DISCOVERY_PORT, errno);
        Serial.flush();
        close(sock);
        vTaskDelete(nullptr);
        return;
    }

    char chip_id[7];
    chip_id_hex(chip_id);
    Serial.printf("[discovery] listening on UDP :%d (chip_id=%s variant=%s)\n",
                  HC33_DISCOVERY_PORT, chip_id, kVariant);
    Serial.flush();

    char rx[64];
    for (;;) {
        struct sockaddr_in from{};
        socklen_t fromlen = sizeof(from);
        int n = recvfrom(sock, rx, sizeof(rx) - 1, 0,
                         (struct sockaddr*)&from, &fromlen);
        if (n <= 0) {
            // Transient error (e.g. netif down mid-call).  Brief backoff so a
            // hard error loop can't peg the CPU; normal blocking recv returns
            // here only on real traffic.
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        rx[n] = '\0';
        if (strncmp(rx, kProbe, sizeof(kProbe) - 1) != 0) continue;

        std::string bonded = s_bonded_provider ? s_bonded_provider() : std::string();
        if (bonded.empty()) bonded = "none";

        char reply[160];
        int rn = snprintf(reply, sizeof(reply),
            "HC33-PROXY chip_id=%s variant=%s proxy_port=%u bonded_name=%s",
            chip_id, kVariant, (unsigned)s_proxy_port, bonded.c_str());
        if (rn > 0) {
            sendto(sock, reply, (size_t)rn, 0, (struct sockaddr*)&from, fromlen);
        }

        // Format the source IP from the raw network-order address — avoids
        // depending on lwIP's inet_ntoa_r macro signature (varies by version).
        uint32_t a = ntohl(from.sin_addr.s_addr);
        Serial.printf("[discovery] probe from %u.%u.%u.%u:%u -> replied (bonded=%s)\n",
                      (unsigned)((a >> 24) & 0xFF), (unsigned)((a >> 16) & 0xFF),
                      (unsigned)((a >> 8) & 0xFF), (unsigned)(a & 0xFF),
                      (unsigned)ntohs(from.sin_port), bonded.c_str());
        Serial.flush();
    }
}

bool discovery_begin(uint16_t proxy_port,
                     std::function<std::string()> bonded_name_provider) {
    s_proxy_port      = proxy_port;
    s_bonded_provider = std::move(bonded_name_provider);
    // Low priority: discovery is best-effort and must never starve the TCP
    // proxy / BLE / HaLow tasks.  4 KB stack covers the lwIP socket calls and
    // the snprintf with margin.
    BaseType_t ok = xTaskCreate(discovery_task, "hc33_discovery", 4096,
                                nullptr, tskIDLE_PRIORITY + 1, nullptr);
    if (ok != pdPASS) {
        Serial.printf("[discovery] xTaskCreate failed — responder off\n");
        Serial.flush();
    }
    return ok == pdPASS;
}
