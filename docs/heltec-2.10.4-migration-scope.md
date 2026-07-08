# Migration scope: Heltec ESP_HaLow 2.8.2 → 2.10.4

**Status (2026-07-07):** analysis only — no code changed. Our firmware tree
builds against a hand-patched **2.8.2-release** morselib (which already fixes the
TX wedge). Upstream Heltec shipped **2.10.4-esp32** on 2026-07-07 (`ESP_HaLow`
`origin/main`, commit `8ff9178`). 2.10.4 is a **Morse Micro MM-IoT-SDK generation
jump**, not a drop-in: 73 files changed in `wifi-halow` (+6568 / −1896), incl.
`mmwlan.h` alone +2760 lines. There is **no urgency** — migrate deliberately,
because the payoff is retiring several of our fragile HaLow hacks, not the wedge
fix (we already have that).

Do this on a branch, with a mower available for validation. The header analysis
below is done; only on-hardware checks remain (flagged **[HW]**).

---

## Version facts

| Component | Ours now | 2.10.4 upstream |
|---|---|---|
| morselib | `2.8.2-esp32` (manual swap over the shipped `2.7.3-…-dbg`) | `2.10.4-esp32` **release** (`MM_VERSION_BUILDID`, `mmversion.h`) |
| `libmm6108.a` (chip fw) | wraps `mm6108_1_15_3.mbin` (~370 KB) | new (~400 KB); exact chip-fw version **[HW]** via `mmwlan_get_version()` |
| `liblwip.a` / `libesp_netif.a` | **bundled** in `wifi-halow/src/`, NAPT-patched by us | **deleted** from `wifi-halow/src/` — now expected from the IDF libs |

Related memory: [[heltec-hc33-bcf]], [[heltec-libmorse-header-drift]],
[[heltec-lwip-rebuild]], [[halow-tx-blocking-root-cause]],
[[mmnetif-link-state-assert]], [[lwip-napt-udp-timeout-too-short]].

---

## Bug-fix delta since 2.8.2 — do we actually need this port?

Reviewed the Morse Micro MM-IoT-SDK release notes (2.8.2 → 2.9.7 → 2.10.4)
against our usage. **Conclusion: no single critical must-have bugfix forces an
urgent port.** Our worst issue — the TX wedge — was already fixed by the
debug→release morselib swap (2.7.3-dbg → 2.8.2-release); none of the 2.9/2.10
changes are "the wedge fix." Source: `MM-IoT-SDK-Software-Release-Notes.pdf`
(morsemicro.com), corroborated by the local `mm-iot-esp32` tags.

Morse chip firmware progression: **2.8.2 = 1.15.3 · 2.9.7 = 1.16.4 · 2.10.4 = 1.17.6**
(the `libmm6108.a`/mbin + BCF re-pairing in §F is exactly this).

### Fixes we'd GAIN that are relevant to us (MM6108, STA + our own softAP/NAPT)
- **Supplicant could get stuck in "scanning" forever** (fixed 2.10.4) — the most
  relevant: a reconnect-wedge failure mode for a headless always-on proxy that
  must self-recover after a HaLow drop.
- **Action frames dropped under Open security → low throughput** (known issue in
  2.9.7, **fixed 2.10.4**). Relevant *only if* running `MMWLAN_OPEN` (our
  `config.h` default). **Mitigable now without porting** — switch HD01 + HC33 to
  `MMWLAN_SAE` (WPA3).
- **Stations continuously sending QOS_NULL every 100 ms** (fixed 2.10.4) —
  needless medium usage; tangential to our medium-contention/wedge history.
- **Improved 4-way-handshake robustness (STA) + disconnect-on-beacon-loss**
  (2.10.4) — association/reconnect reliability.
- **LWIP↔MMOSAL timeout discrepancy fixed** (2.10.4) — touches our lwIP glue; minor.

