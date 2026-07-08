// HC33 BLE Proxy flasher — orchestrates form → esptool-js → flash.
//
// esptool-js is vendored locally (vendor/esptool-js-0.5.4.js) rather than
// pulled from a CDN.  esptool-js loads its per-chip flasher stub as a base64
// JSON payload; an on-the-fly transpiling CDN (esm.sh / jsdelivr +esm) mangles
// that base64 and window.atob() then throws "not correctly encoded" at
// "Uploading stub...".  The vendored prebuilt bundle inlines every stub intact
// and has no external imports, so the page stays pure static HTML/JS (no build
// step, no npm install) with no runtime CDN dependency.
//
// Flash layout (must match firmware/partitions/hc33.csv):
//   0x000000  bootloader.bin
//   0x008000  partitions.bin
//   0x00e000  boot_app0.bin
//   0x010000  firmware.bin    (lands in app0)
//   0x7EF000  config.bin      (built in browser from form input)
//
// We never set eraseAll=true.  The key_data partition at 0x7D0000 contains
// MM6108 calibration/license and erasing it kills HaLow on the device.

import { ESPLoader, Transport } from "./vendor/esptool-js-0.5.4.js";
import { buildConfigBlob, bytesToBinaryString } from "./pack.js";

// ── flash regions per variant ────────────────────────────────────────────────

const VARIANTS = {
    "hc33-halow": {
        label: "HaLow",
        dir: "firmware/hc33-halow",
        // HaLow uses every form field.
        needsHaLowFields: true,
    },
    "hc33-wifi": {
        label: "WiFi",
        dir: "firmware/hc33-wifi",
        // Standard-WiFi build only reads uplink_ssid + uplink_pass.  The
        // unused fields are still written to the blob (firmware ignores them)
        // so we keep ONE blob layout across both variants.
        needsHaLowFields: false,
    },
};

// Offsets are SAME for both variants because firmware/partitions/hc33.csv
// is shared.  Region we generate (config) is the same offset too.
const FLASH_PARTS = [
    { name: "bootloader.bin", offset: 0x000000 },
    { name: "partitions.bin", offset: 0x008000 },
    { name: "boot_app0.bin",  offset: 0x00e000 },
    { name: "firmware.bin",   offset: 0x010000 },
];
const CONFIG_OFFSET = 0x7EF000;

// ── form wiring ──────────────────────────────────────────────────────────────

const els = {
    variant:        document.getElementById("variant"),
    variantHint:    document.getElementById("variant-hint"),
    uplinkSsid:     document.getElementById("uplink_ssid"),
    uplinkPass:     document.getElementById("uplink_pass"),
    uplinkSsidLbl:  document.getElementById("uplink-ssid-label"),
    uplinkPassLbl:  document.getElementById("uplink-pass-label"),
    region:         document.getElementById("region"),
    softapSsid:     document.getElementById("softap_ssid"),
    softapPass:     document.getElementById("softap_pass"),
    softapChannel:  document.getElementById("softap_channel"),
    halowFields:    document.querySelector(".halow-only"),
    install:        document.getElementById("install"),
    formError:      document.getElementById("form-error"),
    statusCard:     document.getElementById("status-card"),
    statusLine:     document.getElementById("status-line"),
    progress:       document.getElementById("progress"),
    log:            document.getElementById("log"),
};

function updateVariantUI() {
    const v = VARIANTS[els.variant.value];
    if (v.needsHaLowFields) {
        els.halowFields.style.display = "";
        els.uplinkSsidLbl.textContent = "HaLow SSID";
        els.uplinkPassLbl.textContent = "HaLow password";
        els.variantHint.textContent =
            "HaLow needs the HD01 SSID/password/region AND the SoftAP " +
            "credentials the mower will join.";
    } else {
        els.halowFields.style.display = "none";
        els.uplinkSsidLbl.textContent = "WiFi SSID";
        els.uplinkPassLbl.textContent = "WiFi password";
        els.variantHint.textContent =
            "Standard-WiFi only uses the SSID and password of your home " +
            "WiFi.  The HC33 must stay in router range — no range extension.";
    }
    validateForm();
}

