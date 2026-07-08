#!/usr/bin/env python3
"""Pack an INI of user config into the on-flash blob that config_load.cpp reads.

This is the reference packer.  The browser-flasher JS (step 4) MUST produce the
exact same byte layout — keep this file as the single source of truth for the
format.

Usage:
    python3 pack_config.py <config.local.ini> <out.bin>

INI shape (see config.local.ini.example):
    [config]
    uplink_ssid = ...
    uplink_pass = ...
    region      = AU
    softap_ssid = ...
    softap_pass = ...
"""

import configparser
import struct
import sys
import zlib

# Must match HC33_CONFIG_MAGIC / HC33_CONFIG_VERSION in config_load.h.
MAGIC = 0x33334643
VERSION = 2   # v2 added softap_channel; v1 layout no longer accepted

# Field byte widths (incl. NUL terminator).  Must match hc33_config_v1.
WIDTH_SSID   = 33   # WiFi SSID max 32 + NUL
WIDTH_PASS   = 65   # WPA passphrase max 63 + NUL  (Heltec/WiFi APIs use 64; we pad to 65 for safety)
WIDTH_REGION = 4    # "AU"/"US"/.. + NUL


def fixed(s: str, n: int) -> bytes:
    """UTF-8 encode `s`, truncate to n-1 bytes, NUL-pad to n total."""
    b = s.encode("utf-8")[: n - 1]
    return b + b"\x00" * (n - len(b))


def pack(ini_path: str) -> bytes:
    cfg = configparser.ConfigParser()
    # Allow values containing % without ConfigParser interpolating.
    cfg = configparser.RawConfigParser()
    if not cfg.read(ini_path):
        sys.exit(f"pack_config: can't read {ini_path}")
    if "config" not in cfg:
        sys.exit(f"pack_config: {ini_path} has no [config] section")
    c = cfg["config"]

    body = struct.pack("<IB", MAGIC, VERSION)            # 5 bytes, no padding
    body += fixed(c.get("uplink_ssid", ""), WIDTH_SSID)
    body += fixed(c.get("uplink_pass", ""), WIDTH_PASS)
    body += fixed(c.get("region",      "AU"), WIDTH_REGION)
    body += fixed(c.get("softap_ssid", ""), WIDTH_SSID)
    body += fixed(c.get("softap_pass", ""), WIDTH_PASS)

    # softap_channel: validated here so a typo in the INI doesn't write a
    # blob the firmware will reject (firmware also clamps to 1-14 on load).
    channel = c.getint("softap_channel", fallback=6)
    if not 1 <= channel <= 14:
        sys.exit(f"pack_config: softap_channel={channel} out of range 1-14")
    body += struct.pack("<B", channel)

    # 5 + 33 + 65 + 4 + 33 + 65 + 1 = 206 bytes.  Sanity-check against drift.
    assert len(body) == 206, f"body length drift: {len(body)}"

    # zlib.crc32 is exactly what config_load.cpp's crc32_zlib() computes.
    crc = zlib.crc32(body) & 0xFFFFFFFF
    return body + struct.pack("<I", crc)


def main() -> None:
    if len(sys.argv) != 3:
        sys.exit("usage: pack_config.py <config.local.ini> <out.bin>")
    blob = pack(sys.argv[1])
    with open(sys.argv[2], "wb") as f:
        f.write(blob)
    print(f"pack_config: wrote {sys.argv[2]} ({len(blob)} bytes)")


if __name__ == "__main__":
    main()
