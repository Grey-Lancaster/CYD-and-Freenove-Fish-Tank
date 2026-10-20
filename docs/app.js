import { ESPLoader, Transport } from "https://unpkg.com/esptool-js@0.6.0/bundle.js";

// ---- Personalize this page by editing the values below ----
const CONFIG = {
  title: "Fish Tank Flasher",
  subtitle: "Flash the animated aquarium straight from this page. No software to install.",
  footerNote:
    'Built with <a href="https://github.com/espressif/esptool-js" target="_blank" rel="noopener">esptool-js</a>. ' +
    'Source: <a href="https://github.com/Grey-Lancaster/CYD-and-Freenove-Fish-Tank" target="_blank" rel="noopener">CYD-and-Freenove-Fish-Tank</a>.',
  // Every chapter's merged image (bootloader + partitions + boot_app0 + app,
  // see docs/README.md) is a single full-flash write starting at 0x0 -- no
  // OTA/app-only mode, since these are standalone demos, not upgradeable apps.
  flashAddress: 0x0,
};
// -------------------------------------------------------------

document.title = CONFIG.title;
document.getElementById("pageTitle").textContent = CONFIG.title;
document.getElementById("pageSubtitle").textContent = CONFIG.subtitle;
document.getElementById("footerNote").innerHTML = CONFIG.footerNote;

const els = {
  unsupported: document.getElementById("unsupported"),
  app: document.getElementById("app"),
  chapterSelect: document.getElementById("chapterSelect"),
  chapterStatus: document.getElementById("chapterStatus"),
  connectBtn: document.getElementById("connectBtn"),
  connectStatus: document.getElementById("connectStatus"),
  flashBtn: document.getElementById("flashBtn"),
  baudRate: document.getElementById("baudRate"),
  eraseAll: document.getElementById("eraseAll"),
  progressWrap: document.getElementById("progressWrap"),
  progressFill: document.getElementById("progressFill"),
  progressLabel: document.getElementById("progressLabel"),
  log: document.getElementById("log"),
  clearLogBtn: document.getElementById("clearLogBtn"),
  viewLogsBtn: document.getElementById("viewLogsBtn"),
  stopLogsBtn: document.getElementById("stopLogsBtn"),
  noPortDialog: document.getElementById("noPortDialog"),
  noPortCancelBtn: document.getElementById("noPortCancelBtn"),
  noPortRetryBtn: document.getElementById("noPortRetryBtn"),
  linuxDriverNote: document.getElementById("linuxDriverNote"),
  connectErrorDialog: document.getElementById("connectErrorDialog"),
  connectErrorDetail: document.getElementById("connectErrorDetail"),
  connectErrorCancelBtn: document.getElementById("connectErrorCancelBtn"),
  connectErrorRetryBtn: document.getElementById("connectErrorRetryBtn"),
};

if (/Linux/.test(navigator.platform || "") && !/Android/.test(navigator.userAgent)) {
  els.linuxDriverNote.classList.remove("hidden");
}

let firmwareData = null;
let transport = null;
let esploader = null;
let device = null;
let logTransport = null;
let logStreamClosed = true;

function logLine(str) {
  els.log.textContent += str + "\n";
  els.log.scrollTop = els.log.scrollHeight;
}

const terminal = {
  clean() {
    els.log.textContent = "";
  },
  writeLine(data) {
    logLine(data);
  },
  write(data) {
    els.log.textContent += data;
    els.log.scrollTop = els.log.scrollHeight;
  },
};

function setConnectStatus(text, kind) {
  els.connectStatus.textContent = text;
  els.connectStatus.className = "status" + (kind ? " " + kind : "");
}

function setChapterStatus(text, kind) {
  els.chapterStatus.textContent = text;
  els.chapterStatus.className = "step-help" + (kind ? " status " + kind : "");
}

function updateFlashButtonState() {
  els.flashBtn.disabled = !(firmwareData && transport && esploader);
}

// --- Browser support check ---
if (!("serial" in navigator)) {
  els.unsupported.classList.remove("hidden");
  els.app.classList.add("hidden");
}

