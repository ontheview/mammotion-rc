// Luba Remote — client glue.
//
// Flow:
//   1. /api/mowers populates the selector.
//   2. Choosing a mower opens a WebSocket to /ws/joystick/{name} and wires
//      nipplejs to send {x, y, force} frames at ~6 Hz.
//   3. Start Camera button POSTs /api/camera/{name}/start, gets Agora token,
//      joins the channel.  Stop Camera reverses it.
//   4. Pause/Resume/Go Home/STOP map to /api/action/{name}/{action}.

const els = {
  mower:    document.getElementById("mower"),
  reconnect:document.getElementById("reconnect"),
  status:   document.getElementById("status"),
  battery:  document.getElementById("battery"),
  mowerStatus: document.getElementById("mower-status"),
  mowerError:  document.getElementById("mower-error"),
  video:    document.getElementById("video"),
  camStart: document.getElementById("cam-start"),
  camStop:  document.getElementById("cam-stop"),
  compass:        document.getElementById("compass"),
  compassRing:    document.getElementById("compass-ring"),
  compassReadout: document.getElementById("compass-readout"),
  joyZone:  document.getElementById("joystick-zone"),
  pause:    document.getElementById("pause"),
  resume:   document.getElementById("resume"),
  dock:     document.getElementById("dock"),
  undock:   document.getElementById("undock"),
  stop:     document.getElementById("stop"),
  light:    document.getElementById("light"),
  log:      document.getElementById("log"),
};

const log = (msg) => {
  const t = new Date().toLocaleTimeString();
  els.log.textContent += `[${t}] ${msg}\n`;
  els.log.scrollTop = els.log.scrollHeight;
  console.log(msg);
};

// Map a device_name to its friendly app nickname for display in the log.
// The device_name stays the key everywhere else (selector value, API paths,
// joystick ws) — this is purely cosmetic.  Falls back to the raw name if no
// nickname is configured or mowersList hasn't loaded yet.
const nick = (name) => {
  const m = mowersList.find(m => m.name === name);
  return (m && m.nickname) || name;
};

const setStatus = (text, cls) => {
  els.status.textContent = text;
  els.status.classList.remove("connected", "connecting", "disconnected");
  if (cls) els.status.classList.add(cls);
};

// Render the header battery chip.  pct === null → hidden (mower hasn't reported
// yet, or link is down and we cleared it).  <20% turns it red; charging blue.
const LOW_BATTERY_PCT = 20;
const setBattery = (pct, charging) => {
  els.battery.classList.remove("low", "charging");
  if (pct === null || pct === undefined) {
    els.battery.textContent = "";   // :empty CSS hides it
    return;
  }
  els.battery.textContent = `${charging ? "⚡" : "🔋"} ${pct}%`;
  if (charging) els.battery.classList.add("charging");
  else if (pct <= LOW_BATTERY_PCT) els.battery.classList.add("low");
};

// Render the mower state chip ("Mowing", "Charging", …) and any fault message
// next to the battery.  null → hidden (:empty CSS).  Cleared when the link is
// down so we never show a stale state.  The full fault text is on the title
// attribute so a truncated message is readable on hover.
const setMowerState = (label, error) => {
  els.mowerStatus.textContent = label || "";
  els.mowerError.textContent  = error ? `⚠ ${error}` : "";
  els.mowerError.title        = error || "Mower fault";
};

let currentMower = null;
let ws = null;
let joystick = null;
let agora = null;          // AgoraRTC client when camera is up
let statusTimer = null;    // setInterval handle for status polling
let headingTimer = null;   // setInterval handle for the compass heading poll

// Fixed correction applied to the mower's reported heading before it drives the
// compass. Leave 0 unless the compass reads offset from reality (e.g. if the
// firmware's heading is magnetic or grid-relative rather than true north); then
// set this to the number of degrees to add so a known bearing reads correctly.
const COMPASS_OFFSET_DEG = 0;

// Rotate the heading-up compass. `orientation` is degrees (0-359, N=0), or null.
// The forward arrow is fixed pointing up; we rotate the ring by -heading so the
// N marker shows where geographic north is relative to the camera's view.
function setCompass(orientation) {
  if (orientation === null || orientation === undefined) {
    els.compassReadout.textContent = "—";
    return;
  }
  const deg = (((orientation + COMPASS_OFFSET_DEG) % 360) + 360) % 360;
  els.compassRing.style.transform = `rotate(${-deg}deg)`;
  els.compassReadout.textContent = `${Math.round(deg)}°`;
}

