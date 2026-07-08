# HC33 web flasher

Static HTML/JS that flashes the HC33 BLE proxy firmware over USB via Web
Serial.  No server, no backend, no build step.

## What's in here

- `index.html`, `styles.css`, `app.js`, `pack.js` — the page itself.
- `firmware/hc33-halow/`, `firmware/hc33-wifi/` — drop the four PIO build
  artefacts (`bootloader.bin`, `partitions.bin`, `boot_app0.bin`, `firmware.bin`)
  for each variant here.  See `firmware/README.md`.
- `pack.js` — config blob builder.  MUST stay byte-identical to
  `../scripts/pack_config.py`.  When you change one, change both.

## Local testing

Web Serial needs a secure context.  Either `https://` or `localhost` works;
plain `http://` IPs do NOT.  Easiest local server:

```bash
cd firmware/flasher
python3 -m http.server 8000
# then open http://localhost:8000/
```

## Publishing to GitHub Pages

> **License requirement:** the HaLow `firmware.bin` contains proprietary Morse
> Micro blobs under the Morse BDL, which requires the license text to travel with
> the binaries. Keep `firmware/LICENSES/` in whatever you publish — don't strip
> it. See `firmware/README.md` in this folder.

Two common patterns:

### A) Publish a subfolder from main (simplest)

In the repo's GitHub Pages settings, set source to **main branch /
firmware/flasher folder** (GitHub Pages supports `/docs` or root only by
default — for a non-standard subfolder, use pattern B below).

### B) Push `flasher/` to a `gh-pages` branch

```bash
cd firmware/flasher
git init -b gh-pages
git add .
git commit -m "publish flasher"
git remote add origin git@github.com:ontheview/mammotion-rc.git
git push -u origin gh-pages --force
```

Then in repo Settings → Pages, set source to **gh-pages branch / root**.
Final URL: `https://ontheview.github.io/mammotion-rc/`.

Either way the URL must be HTTPS (GH Pages does that automatically).

## After publishing

1. Open the URL in a Chromium-based browser.
2. Plug the HC33 into USB.
3. Fill the form, click Install.
4. When the browser asks which port, pick the CP210x device.
5. Wait ~30s.  When it says "Done", power-cycle the HC33.

## Re-flashing to change config

Re-running the flow is the way to change config.  The MM6108 calibration
partition (`key_data` at `0x7D0000`) is preserved — the flasher only writes
the five regions it knows about and never sets `eraseAll`.

## Browser support

- ✅ Chrome / Edge / Brave / Opera / Arc (all Chromium-based)
- ❌ Firefox, Safari (no Web Serial)
- Windows additionally needs the SiLabs CP210x driver installed once.
