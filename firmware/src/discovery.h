// UDP broadcast discovery responder.
//
// Role-flipped from mDNS: instead of the HC33 advertising and the server
// browsing, the web-server onboarding broadcasts a probe to
// HC33_DISCOVERY_PORT and every HC33 on the segment replies (unicast) with
// its chip_id / variant / proxy port / bonded mower name.
//
// Why not mDNS on HaLow: lwIP's mDNS responder needs per-netif client_data,
// which can't be enabled without changing struct netif's layout/size and
// breaking ABI with Heltec's precompiled libmmipal.a/libmorse.a (see memory:
// idf-mdns-empty-txt-quirk and heltec-lwip-rebuild).  This responder is pure
// BSD sockets — it sits above lwIP, so it works identically on the HaLow raw
// netif and the standard-wifi esp_netif with no rebuild and no ABI risk.
//
// Wire protocol (ASCII, UDP):
//   probe (server -> broadcast):  "HC33-DISCOVER?<ver>"   (we match the prefix)
//   reply (HC33  -> unicast):     "HC33-PROXY chip_id=A49DF4 variant=wifi "
//                                 "proxy_port=9876 bonded_name=Luba-XXXXXXXX"
//   bonded_name is "none" until the BLE scan picks a mower.  The server reads
//   the proxy's IP from the reply's source address, not the payload.

#pragma once

#include <cstdint>
#include <functional>
#include <string>

#define HC33_DISCOVERY_PORT 9878

// Start the responder task.  proxy_port is advertised so the server knows
// which TCP port to open.  bonded_name_provider is invoked per-reply so the
// answer always reflects the latest BLE scan result (empty -> "none").
// Returns false if the task couldn't be created.
bool discovery_begin(uint16_t proxy_port,
                     std::function<std::string()> bonded_name_provider);