async function pollHeading(name) {
  if (name !== currentMower || !agora) return;   // only while this mower's camera is up
  try {
    const r = await fetch(`/api/heading/${encodeURIComponent(name)}`);
    if (!r.ok || name !== currentMower) return;
    const h = await r.json();
    setCompass(h.orientation);
  } catch (_) { /* transient — next tick retries */ }
}

function startCompass(name) {
  if (headingTimer) clearInterval(headingTimer);
  els.compass.hidden = false;
  pollHeading(name);                                     // paint immediately
  headingTimer = setInterval(() => pollHeading(name), 1000);
}

function stopCompass() {
  if (headingTimer) { clearInterval(headingTimer); headingTimer = null; }
  els.compass.hidden = true;
  els.compassReadout.textContent = "—";
}

// ── Mower list ──────────────────────────────────────────────────────────────
// Camera availability is per-mower (depends on iot_id being set in mowers.toml).
// We cache the /api/mowers payload so selectMower() can read it without re-fetching.
let mowersList = [];

async function loadMowers() {
  log("loadMowers: fetching /api/mowers…");
  try {
    const r = await fetch("/api/mowers", { cache: "no-store" });
    log(`loadMowers: HTTP ${r.status}`);
    if (!r.ok) { setStatus(`mower list failed: HTTP ${r.status}`, "disconnected"); return; }
    mowersList = await r.json();
    log(`loadMowers: got ${mowersList.length} mower(s)`);
  } catch (e) {
    log(`loadMowers: fetch error: ${e}`);
    setStatus("mower list failed to load", "disconnected");
    return;
  }
  els.mower.innerHTML = "";
  for (const m of mowersList) {
    const opt = document.createElement("option");
    opt.value = m.name;                       // device_name stays the key
    const label = m.nickname || m.name;       // show the friendly app name when set
    opt.textContent = m.camera ? label : `${label} (no camera)`;
    els.mower.appendChild(opt);
  }
  if (mowersList.length > 0) {
    currentMower = mowersList[0].name;
    await selectMower(currentMower);
  } else {
    setStatus("no mowers configured");
  }
}

async function selectMower(name) {
  currentMower = name;
  setStatus(`connecting ${name}…`, "connecting");
  lightOn = false;          // unknown on a fresh mower — assume off
  setLightLabel();
  await stopCamera({ silent: true });   // if a previous mower's camera was up

  const meta = mowersList.find(m => m.name === name);
  const cameraAvailable = meta && meta.camera;
  els.camStart.disabled = !cameraAvailable;
  els.camStop.disabled = true;
  if (!cameraAvailable) {
    log(`(${nick(name)}: no iot_id configured → camera disabled)`);
  }

  // Idempotent — also recovers any mower the lifespan failed to reach at boot.
  await reconnectMower(name, { silent: true });

  startJoystick(name);
  startStatusPolling(name);
}

els.mower.onchange = (e) => selectMower(e.target.value);

// ── Reconnect + status polling ──────────────────────────────────────────────
async function reconnectMower(name, { silent } = {}) {
  if (!silent) log(`reconnecting ${nick(name)}…`);
  try {
    const r = await fetch(`/api/reconnect/${encodeURIComponent(name)}`, { method: "POST" });
    if (!r.ok) {
      const body = await r.text();
      log(`reconnect failed: ${r.status} ${body}`);
      setStatus(`${name}: reconnect failed`, "disconnected");
      return false;
    }
    if (!silent) log(`reconnect ok (${nick(name)})`);
    await pollStatus(name);   // refresh status immediately
    return true;
  } catch (e) {
    log(`reconnect threw: ${e}`);
    setStatus(`${name}: reconnect error`, "disconnected");
    return false;
  }
}

// Thresholds for the "mower silent" indicator (seconds since last LubaMsg).
const MOWER_SLOW_S   = 5;
const MOWER_SILENT_S = 10;