function validateForm() {
    const v = VARIANTS[els.variant.value];
    let ok = true;
    let err = "";

    if (!els.uplinkSsid.value.trim()) { ok = false; err ||= "Enter the uplink SSID."; }
    if (!els.uplinkPass.value)        { ok = false; err ||= "Enter the uplink password."; }

    if (v.needsHaLowFields) {
        if (!els.softapSsid.value.trim()) { ok = false; err ||= "Enter the SoftAP SSID."; }
        if (els.softapPass.value.length < 8) { ok = false; err ||= "SoftAP password must be ≥8 chars (WPA2)."; }
        const ch = parseInt(els.softapChannel.value, 10);
        if (!(ch >= 1 && ch <= 14))      { ok = false; err ||= "SoftAP channel must be 1–14."; }
    }

    els.install.disabled = !ok;
    els.formError.textContent = ok ? "" : err;
    return ok;
}

els.variant.addEventListener("change", updateVariantUI);
for (const id of ["uplinkSsid","uplinkPass","softapSsid","softapPass","softapChannel"]) {
    els[id].addEventListener("input", validateForm);
}

// Browser-support check.  Web Serial is the hard gate.
if (!("serial" in navigator)) {
    els.install.disabled = true;
    els.formError.textContent =
        "Your browser doesn't support Web Serial.  Use Chrome, Edge, " +
        "Brave, Opera, or Arc.";
}

updateVariantUI();

// ── flashing ─────────────────────────────────────────────────────────────────

function logLine(s) {
    els.log.textContent += s + "\n";
    els.log.scrollTop = els.log.scrollHeight;
}

// esptool-js writes its own log via a `terminal` object.  We wire its
// writeLine / write into the same #log element.
const espTerminal = {
    clean()         { els.log.textContent = ""; },
    writeLine(data) { logLine(data); },
    write(data)     { els.log.textContent += data; els.log.scrollTop = els.log.scrollHeight; },
};

async function fetchBin(url) {
    const res = await fetch(url, { cache: "no-cache" });
    if (!res.ok) throw new Error(`fetch ${url} → HTTP ${res.status}`);
    return new Uint8Array(await res.arrayBuffer());
}

function collectFormValues() {
    return {
        uplink_ssid:    els.uplinkSsid.value,
        uplink_pass:    els.uplinkPass.value,
        region:         els.region.value,
        softap_ssid:    els.softapSsid.value,
        softap_pass:    els.softapPass.value,
        softap_channel: els.softapChannel.value,
    };
}

