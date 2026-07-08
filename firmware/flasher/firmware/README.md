# Firmware binaries

The flasher reads these `.bin` files at runtime and writes them to the HC33
along with a user-generated config blob.

## Expected layout

```
firmware/
├── hc33-halow/
│   ├── bootloader.bin
│   ├── partitions.bin
│   ├── boot_app0.bin
│   └── firmware.bin
└── hc33-wifi/
    ├── bootloader.bin
    ├── partitions.bin
    ├── boot_app0.bin
    └── firmware.bin
```

Each file goes to a fixed flash offset — those are hardcoded in `app.js`
(`FLASH_PARTS`) and match `firmware/partitions/hc33.csv`:

| File              | Offset    |
|-------------------|-----------|
| `bootloader.bin`  | `0x000000` |
| `partitions.bin`  | `0x008000` |
| `boot_app0.bin`   | `0x00e000` |
| `firmware.bin`    | `0x010000` |
| `config.bin` (generated in browser) | `0x7EF000` |

## How to refresh after a firmware change

After a `pio run -e hc33` or `pio run -e hc33-standard-wifi`, the PIO build
output sits in:

```
<repo>/.pio/build/hc33/                    -> copy to firmware/hc33-halow/
<repo>/.pio/build/hc33-standard-wifi/      -> copy to firmware/hc33-wifi/
```

Copy the four `.bin` files (`bootloader.bin`, `partitions.bin`, `boot_app0.bin`,
`firmware.bin`) from each PIO build dir into the corresponding subdir here,
then commit and re-publish.  Browser will cache by URL so you don't need to
cache-bust unless filenames change.

`boot_app0.bin` is identical for both variants and rarely changes — it ships
with the Arduino/IDF toolchain.  If the file isn't present in your PIO output,
check the framework's `tools/partitions/` dir.

## Licensing (read before redistributing these binaries)

These prebuilt images are **not** pure GPL. The HaLow build
(`hc33-halow/firmware.bin`) embeds **proprietary Morse Micro** Wi-Fi HaLow
firmware/driver blobs (`libmorse`, `mm6108.mbin`, the BCF) licensed under the
**Morse Micro Binary Distribution License (BDL)**. Per that license you may
redistribute these images **only**:

- **complete and unmodified**, and **only for use with Morse Micro Wi-Fi HaLow
  hardware** (the HC33's MM6108 qualifies);
- **with the BDL text included** — kept in [`LICENSES/`](LICENSES/) in this
  folder, so it travels with the binaries; and
- with all Morse Micro copyright notices retained.

The rest of each image (this project + Heltec ESP_HaLow) is **GPL-3.0**; the
corresponding source is at the project repository. The standard-wifi build
(`hc33-wifi/`) contains no Morse components. See the repo-root
[`THIRD-PARTY-NOTICES.md`](../../../THIRD-PARTY-NOTICES.md) for the full picture,
and confirm the linked-`.bin` case with Morse Micro if you distribute
commercially.

**When you publish the flasher, keep the `LICENSES/` folder alongside the
`.bin` files** — do not strip it.