async function pollStatus(name) {
  if (name !== currentMower) return;   // stale tick after mower switch
  try {
    const r = await fetch(`/api/status/${encodeURIComponent(name)}`);
    if (!r.ok) return;
    const s = await r.json();
    if (name !== currentMower) return;

    // Battery reflects the last decoded report; hide it whenever the link to
    // the mower isn't healthy so we never show a stale charge.
    const live = s.availability === "connected";
    setBattery(live ? s.battery : null, s.charging);
    setMowerState(live ? s.status : null, live ? s.error : null);

    // Reconcile the headlight toggle with the mower's actual state — the
    // firmware auto-offs the light, so trust the server's probe over our
    // optimistic guess.  null means "not probed yet"; keep the local guess.
    if (typeof s.light_on === "boolean" && s.light_on !== lightOn) {
      lightOn = s.light_on;
      setLightLabel();
    }

    // Layer 1: HC33 TCP/HaLow socket.  If this is bad, BLE is irrelevant.
    if (s.availability !== "connected") {
      if (s.auto_retrying) {
        setStatus(`${nick(name)}: auto-reconnecting…`, "connecting");
      } else if (s.auto_gave_up) {
        setStatus(`${nick(name)}: gave up — click Reconnect`, "disconnected");
      } else {
        setStatus(`${nick(name)}: HC33 ${s.availability}`, s.availability);
      }
      return;
    }

    // Layer 2: have we ever heard back from the mower over BLE?
    const silent = s.mower_silent_s;
    if (silent === null) {
      setStatus(`${name}: HC33 ok · waiting for mower`, "connecting");
    } else if (silent > MOWER_SILENT_S) {
      setStatus(`${name}: mower silent ${silent.toFixed(0)}s — is it on?`, "disconnected");
    } else if (silent > MOWER_SLOW_S) {
      setStatus(`${name}: mower slow ${silent.toFixed(0)}s`, "connecting");
    } else {
      setStatus(`${name}: mower ok (${silent.toFixed(1)}s)`, "connected");
    }
  } catch (_) { /* network blip — next tick will retry */ }
}

function startStatusPolling(name) {
  if (statusTimer) clearInterval(statusTimer);
  statusTimer = setInterval(() => pollStatus(name), 3000);
}

els.reconnect.onclick = () => {
  if (currentMower) reconnectMower(currentMower, {});
};

// ── Joystick ───────────────────────────────────────────────────────────────
function startJoystick(name) {
  if (ws) { ws.close(); ws = null; }
  if (joystick) { joystick.destroy(); joystick = null; }

  const wsProto = location.protocol === "https:" ? "wss:" : "ws:";
  ws = new WebSocket(`${wsProto}//${location.host}/ws/joystick/${encodeURIComponent(name)}`);
  ws.onopen  = () => log(`joystick ws open (${nick(name)})`);
  ws.onclose = () => log(`joystick ws closed (${nick(name)})`);
  ws.onerror = () => log(`joystick ws error`);

  // Dynamic mode: the stick is created under the finger on each touch, so the
  // initial touch is always neutral (force 0) and motion is measured relative
  // to where you touched.  Static mode pins the stick to the centre and treats
  // the touch point's offset from centre as an instant command — on a phone
  // your thumb lands in the lower half, so it jumped straight to "back/down".
  // dynamicPage recomputes the zone offset per interaction so a scrolled or
  // transformed page can't skew the coordinates either.
  joystick = nipplejs.create({
    zone: els.joyZone,
    mode: "dynamic",
    dynamicPage: true,
    color: "#666",
    size: 180,
  });

  let lastSend = 0;
  const THROTTLE_MS = 150;   // ~6.5 Hz

  joystick.on("move", (_evt, data) => {
    const now = Date.now();
    if (now - lastSend < THROTTLE_MS) return;
    lastSend = now;
    // nipplejs's data.vector has y positive UP (it negates internally).
    // angle.radian uses screen-clockwise-from-right, so sin(angle) for "up"
    // gives -1 — wrong sign for our server.  Stick with vector.
    sendJoystick({
      x: data.vector.x,
      y: data.vector.y,
      force: Math.min(data.force, 1),
    });
  });

  joystick.on("end", () => {
    sendJoystick({ x: 0, y: 0, force: 0 });
  });
}

function sendJoystick(payload) {
  if (ws && ws.readyState === WebSocket.OPEN) {
    ws.send(JSON.stringify(payload));
  }
}

// ── Actions ─────────────────────────────────────────────────────────────────
async function action(name) {
  if (!currentMower) return;
  log(`action: ${name}`);
  try {
    const r = await fetch(`/api/action/${encodeURIComponent(currentMower)}/${name}`, { method: "POST" });
    if (!r.ok) log(`action ${name} failed: ${r.status} ${await r.text()}`);
  } catch (e) {
    log(`action ${name} threw: ${e}`);
  }
}

els.pause.onclick  = () => action("pause");
els.resume.onclick = () => action("resume");
els.dock.onclick   = () => action("dock");
els.undock.onclick = () => action("undock");
els.stop.onclick   = () => action("stop");

