/*
 * Runtime activation of NAPT.  Call this once after both the softAP and the
 * HaLow STA are up.  Extracted from our firmware (full source in
 * firmware/src/soft_ap.cpp) and trimmed to the NAPT-relevant lines only.
 *
 * Requires liblwip.a + libesp_netif.a built with:
 *   CONFIG_LWIP_IP_FORWARD=y
 *   CONFIG_LWIP_IPV4_NAPT=y
 *   CONFIG_LWIP_IPV4_NAPT_PORTMAP=y
 *   CONFIG_LWIP_L2_TO_L3_COPY=y
 *
 * (See sdkconfig.napt-additions in this folder.)
 */

#include <esp_log.h>
#include <esp_netif.h>
#include <esp_netif_net_stack.h>
#include <esp_wifi.h>
#include <lwip/netif.h>
#include <dhcpserver/dhcpserver.h>
#include "mmipal.h"   // for mmipal_get_lwip_netif()

static const char* TAG = "napt";

bool enable_napt_after_softap_and_halow_up(const char* upstream_dns_ip)
{
    /* 1. Get the AP netif handle.  WIFI_AP_DEF is the conventional key used by
     *    arduino-esp32's WiFi.softAP() and IDF's esp_netif default factories. */
    esp_netif_t* ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (ap_netif == nullptr) {
        ESP_LOGE(TAG, "AP netif not found — did softAP come up?");
        return false;
    }

    /* 2. Configure the AP's DHCP server to push a DNS server to clients.
     *    Without this, clients get an IP+gateway but no DNS, so even with NAPT
     *    working they can't resolve hostnames.  Must be done BEFORE softAP()
     *    starts dhcps for the option to take effect at first start; if you're
     *    enabling NAPT after softAP() is already up, you'll need to stop+
     *    restart dhcps for the DNS option to apply. */
    dhcps_offer_t offer_dns = OFFER_DNS;
    esp_netif_dhcps_option(ap_netif, ESP_NETIF_OP_SET,
                           ESP_NETIF_DOMAIN_NAME_SERVER,
                           &offer_dns, sizeof(offer_dns));

    esp_netif_dns_info_t dns_info = {};
    dns_info.ip.u_addr.ip4.addr = ipaddr_addr(upstream_dns_ip);
    dns_info.ip.type = ESP_IPADDR_TYPE_V4;
    esp_netif_set_dns_info(ap_netif, ESP_NETIF_DNS_MAIN, &dns_info);
    ESP_LOGI(TAG, "dhcps configured to advertise DNS=%s", upstream_dns_ip);

    /* 3. Enable NAPT on the AP netif.  This is the actual NAT translation:
     *    packets sourced from 192.168.4.X (softAP subnet) get their src addr
     *    rewritten to the HaLow STA address as they egress. */
    esp_err_t err = esp_netif_napt_enable(ap_netif);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif_napt_enable failed: %s (0x%x)",
                 esp_err_to_name(err), err);
        return false;
    }
    ESP_LOGI(TAG, "NAPT enabled on AP netif");

    /* 4. Make HaLow the default route.  Without this, NAPT decides to forward
     *    a packet but lwIP has no egress interface (the AP netif's default
     *    isn't applicable for outbound traffic) and the packet is silently
     *    dropped.  mmipal_init() does NOT call netif_set_default() — verified
     *    by reading the source — so this is on us. */
    struct netif* halow_netif = mmipal_get_lwip_netif();
    if (halow_netif == nullptr) {
        ESP_LOGE(TAG, "HaLow netif not found — did HaLow associate?");
        return false;
    }
    netif_set_default(halow_netif);
    ESP_LOGI(TAG, "default route set to HaLow netif (ip=%d.%d.%d.%d)",
             ip4_addr1(&halow_netif->ip_addr.u_addr.ip4),
             ip4_addr2(&halow_netif->ip_addr.u_addr.ip4),
             ip4_addr3(&halow_netif->ip_addr.u_addr.ip4),
             ip4_addr4(&halow_netif->ip_addr.u_addr.ip4));

    return true;
}
