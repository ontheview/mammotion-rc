// Thin compatibility layer over Arduino-ESP32's WiFi vs Heltec's ESP_HaLow.
//
// Picks one network API based on the USE_STANDARD_WIFI compile flag and
// exposes uniform NetServer / NetClient types so the rest of the firmware
// (tcp_proxy.{h,cpp}) doesn't have to care which radio the bytes ride on.
//
// The two libraries are parallel — same shape, different names:
//
//   Standard WiFi (Arduino-ESP32):   HaLow (Heltec):
//   --------------------------------- ---------------------------------------
//   #include <WiFi.h>                 #include <HaLow.h>
//   class WiFiServer { ... }          class HalowServer { ... }
//   class WiFiClient { ... }          class HalowClient { ... }
//   WiFiClass WiFi;                   HalowClass HaLow;
//   WiFi.begin(ssid, pass)            HaLow.begin(ssid, pass, security, region)
//
// Network association lives in main.cpp because the begin() signatures don't
// line up cleanly — HaLow demands a security type + regulatory region.

#pragma once

#ifdef USE_STANDARD_WIFI
  #include <WiFi.h>
  using NetServer = WiFiServer;
  using NetClient = WiFiClient;
#else
  // HaLow.h is just the STA/Scan umbrella; it does NOT transitively pull in
  // the server/client classes.  Include them explicitly.
  #include <HaLow.h>
  #include <HalowServer.h>
  #include <HalowClient.h>
  using NetServer = HalowServer;
  using NetClient = HalowClient;
#endif
