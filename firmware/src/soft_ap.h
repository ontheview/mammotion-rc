// SoftAP for the mower (Mode 2 / HaLow env only).
//
// Brings up the S3's built-in 2.4 GHz radio as an AP, hands out DHCP leases,
// and NAPTs outbound traffic onto the HaLow STA netif so the mower can reach
// the Mammotion cloud through the HC33.
//
// Not compiled in the standard-wifi env — there the S3 radio is the STA and
// there's no second radio to spare for an AP.

#pragma once

#ifndef USE_STANDARD_WIFI

// Bring up the softAP and enable NAPT.  Returns true on success.  Call AFTER
// net_connect() — NAPT enable needs the HaLow netif to already exist so the
// lwIP routing tables can route AP→HaLow.  Idempotent: returns true
// immediately if the AP is already up.
bool soft_ap_begin();

// Tear down the softAP.  Stops the WiFi AP and DHCP server so the mower
// (or any other client) no longer sees the SSID.  Called when HaLow goes
// down — there's no point advertising an AP whose uplink is dead, and a
// reachable-looking SSID prevents the mower from roaming to a better one.
void soft_ap_stop();

// True iff the softAP is currently being broadcast.
bool soft_ap_is_up();

#endif  // !USE_STANDARD_WIFI
