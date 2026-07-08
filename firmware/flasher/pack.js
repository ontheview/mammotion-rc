// Build the on-flash config blob that config_load.cpp reads.
//
// THIS MUST PRODUCE BYTE-IDENTICAL OUTPUT TO scripts/pack_config.py.
// If you edit one, edit the other in the same commit and re-test by diffing
// the output bytes (Python `python3 pack_config.py x.ini /tmp/py.bin` vs
// the Uint8Array produced here for the same input).
//
// Layout (v2, 210 bytes total, little-endian):
//   uint32  magic   = 0x33334643      (4)
//   uint8   version = 2               (1)
//   char[]  uplink_ssid               (33, NUL-padded)
//   char[]  uplink_pass               (65)
//   char[]  region  (e.g. "AU")       (4)
//   char[]  softap_ssid               (33)
//   char[]  softap_pass               (65)
//   uint8   softap_channel  1-14      (1)
//   uint32  crc32 over preceding 206 bytes  (4)

const MAGIC = 0x33334643;
const VERSION = 2;
const WIDTH_SSID = 33;
const WIDTH_PASS = 65;
const WIDTH_REGION = 4;
const BODY_LEN = 206;
const TOTAL_LEN = 210;

// zlib CRC32 — same algorithm as Python zlib.crc32 and config_load.cpp.
// Poly 0xEDB88320, init 0xFFFFFFFF, final XOR 0xFFFFFFFF, refin/refout=true.
// Lazy-init the table on first call so the module load is cheap.
let _crcTable = null;
function crc32(bytes) {
    if (_crcTable === null) {
        _crcTable = new Uint32Array(256);
        for (let i = 0; i < 256; i++) {
            let c = i;
            for (let k = 0; k < 8; k++) {
                c = (c & 1) ? (0xEDB88320 ^ (c >>> 1)) : (c >>> 1);
            }
            _crcTable[i] = c >>> 0;
        }
    }
    let crc = 0xFFFFFFFF >>> 0;
    for (let i = 0; i < bytes.length; i++) {
        crc = (_crcTable[(crc ^ bytes[i]) & 0xFF] ^ (crc >>> 8)) >>> 0;
    }
    return (crc ^ 0xFFFFFFFF) >>> 0;
}

// UTF-8 encode `s`, truncate to width-1 bytes, NUL-pad to `width` total.
// Truncation is on byte boundaries so a multi-byte char at the boundary is
// dropped entirely rather than left half-encoded.  Matches Python:
//   s.encode("utf-8")[: width - 1] + b"\x00" * pad
function fixedField(s, width) {
    const enc = new TextEncoder().encode(s || "");
    const out = new Uint8Array(width);
    const n = Math.min(enc.length, width - 1);
    out.set(enc.subarray(0, n), 0);
    // out is zero-initialised, so the NUL pad is implicit.
    return out;
}

// Build the config blob from a form-style object:
//   { uplink_ssid, uplink_pass, region, softap_ssid, softap_pass, softap_channel }
// Returns Uint8Array(210).  Throws on invalid input.
export function buildConfigBlob(c) {
    const channel = parseInt(c.softap_channel, 10);
    if (!Number.isFinite(channel) || channel < 1 || channel > 14) {
        throw new Error(`softap_channel=${c.softap_channel} out of range 1-14`);
    }

    const body = new Uint8Array(BODY_LEN);
    const dv = new DataView(body.buffer);
    let off = 0;
    dv.setUint32(off, MAGIC, true); off += 4;
    dv.setUint8(off, VERSION);      off += 1;
    body.set(fixedField(c.uplink_ssid, WIDTH_SSID),  off); off += WIDTH_SSID;
    body.set(fixedField(c.uplink_pass, WIDTH_PASS),  off); off += WIDTH_PASS;
    body.set(fixedField(c.region,      WIDTH_REGION), off); off += WIDTH_REGION;
    body.set(fixedField(c.softap_ssid, WIDTH_SSID),  off); off += WIDTH_SSID;
    body.set(fixedField(c.softap_pass, WIDTH_PASS),  off); off += WIDTH_PASS;
    dv.setUint8(off, channel);      off += 1;

    if (off !== BODY_LEN) {
        // Should be unreachable; means the constants are out of sync.
        throw new Error(`body length drift: ${off} != ${BODY_LEN}`);
    }

    const crc = crc32(body);
    const out = new Uint8Array(TOTAL_LEN);
    out.set(body, 0);
    new DataView(out.buffer).setUint32(BODY_LEN, crc, true);
    return out;
}

// Convert Uint8Array → binary string (each char is one byte, Latin-1 range).
// esptool-js's writeFlash expects this string-of-bytes format, not Uint8Array.
// Done in 64 KB chunks to avoid blowing the JS engine's argument stack on
// String.fromCharCode(...big array).
export function bytesToBinaryString(u8) {
    const CHUNK = 0x10000;
    let s = "";
    for (let i = 0; i < u8.length; i += CHUNK) {
        s += String.fromCharCode.apply(null, u8.subarray(i, i + CHUNK));
    }
    return s;
}