// Headlight toggle.  The press is optimistic (flip + send immediately) for a
// snappy feel; pollStatus then reconciles `lightOn` against the mower's real
// state, which the server re-probes every ~6 s (the firmware auto-offs the
// light).  The label shows what pressing will *do*: "Light On" when we believe
// it's off, "Light Off" when it's on.
let lightOn = false;
function setLightLabel() {
  els.light.textContent = lightOn ? "Light Off" : "Light On";
}
els.light.onclick = async () => {
  lightOn = !lightOn;          // optimistic flip for instant feedback
  setLightLabel();
  const target = currentMower;
  await action(lightOn ? "light-on" : "light-off");
  // The server fires a get_car_light read-back right after the set; poll a
  // couple of times soon so the toggle reconciles to the mower's real state
  // (confirming success, or reverting on failure) within ~1-2 s instead of
  // waiting for the regular 3 s cadence.  Guard against a mower switch mid-wait.
  setTimeout(() => { if (currentMower === target) pollStatus(target); }, 1200);
  setTimeout(() => { if (currentMower === target) pollStatus(target); }, 2500);
};

// ── Camera ──────────────────────────────────────────────────────────────────
// Throttled so a storm of decode-failure events can't flood the BLE link.
let lastKeyframeReq = 0;
async function requestKeyframe() {
  if (!currentMower) return;
  const now = Date.now();
  if (now - lastKeyframeReq < 3000) return;
  lastKeyframeReq = now;
  try {
    await fetch(`/api/camera/${encodeURIComponent(currentMower)}/refresh`, { method: "POST" });
  } catch (e) {
    /* keyframe poke is best-effort */
  }
}

async function startCamera() {
  if (!currentMower) return;
  els.camStart.disabled = true;
  log(`starting camera for ${nick(currentMower)}…`);
  try {
    const r = await fetch(`/api/camera/${encodeURIComponent(currentMower)}/start`, { method: "POST" });
    if (!r.ok) {
      const body = await r.text();
      log(`camera start failed: ${r.status} ${body}`);
      els.camStart.disabled = false;
      return;
    }
    const tok = await r.json();
    log(`got Agora token (channel=${tok.channelName})`);
    // The mowers now publish H.265/HEVC.  The SFU forwards HEVC regardless of
    // the SDP, so the client must negotiate H.265 or every frame is "(none
    // matched)".  Requires the browser to support HEVC decode (Safari/iOS does
    // natively; Chrome needs the OS HEVC decoder, e.g. Windows "HEVC Video
    // Extensions", and may need chrome://flags HEVC WebRTC enabled).
    agora = AgoraRTC.createClient({ mode: "rtc", codec: "h265" });
    agora.on("user-published", async (user, mediaType) => {
      log(`user-published uid=${user.uid} ${mediaType}`);
      await agora.subscribe(user, mediaType);
      if (mediaType === "video") {
        user.videoTrack.play("video");
        // We've likely missed the publisher's initial keyframe; demand a fresh
        // IDR now that we're subscribed, or the decoder stays stuck on "waiting".
        requestKeyframe();
      }
      if (mediaType === "audio") user.audioTrack.play();
    });
    agora.on("user-unpublished", (u) => log(`user-unpublished uid=${u.uid}`));
    // 1005 = RECV_VIDEO_DECODE_FAILED — a missed/lost keyframe; ask for another.
    agora.on("exception", (evt) => {
      if (evt && evt.code === 1005) requestKeyframe();
    });
    await agora.join(tok.appid, tok.channelName, tok.token, tok.uid);
    log("joined Agora channel");
    els.camStop.disabled = false;
    startCompass(currentMower);   // begin polling heading for the overlay compass
  } catch (e) {
    log(`camera start threw: ${e}`);
    els.camStart.disabled = false;
  }
}

async function stopCamera({ silent } = {}) {
  stopCompass();
  if (agora) {
    try { await agora.leave(); } catch (_) {}
    agora = null;
    els.video.innerHTML = "";
  }
  if (!currentMower) {
    els.camStart.disabled = false;
    els.camStop.disabled = true;
    return;
  }
  try {
    await fetch(`/api/camera/${encodeURIComponent(currentMower)}/stop`, { method: "POST" });
    if (!silent) log("camera stopped");
  } catch (e) {
    if (!silent) log(`camera stop threw: ${e}`);
  }
  els.camStart.disabled = false;
  els.camStop.disabled = true;
}

els.camStart.onclick = startCamera;
els.camStop.onclick  = () => stopCamera({});

// ── Boot ────────────────────────────────────────────────────────────────────
loadMowers();
