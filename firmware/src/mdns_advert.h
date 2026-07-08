// mDNS service advertisement so the web-server onboarding flow can discover
// HC33 proxies on the LAN without needing the user to type IPs.
//
// Published as:
//   hostname:  hc33-XXXXXX.local        (XXXXXX = last 6 hex chars of base MAC)
//   service:   _hc33proxy._tcp on TCP_PORT
//   TXT:       chip_id=XXXXXX
//              variant=halow|wifi
//              bonded_name=Luba-XXXXXXXX    (empty until BLE scan picks a mower)
//
// Server-side discovery: browse `_hc33proxy._tcp.local` (e.g. via Python's
// zeroconf lib) and match `bonded_name` TXT against the cloud-side device
// name from list_binding_by_account.  An empty bonded_name means the HC33
// hasn't scanned yet — UI should prompt the user to power on the mower.

#pragma once

#include <string>

// Bring up mDNS, set hostname, register the _hc33proxy._tcp service with
// initial TXT records.  Call after net_connect() has an IP.  Returns false
// on any mDNS init/register failure (logged); proxy keeps working without
// discovery.
bool mdns_advert_begin();

// Re-publish the bonded_name TXT record if it differs from what was last
// advertised.  Cheap no-op when unchanged.  Safe to call from the main loop
// at any cadence; ~1 Hz is plenty.
void mdns_advert_refresh(const std::string& bonded_name);