### Confirmed NOT relevant (the bulk of the changelog)
MMAGIC fixes (we don't use MMAGIC), all MM8108-specific items (different chip),
TWT / RAW / DPP, MM6108-as-AP fixes (our AP is the S3 radio, not the MM6108),
and JP/EU/GB duty-cycle/regulatory fixes (we're AU).

### Known issues STILL OPEN in 2.10.4 (porting won't fix)
- "Some link instability at specific signal levels → elevated packet error rate"
  is still listed as an open stability known-issue. So 2.10.4 does **not** claim
  to fully solve marginal-signal link instability — our RSSI/antenna work isn't
  obsoleted by porting.

### Near-term actions
1. **No port needed:** if on `MMWLAN_OPEN`, move to `MMWLAN_SAE` to dodge the
   action-frame-drop/low-throughput issue today.
2. **When porting to 2.10.4:** additionally collect the supplicant-scan-stuck
   fix, handshake/beacon-loss robustness, and fw 1.17.6.

---

## API drift vs our call sites

Of the 28 `mm*` driver calls our firmware makes, **25 survive by name**; **3
break**, and they are exactly our netif/TX integration layer. (Name-survival ≠
signature-survival — see §G.)

### A. Obtaining the HaLow netif — `mmipal_get_lwip_netif()` REMOVED
- **Call sites:** `firmware/src/soft_ap.cpp:121` (install TX-fix) and `:295`.
- 2.10.4 has **no public accessor**. The netif now lives as a field
  `struct netif lwip_mmnetif;` inside `struct mmipal_data` (`mmnetif.h`), but
  nothing exposes that struct.
- **Options to resolve [HW/design]:**
  1. Walk lwIP's `netif_list` and pick the HaLow netif by its 2-char name
     (Morse uses `"ms"`/`"mm"`-style; confirm on-target) — most robust.
  2. If a `mmipal_get_data()`-style accessor turns up in the full header set,
     use `data->lwip_mmnetif`.
- This is the single trickiest item; everything in §C depends on it.

### B. Link state — `mmnetif_link_state()` REMOVED → callback API (a WIN)
- **Call site:** `firmware/src/soft_ap.cpp:166` (the source of our
  "sched callback failed" mailbox-overflow assert, [[mmnetif-link-state-assert]]).
- 2.10.4 replaces it with a proper callback model (`mmipal.h`):
  - `void mmipal_set_link_status_callback(mmipal_link_status_cb_fn_t fn);`
  - `void mmipal_set_ext_link_status_callback(fn, void *arg);`
  - `enum mmipal_link_state mmipal_get_link_state(void);`
  - payload `struct mmipal_link_status`.
- **Action:** register a callback instead of poking link state manually. This
  very likely **retires** the assert workaround (the manual call into the tcpip
  thread was what overflowed the mailbox) — re-test whether the
  `CONFIG_LWIP_TCPIP_RECVMBOX_SIZE=32` bump is still needed afterward.

### C. Non-blocking TX fix — `mmnetif_tx` now INTERNAL to `libmmipal.a`
- **File:** `firmware/src/halow_tx_nonblock.cpp` (installs a replacement
  `netif->linkoutput` with a bounded `mmwlan_tx_wait_until_ready(50 ms)`).
- **Good news — the body ports nearly verbatim.** Every primitive it uses is
  still public in 2.10.4: `mmwlan_tx_wait_until_ready`,
  `mmwlan_alloc_mmpkt_for_tx`, `mmpkt_open` / `mmpkt_append_data` / `mmpkt_close`,
  `mmwlan_tx_pkt`. Only the **install site** changes (needs the netif from §A;
  `halow_tx_nonblock.cpp:64` sets `halow_netif->linkoutput`).
- **Verify [HW/compile]:**
  - `mmwlan_tx_pkt`'s 2nd-arg metadata type (we pass `nullptr`) and the `mmpkt_*`
    signatures — a compile confirms.
  - **Whether the override is still needed at all:** does 2.10.4's `mmnetif_init`
    still route egress through `netif->linkoutput`? And is upstream's own TX path
    now non-blocking? If the reorg made TX non-blocking, our fix is **obviated** —
    grep the new path for a blocking `mmwlan_tx_wait_until_ready(...)` with a long
    timeout. If gone, delete `halow_tx_nonblock.*` entirely.

### D. IP config — `mmipal_get_ip_config()` survives
- **Call site:** `firmware/src/halow_init.cpp:123`. Present in 2.10.4
  (`mmipal.h:332`). Verify `struct mmipal_ip_config` layout is unchanged
  [compile]. The `struct netif` offset debug prints (`halow_init.cpp:92-95`) are
  cosmetic and safe even if the struct grew.

### E. Link/build overhaul — the removed lwIP archives (the pivotal one)
- **`platformio.ini` `env:hc33`** links our patched libs from
  `-L …/wifi-halow/src` (`liblwip.a`, `libesp_netif.a`) and sets struct-layout
  defines (`-DLWIP_NETIF_LINK_CALLBACK=1`, `-DCONFIG_LWIP_NETIF_STATUS_CALLBACK=1`,
  …). Those archives are **gone** in 2.10.4; lwIP/esp_netif now come from the IDF
  libs (`framework-arduinoespressif32-libs`, via `tools/get.py`).
- **Why this might DELETE our whole lwip-rebuild hack:** 2.10.4's
  `halow_config.h` now defines `CONFIG_LWIP_IPV4_NAPT=1`, `CONFIG_LWIP_IP_FORWARD=1`,
  `CONFIG_LWIP_L2_TO_L3_COPY=1`, `CONFIG_LWIP_NETIF_API=1` — exactly the options we
  rebuilt `liblwip.a` from source to get ([[heltec-lwip-rebuild]]).
- **PIVOTAL CHECK [HW/build]:** after `git clone`+`tools/get.py`, inspect the
  IDF `liblwip.a` for the NAPT symbols our patched one exports:
  ```
  nm <esp32-arduino-libs>/esp32s3/lib/liblwip.a 2>/dev/null | grep -i ip_napt
  # baseline (our patched lib exports): ip_napt_table, ip_napt_max, napt_free, …
  ```
  - **Symbols present →** drop our lwip rebuild entirely: remove the `-L` to our
    libs, the struct-layout `-D` defines, and stop staging `liblwip.a`/
    `libesp_netif.a`. Big simplification.
  - **Symbols absent →** we still must ship a NAPT-patched `liblwip.a`, now
    rebuilt against the 2.10.4 IDF base; and re-apply the UDP NAPT timeout bump
    ([[lwip-napt-udp-timeout-too-short]]). Re-check whether
    `LWIP_NETIF_LINK_CALLBACK` is still needed given §B's new callback API.

### F. Chip firmware / BCF / regdb
- New `libmm6108.a` implies a newer MM6108 firmware paired with 2.10.4.
- `-lbcf_mf08551` is **board RF calibration**, orthogonal to firmware — keep it,
  but confirm the archive is still shipped/named in the 2.10.4 `tools/bcf`
  ([[heltec-hc33-bcf]]).
- `mmwlan_regdb.h` changed +168 lines — re-confirm our region (`AU`) resolves via
  `mmwlan_lookup_regulatory_domain` [compile], and the regdb TX-power tables
  didn't shift behavior [HW].

### G. `mmwlan.h` signature churn (+2760 lines)
- The 25 surviving calls may still have changed args/structs. **Phase 0 of the
  migration is a compile pass** against 2.10.4 to collect the real error list —
  that's the definitive drift map. Header name-presence (done above) only proves
  nothing was deleted, not that signatures match.
- Confirmed stable so far: `struct mmwlan_version { char morselib_version[…]; }`
  (our version check) and `mmwlan_get_version()` — unchanged.

---

## Recommended order

0. Branch; `git pull` ESP_HaLow to `origin/main` (2.10.4) in a **scratch** clone;
   run `tools/get.py`. Run the §E `nm` NAPT check first — it decides how much of
   the lwip hack survives.
1. Attempt `pio run -e hc33` → collect the compile-error list (§G).
2. Fix netif acquisition (§A) + register the link-status callback (§B).
3. Re-point/trim the lib linking per the §E result.
4. Port or delete the TX fix (§C) depending on whether 2.10.4 TX is already
   non-blocking.
5. Clean compile → flash → **hardware validation**: association (region/security),
   NAPT routing (phone → MowerAP → cloud), a burst/wedge stress run
   ([[halow-wedge-root-cause-confirmed]]), RTS + power-save behavior, and read the
   chip-fw version.

## Payoff if it lands
- Manual morselib swap → gone (upstream is release).
- lwip source-rebuild → likely gone (NAPT now in the config; verify §E).
- `mmnetif_link_state` assert workaround → gone (new callback API, §B).
- Non-blocking TX fix → possibly gone (§C).

Net: 2.10.4 could collapse four hand-maintained patches into stock upstream —
which is the real reason to do it, on our schedule, with hardware in hand.