// --- Chapter selection: fetch that chapter's merged .bin from this repo ---
els.chapterSelect.addEventListener("change", async () => {
  const opt = els.chapterSelect.selectedOptions[0];
  const binPath = opt && opt.dataset.bin;
  firmwareData = null;
  updateFlashButtonState();

  if (!binPath) {
    setChapterStatus("");
    return;
  }

  setChapterStatus("Fetching firmware…");
  try {
    // no-store: firmware files get overwritten in place under the same
    // filename as chapters are rebuilt, so a browser-cached response
    // (this repo's GitHub Pages sends Cache-Control: max-age=600) can
    // silently serve stale/buggy firmware for up to 10 minutes after a fix
    // is pushed and already live on the server.
    const res = await fetch(binPath, { cache: "no-store" });
    if (!res.ok) throw new Error(`HTTP ${res.status}`);
    const buf = await res.arrayBuffer();
    firmwareData = new Uint8Array(buf);
    setChapterStatus(`Ready: ${opt.textContent} — ${(firmwareData.length / 1024).toFixed(1)} KB`, "good");
    logLine(`Loaded ${binPath}: ${firmwareData.length} bytes`);
  } catch (err) {
    firmwareData = null;
    setChapterStatus(`Couldn't load that chapter's firmware: ${err.message || err}`, "bad");
    logLine(`Error fetching ${binPath}: ${err.message || err}`);
  }
  updateFlashButtonState();
});

// Enable connect once Web Serial is confirmed present.
els.connectBtn.disabled = false;

// --- Connect ---
async function connectDevice() {
  try {
    els.connectBtn.disabled = true;

    if (logTransport) {
      await stopLogStream();
      els.connectBtn.disabled = true;
    }

    setConnectStatus("Requesting device...");

    device = await navigator.serial.requestPort();
    transport = new Transport(device, true);

    const baudrate = parseInt(els.baudRate.value, 10);
    esploader = new ESPLoader({ transport, baudrate, terminal });

    setConnectStatus("Connecting...");
    const chipName = await esploader.main();

    setConnectStatus(`Connected: ${chipName}`, "good");
    logLine(`Connected to ${chipName}`);
    els.connectBtn.textContent = "Reconnect";
    updateFlashButtonState();
  } catch (err) {
    console.error(err);
    transport = null;
    esploader = null;
    updateFlashButtonState();

    if (err.name === "NotFoundError") {
      // User closed the port picker without selecting a device.
      setConnectStatus("");
      els.noPortDialog.showModal();
    } else {
      // A port was selected, but the handshake with the chip failed.
      setConnectStatus(err.message || "Connection failed", "bad");
      logLine(`Error: ${err.message || err}`);
      els.connectErrorDetail.textContent = err.message || String(err);
      els.connectErrorDialog.showModal();
    }
  } finally {
    els.connectBtn.disabled = false;
  }
}

els.connectBtn.addEventListener("click", connectDevice);
els.noPortCancelBtn.addEventListener("click", () => els.noPortDialog.close());
els.noPortRetryBtn.addEventListener("click", () => {
  els.noPortDialog.close();
  connectDevice();
});
els.connectErrorCancelBtn.addEventListener("click", () => els.connectErrorDialog.close());
els.connectErrorRetryBtn.addEventListener("click", () => {
  els.connectErrorDialog.close();
  connectDevice();
});

// --- Flash ---
els.flashBtn.addEventListener("click", async () => {
  if (!firmwareData || !esploader) return;

  const confirmMessage =
    "This will fully erase and reflash the board with the selected " +
    "chapter's firmware, including WiFi and any other saved settings.\n\nContinue?";
  if (!window.confirm(confirmMessage)) return;

  els.flashBtn.disabled = true;
  els.connectBtn.disabled = true;
  els.progressWrap.classList.remove("hidden");
  els.progressFill.style.width = "0%";
  els.progressLabel.textContent = "0%";

  try {
    await esploader.writeFlash({
      fileArray: [{ data: firmwareData, address: CONFIG.flashAddress }],
      flashMode: "keep",
      flashFreq: "keep",
      flashSize: "keep",
      eraseAll: els.eraseAll.checked,
      compress: true,
      reportProgress: (fileIndex, written, total) => {
        const percent = Math.round((written / total) * 100);
        els.progressFill.style.width = percent + "%";
        els.progressLabel.textContent = percent + "%";
      },
    });

    logLine("Flash complete.");
    await transport.disconnect();

    els.progressLabel.textContent = "Done!";

    transport = null;
    esploader = null;
    setConnectStatus("Disconnected", "");
    els.connectBtn.textContent = "Connect Device";

    logLine("Power-cycle the board (unplug/replug) to boot into the new firmware, then click \"View Device Logs\" below to watch it come up.");
    els.viewLogsBtn.classList.remove("hidden");
  } catch (err) {
    console.error(err);
    logLine(`Error: ${err.message || err}`);
    els.progressLabel.textContent = "Failed";
  } finally {
    els.flashBtn.disabled = false;
    els.connectBtn.disabled = false;
    updateFlashButtonState();
  }
});