async function install() {
    if (!validateForm()) return;

    els.install.disabled = true;
    els.statusCard.hidden = false;
    els.log.textContent = "";
    els.progress.value = 0;
    els.statusLine.textContent = "Requesting serial port…";

    const variantKey = els.variant.value;
    const variant = VARIANTS[variantKey];

    let transport;
    try {
        // Build the config blob FIRST so a CRC bug or invalid input fails
        // before we open the port (better UX than connect-then-error).
        const configBytes = buildConfigBlob(collectFormValues());
        logLine(`[flasher] built config blob: ${configBytes.length} bytes`);

        // Download all firmware parts in parallel.  Browser will cache; subsequent
        // re-flashes are instant.
        els.statusLine.textContent = "Downloading firmware…";
        const partBytes = await Promise.all(
            FLASH_PARTS.map(p => fetchBin(`${variant.dir}/${p.name}`))
        );
        FLASH_PARTS.forEach((p, i) =>
            logLine(`[flasher] ${p.name}: ${partBytes[i].length} bytes`));

        // Web Serial port picker — must be triggered by user gesture, which
        // we are (this is the install button click handler).
        els.statusLine.textContent = "Opening serial port…";
        const port = await navigator.serial.requestPort();
        transport = new Transport(port, /* tracing */ false);

        const loader = new ESPLoader({
            transport,
            baudrate: 921600,
            terminal: espTerminal,
            // No MD5 verify: avoids a third-party crypto dependency.  esptool
            // still verifies the SLIP framing and bootloader acknowledgements.
            enableTracing: false,
        });

        els.statusLine.textContent = "Connecting to ESP32-S3…";
        const chip = await loader.main();
        logLine(`[flasher] detected: ${chip}`);

        // Assemble fileArray for writeFlash.  Note esptool-js wants binary
        // strings, not Uint8Array — convert each part.
        const fileArray = FLASH_PARTS.map((p, i) => ({
            data: bytesToBinaryString(partBytes[i]),
            address: p.offset,
        }));
        fileArray.push({
            data: bytesToBinaryString(configBytes),
            address: CONFIG_OFFSET,
        });

        // Total bytes for percentage display.
        const totalBytes = fileArray.reduce((s, f) => s + f.data.length, 0);
        let writtenInPriorFiles = 0;
        let currentFileIdx = -1;

        els.statusLine.textContent = "Flashing…";
        await loader.writeFlash({
            fileArray,
            flashSize: "keep",
            flashMode: "keep",
            flashFreq: "keep",
            // CRITICAL: never set eraseAll=true.  key_data at 0x7D0000 is the
            // MM6108 calibration and is unrecoverable if erased.  esptool-js
            // only erases the sectors it writes to when eraseAll is false.
            eraseAll: false,
            compress: true,
            reportProgress: (fileIndex, written, total) => {
                if (fileIndex !== currentFileIdx) {
                    // First report for a new file — bank prior files' totals.
                    for (let i = 0; i < fileIndex; i++) {
                        // running sum recomputed; cheap, small N.
                    }
                    writtenInPriorFiles = fileArray
                        .slice(0, fileIndex)
                        .reduce((s, f) => s + f.data.length, 0);
                    currentFileIdx = fileIndex;
                    const part = fileIndex < FLASH_PARTS.length
                        ? FLASH_PARTS[fileIndex].name
                        : "config.bin";
                    logLine(`[flasher] writing ${part} @ 0x${
                        fileArray[fileIndex].address.toString(16)}`);
                }
                const pct = Math.min(100, Math.round(
                    100 * (writtenInPriorFiles + written) / totalBytes));
                els.progress.value = pct;
            },
        });

        els.statusLine.textContent = "Resetting…";
        // Reboot into the freshly-flashed app.  This mirrors esptool-js's
        // ClassicReset — the exact DTR/RTS sequence that drove THIS board into
        // download mode at connect — but keeps IO0 (DTR) HIGH at the moment EN
        // (RTS) is released, so the chip boots the app instead of the ROM
        // bootloader (ClassicReset pulls DTR low there, which is why it lands in
        // the bootloader).  esptool-js's own HardReset only de-asserts RTS,
        // which is a no-op here (connect already left RTS low) — hence the board
        // never restarting.  The trailing settle lets the chip come out of reset
        // before the finally block closes the port (close de-asserts the pins).
        await transport.setDTR(false);   // IO0 high
        await transport.setRTS(true);    // EN  low  → assert reset
        await new Promise((r) => setTimeout(r, 100));
        await transport.setRTS(false);   // EN  high → release; IO0 high → boots app
        await new Promise((r) => setTimeout(r, 250));  // let it come out of reset
        await transport.setDTR(false);   // leave both signals inactive (run state)
        logLine("[flasher] reset pulse issued — device is rebooting");

        els.progress.value = 100;
        els.statusLine.textContent = "✓ Done.  Disconnect and power-cycle the HC33.";
    } catch (e) {
        console.error(e);
        els.statusLine.textContent = "✗ Install failed.";
        logLine(`[flasher] ERROR: ${e.message || e}`);
        els.formError.textContent =
            (e && e.message) ? e.message : String(e);
    } finally {
        if (transport) {
            try { await transport.disconnect(); } catch (_) { /* ignore */ }
        }
        els.install.disabled = false;
    }
}

els.install.addEventListener("click", install);
