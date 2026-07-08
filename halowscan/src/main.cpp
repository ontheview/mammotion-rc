// Standalone HaLow scan: initializes the MM6108 and scans for HaLow APs every
// 5s.  Reports SSID / RSSI / BSSID / encryption for each one found.
//
// This is the Heltec HalowScan.ino example (libraries/wifi-halow/examples/
// HalowScan/) verbatim, except:
//   - region is "AU" (not "US") — the HD01 advertises on AU channels
//   - converted from .ino to .cpp for PIO with an explicit Arduino.h include
//
// Use this to sanity-check the MM6108 RX path independent of our main
// firmware.  The scan RSSI comes from mmwlan_scan's per-beacon measurement,
// a different code path than the live mmwlan_get_rssi() we use in main.cpp —
// if scan-RSSI and live-RSSI disagree by tens of dB for the same setup, the
// offset is in the live-read path, not the radio.

#include <Arduino.h>
#include "HaLow.h"

void setup() {
  Serial.begin(115200);
  delay(200);

  // The Heltec example here has an `#ifdef HT-RC3268` block enabling an LDO
  // control pin.  The HC33 doesn't need it (different board with the LDO
  // wired directly to power), and `HT-RC3268` isn't a valid C identifier so
  // the ifdef behaves unpredictably across toolchains.  Just dropped.

  HaLow.init("AU");

  Serial.println("Setup done — region AU, scanning every 5s");
}

void loop() {
  Serial.println("Scan start");

  int n = HaLow.scanNetworks();
  Serial.println("Scan done");

  if (n == 0) {
    Serial.println("no networks found");
  } else {
    Serial.print(n);
    Serial.println(" networks found");
    Serial.printf("Nr | %-32.32s | RSSI | %-17.17s | Encryption\r\n", "SSID", "BSSID");
    for (int i = 0; i < n; i++) {
      Serial.printf("%2d", i + 1);
      Serial.print(" | ");
      Serial.printf("%-32.32s", HaLow.SSID(i).c_str());
      Serial.print(" | ");
      Serial.printf("%4ld", HaLow.RSSI(i));
      Serial.print(" | ");
      Serial.print(HaLow.BSSIDstr(i));
      Serial.print(" | ");

      switch (HaLow.encryptionType(i)) {
        case MMWLAN_OPEN:    Serial.print("OPEN");    break;
        case MMWLAN_OWE:     Serial.print("OWE");     break;
        case MMWLAN_SAE:     Serial.print("SAE");     break;
        case MMWLAN_OTHER:
        default:             Serial.print("UNKNOWN");
      }
      Serial.println();
      delay(10);
    }
  }

  Serial.println("");
  HaLow.scanDelete();
  delay(5000);
}
