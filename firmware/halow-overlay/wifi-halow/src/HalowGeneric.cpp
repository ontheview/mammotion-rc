// Path C re-implementation of HalowGenericClass.
//
// Heltec ships these methods inside libheltec_halow.a (closed source).  In
// Path C we drop that archive, so we provide the methods ourselves on top of
// lwIP DNS + a FreeRTOS event group.
//
// Only the methods actually called by the other Halow* wrappers
// (HalowClient, HalowScan, HalowSTA) are implemented.

#include "HalowGeneric.h"

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <lwip/netdb.h>
#include <lwip/ip_addr.h>
#include <string.h>

// ----- Status bits backed by a FreeRTOS event group ------------------------

static EventGroupHandle_t s_status_eg = nullptr;

static EventGroupHandle_t status_eg(void) {
    if (s_status_eg == nullptr) {
        s_status_eg = xEventGroupCreate();
    }
    return s_status_eg;
}

int HalowGenericClass::getStatusBits() {
    return (int)xEventGroupGetBits(status_eg());
}

int HalowGenericClass::setStatusBits(int bits) {
    return (int)xEventGroupSetBits(status_eg(), (EventBits_t)bits);
}

int HalowGenericClass::clearStatusBits(int bits) {
    return (int)xEventGroupClearBits(status_eg(), (EventBits_t)bits);
}

int HalowGenericClass::waitStatusBits(int bits, uint32_t timeout_ms) {
    return (int)xEventGroupWaitBits(status_eg(),
                                    (EventBits_t)bits,
                                    pdFALSE, pdFALSE,
                                    pdMS_TO_TICKS(timeout_ms));
}

// ----- hostByName via lwIP getaddrinfo -------------------------------------

int HalowGenericClass::hostByName(const char *aHostname, IPAddress &aResult,
                                  bool /*preferV6*/) {
    if (aHostname == nullptr) return 0;

    ip_addr_t ipaddr;
    if (ipaddr_aton(aHostname, &ipaddr)) {
        aResult = IPAddress(ip4_addr_get_u32(ip_2_ip4(&ipaddr)));
        return 1;
    }

    struct addrinfo hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *res = nullptr;
    if (lwip_getaddrinfo(aHostname, nullptr, &hints, &res) != 0 || res == nullptr) {
        return 0;
    }
    int rc = 0;
    for (struct addrinfo *p = res; p != nullptr; p = p->ai_next) {
        if (p->ai_family == AF_INET) {
            struct sockaddr_in *sa = (struct sockaddr_in *)p->ai_addr;
            aResult = IPAddress((uint32_t)sa->sin_addr.s_addr);
            rc = 1;
            break;
        }
    }
    lwip_freeaddrinfo(res);
    return rc;
}

// ----- IP math helpers (pure functions, no state) --------------------------

IPAddress HalowGenericClass::calculateNetworkID(IPAddress ip, IPAddress subnet) {
    IPAddress out;
    for (int i = 0; i < 4; ++i) out[i] = ip[i] & subnet[i];
    return out;
}

IPAddress HalowGenericClass::calculateBroadcast(IPAddress ip, IPAddress subnet) {
    IPAddress out;
    for (int i = 0; i < 4; ++i) out[i] = ip[i] | (~subnet[i]);
    return out;
}

uint8_t HalowGenericClass::calculateSubnetCIDR(IPAddress subnetMask) {
    uint8_t cidr = 0;
    for (int i = 0; i < 4; ++i) {
        uint8_t b = subnetMask[i];
        while (b & 0x80) { ++cidr; b <<= 1; }
    }
    return cidr;
}

// ----- Minimal no-ops for symbols referenced but unused in this firmware ---

HalowGenericClass::HalowGenericClass() {}

halow_event_id_t HalowGenericClass::onEvent(HalowEventCb, halow_event_id_t) {
    return 0;
}
void HalowGenericClass::removeEvent(HalowEventCb, halow_event_id_t) {}

int32_t HalowGenericClass::channel(void)         { return 0; }
void    HalowGenericClass::persistent(bool)      {}
void    HalowGenericClass::enableLongRange(bool) {}
const char * HalowGenericClass::eventName(halow_event_id_t) { return ""; }

esp_err_t HalowGenericClass::_eventCallback(arduino_halow_event_t *) { return ESP_OK; }

static bool s_static_buffers = false;
void HalowGenericClass::useStaticBuffers(bool b) { s_static_buffers = b; }
bool HalowGenericClass::useStaticBuffers()       { return s_static_buffers; }

// ----- halowtcpipInit stub --------------------------------------------------
//
// In Heltec's build this called tcpip_init() and pre-registered the netif
// before the chip was up.  In Path C, mmipal_init() handles all of that, so
// HalowSTA.cpp's call here becomes a no-op.

bool halowtcpipInit() { return true; }

// ----- getID() — Heltec exposes the eFuse chip ID; we don't use it, stub.

uint64_t getID() { return 0; }
