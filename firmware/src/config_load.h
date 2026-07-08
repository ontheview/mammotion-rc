// Runtime configuration loaded from the `config` flash partition.
//
// End users get a prebuilt firmware binary (no recompile) and flash their
// network settings as a tiny blob into a dedicated partition via the browser
// flasher (ESP Web Tools) or, for dev, the `flashcfg` PlatformIO target.  This
// loader reads that blob at boot, validates it, and exposes the result in the
// global g_cfg.  If the partition is blank/foreign/corrupt it falls back to the
// compiled defaults in config.h — so a plain `pio run -t upload` (which never
// touches the config partition) just uses config.h, and the dev workflow is
// unchanged.
//
// One struct serves both firmware variants.  The HaLow build (env:hc33) uses
// every field; the standard-wifi build uses only uplink_ssid/uplink_pass and
// ignores region + softap_*.  The on-flash layout is therefore identical for
// both, which keeps the blob-packing code (pack_config.py and the browser JS)
// single-sourced.

#pragma once

#include <stdint.h>
#include <stddef.h>

// "Is this partition ours and a layout we understand?"  Bump HC33_CONFIG_VERSION
// (and add migration) if the field layout ever changes.
#define HC33_CONFIG_MAGIC    0x33334643u
// Bumped to 2 when softap_channel was added — distinguishes the on-flash
// layout cleanly from v1 (which had no channel field).  Any v1 blob in flash
// gets rejected with a version-mismatch log, not silently misread.
#define HC33_CONFIG_VERSION  2

// Custom data-partition subtype for `config` in partitions/hc33.csv.
#define HC33_CONFIG_SUBTYPE  0x99

// On-flash blob layout.  Packed so the C struct, pack_config.py and the browser
// JS all agree byte-for-byte.  Field sizes include room for the NUL terminator
// (SSID max 32 + 1, WPA passphrase max 63 + 1, region "AU" etc. 3 + 1).
struct __attribute__((packed)) hc33_config_v1 {
    uint32_t magic;             // HC33_CONFIG_MAGIC
    uint8_t  version;           // HC33_CONFIG_VERSION
    char     uplink_ssid[33];   // HaLow AP SSID  OR  home WiFi SSID
    char     uplink_pass[65];   // matching password
    char     region[4];         // HaLow regulatory code; ignored by WiFi build
    char     softap_ssid[33];   // HaLow only — SSID the mower joins
    char     softap_pass[65];   // HaLow only — password the mower uses
    uint8_t  softap_channel;    // HaLow only — 2.4 GHz channel for the softAP.
                                // Must be set per-HC33 when multiple deployments
                                // operate near each other, otherwise the radios
                                // step on each other.  Valid 1-14 (region-
                                // dependent); 0 or out-of-range triggers fallback
                                // to compiled AP_CHANNEL default at load time.
    uint32_t crc32;             // zlib CRC32 over the preceding bytes
};

// Resolved configuration for this boot.  Populated by config_load() — either
// from the partition (valid blob) or from config.h defaults (anything else).
// magic/version/crc32 are meaningless once resolved; only the char fields are
// consumed by the rest of the firmware.
extern hc33_config_v1 g_cfg;

// Read + validate the config partition into g_cfg, falling back to config.h
// defaults on any problem.  Logs which source won (password masked).  Call once
// early in setup(), before anything that reads g_cfg.
void config_load();