// --- Clear log ---
els.clearLogBtn.addEventListener("click", () => {
  els.log.textContent = "";
});

// --- Device logs ---
// Reopens the same already-authorized port (no picker dialog) to stream
// its console output, the way ESP Web Tools' "Logs" step does. This board
// uses native USB CDC -- the chip's own USB peripheral drops out along
// with the chip's own power cycle, unlike a board with a separate UART
// bridge chip (which stays connected to the PC the whole time) -- so a
// power cycle always drops the port, and it re-enumerates a moment later.
// That means we can never "stay connected through" one; we can only
// reconnect afterward, and how long that takes varies -- and since this
// board needs to actually be power-cycled by hand, the caller may need
// real time to do that -- so this retries for a while instead of trying
// once after a fixed delay.
async function openLogTransport(timeoutMs = 20000, intervalMs = 150) {
  const deadline = Date.now() + timeoutMs;
  let lastErr;
  while (Date.now() < deadline) {
    try {
      const t = new Transport(device, true);
      await t.connect(115200);
      return t;
    } catch (err) {
      lastErr = err;
      await new Promise((resolve) => setTimeout(resolve, intervalMs));
    }
  }
  throw lastErr || new Error("Timed out waiting for the device to reappear");
}

async function startLogStream() {
  // Logs don't need the ROM bootloader handshake connectDevice() does (that
  // handshake only works with the board actually in bootloader mode, since
  // it's what flashing needs) -- just request a plain serial port directly,
  // so this works standalone against a board that's already running
  // normally, without going through Connect/Flash first.
  if (!device) {
    try {
      device = await navigator.serial.requestPort();
    } catch (err) {
      if (err.name !== "NotFoundError") logLine(`Error: ${err.message || err}`);
      return;
    }
  }

  els.viewLogsBtn.classList.add("hidden");
  els.stopLogsBtn.classList.remove("hidden");
  els.connectBtn.disabled = true;

  let myTransport;
  try {
    logLine("--- Reconnecting for device logs... ---");
    myTransport = await openLogTransport();
    logTransport = myTransport;
    logStreamClosed = false;
    // Guard against a stale callback from a *previous* logTransport
    // tearing down whatever we just reconnected to here.
    logTransport.setDeviceLostCallback(() => {
      if (logTransport === myTransport) stopLogStream();
    });

    logLine("--- Viewing device logs (115200 baud) ---");
    const decoder = new TextDecoder();
    await logTransport.rawRead((data) => {
      els.log.textContent += decoder.decode(data);
      els.log.scrollTop = els.log.scrollHeight;
    }, () => logStreamClosed);
  } catch (err) {
    if (!logStreamClosed) logLine(`Log stream error: ${err.message || err}`);
  } finally {
    if (logTransport === myTransport) {
      await stopLogStream();
    }
  }
}

async function stopLogStream() {
  logStreamClosed = true;
  if (logTransport) {
    try {
      await logTransport.disconnect();
    } catch {
      // port may already be gone (e.g. device unplugged)
    }
    logTransport = null;
  }
  els.stopLogsBtn.classList.add("hidden");
  els.viewLogsBtn.classList.remove("hidden");
  els.connectBtn.disabled = false;
}

els.viewLogsBtn.addEventListener("click", startLogStream);
els.stopLogsBtn.addEventListener("click", stopLogStream);

// Single firmware: load it immediately.
els.chapterSelect.dispatchEvent(new Event("change"));
