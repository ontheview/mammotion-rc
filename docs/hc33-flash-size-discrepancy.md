# HC33 flash size: 16 MB hardware vs 8 MB in the build

**Status:** recorded, not acted on. Nothing in the build has been changed.

## What was observed

esptool reports **16 MB** of flash on the HT-HC33 during upload. The project
builds for **8 MB**:

| Where | Value |
|---|---|
| `firmware/platformio.ini` | `board_build.flash_size = 8MB` |
| `firmware/partitions/hc33.csv` | 8 MB layout (two ~3.5 MB app slots + 4 KB `config` at `0x7EF000`) |
| Heltec `boards.txt`, `HT-HC33.build.flash_size` | `8MB` |

So the mismatch is not between the repo and Heltec — the repo matches Heltec
exactly. It is between **Heltec's own board definition and the part actually
populated on the board**.

> Note on provenance: the 16 MB figure comes from an esptool run reported by
> the project owner. It has not been independently reproduced in this analysis.
> esptool derives it from the flash chip's JEDEC ID, which reads the silicon
> rather than any configuration, so it is normally trustworthy — but confirm it
> against a second board before treating it as a fleet-wide fact. Heltec may
> have changed the BOM mid-production.

## Does it have real impact?

**No, not in the current configuration.** Declaring less flash than is
physically present is the safe direction of the error:

- The partition table and both OTA slots live entirely inside the first 8 MB.
  Nothing addresses beyond it, so nothing can run off the end.
- The bootloader's flash-size header field is advisory for the ROM loader; an
  8 MB value on a 16 MB part is simply conservative.
- SPI mode and frequency are unaffected — they're set independently
  (`flash_mode=dio`, `boot=qio`).
- `scripts/flashcfg.py` writes the `config` partition at `0x7EF000`, well
  inside 8 MB.

The only cost is **capacity**: roughly 8 MB of the part is never addressed.

The dangerous direction would be the opposite — declaring 16 MB on an 8 MB
part, where writes past the boundary wrap or fail silently. That is not the
situation here.

## What it would buy, and what it would cost

Worth revisiting only if one of these becomes a real requirement:

- Larger OTA slots (the current app is ~1.0 MB against a 3.5 MB slot — no
  pressure at all today, even with the camera compiled in).
- On-board storage for recorded video or stills, which the camera work makes
  plausible for the first time.
- A larger LittleFS/SPIFFS region.

Against that, changing it is not free:

- `partitions/hc33.csv` would have to be re-laid-out, which **moves the
  `config` partition**. Every already-deployed HC33 would lose its stored
  runtime config on the next flash, and `config_load.cpp` would read a foreign
  partition until re-provisioned with `pio run -t flashcfg`.
- The prebuilt binaries served by `firmware/flasher/` would need rebuilding
  and re-testing, since the browser flasher writes fixed offsets.
- It would have to be verified on **every** board in use, not just the one
  that reported 16 MB. A 16 MB table on an 8 MB unit bricks it until
  re-flashed.

## Recommendation

Leave it at 8 MB. The mismatch is benign, the current app uses 28 % of one
app slot, and the migration cost falls on already-deployed devices. Revisit
only alongside a concrete need for the extra space — most likely on-board
video recording — and confirm the part on several boards first.
