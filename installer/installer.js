import { ESPLoader, Transport } from "esptool-js";

const OWNER = "gianmarco112";
const REPO = "esp32-teams-presence";
const API = `https://api.github.com/repos/${OWNER}/${REPO}/releases`;
const $ = (id) => document.getElementById(id);

let releases = [];
let latestRelease = null;
let transport = null;
let esploader = null;

function setStatus(message, type = "") {
  const el = $("status");
  el.textContent = message;
  el.className = `status ${type}`;
}

function setProgress(percent, title, message) {
  $("progress-panel").hidden = false;
  $("progress-title").textContent = title;
  $("progress-percent").textContent = `${Math.round(percent)}%`;
  $("progress-bar").style.width = `${percent}%`;
  $("progress-message").textContent = message;
}

function log(message) {
  const el = $("flash-log");
  el.textContent += `${message}\n`;
  el.scrollTop = el.scrollHeight;
}

function assetFor(release, name) {
  return release.assets?.find((asset) => asset.name === name);
}

async function loadReleases() {
  try {
    const response = await fetch(API, { headers: { Accept: "application/vnd.github+json" } });
    if (!response.ok) throw new Error(`GitHub API: HTTP ${response.status}`);
    releases = await response.json();
    releases = releases.filter((release) => !release.draft && !release.prerelease);
    latestRelease = releases[0] || null;
    renderReleases();
    if (!latestRelease) {
      $("latest-version").textContent = "Nessuna release";
      $("latest-notes").textContent = "Le versioni saranno mostrate qui quando verranno pubblicate su GitHub.";
      setStatus("Nessuna release firmware pubblicata. L’installer è pronto, ma non ci sono binari da installare.", "error");
      return;
    }
    $("latest-version").textContent = latestRelease.tag_name;
    $("latest-date").textContent = new Date(latestRelease.published_at || latestRelease.created_at).toLocaleDateString("it-IT");
    $("latest-notes").textContent = latestRelease.body?.trim() || "Release stabile di Teams Presence.";
    $("install-latest").disabled = !hasRequiredAssets(latestRelease);
    if (!hasRequiredAssets(latestRelease)) {
      setStatus("Release trovata, ma manca manifest.json o non contiene la build necessaria.", "error");
    } else {
      setStatus("Firmware disponibile. Collega l’ESP32 per iniziare.", "success");
    }
  } catch (error) {
    console.error(error);
    $("latest-version").textContent = "Non disponibile";
    $("latest-notes").textContent = "Impossibile recuperare le release da GitHub.";
    setStatus(`Errore nel caricamento delle release: ${error.message}`, "error");
  }
}

function hasRequiredAssets(release) {
  return Boolean(assetFor(release, "manifest.json"));
}

function renderReleases() {
  const list = $("release-list");
  const older = releases.slice(latestRelease ? 1 : 0);
  if (!older.length) {
    list.innerHTML = '<div class="empty">Non sono ancora disponibili versioni precedenti.</div>';
    return;
  }
  list.replaceChildren();
  for (const release of older) {
    const row = document.createElement("div");
    row.className = "release-item";
    const info = document.createElement("div");
    const version = document.createElement("strong");
    version.textContent = release.tag_name;
    const date = document.createElement("small");
    date.textContent = new Date(release.published_at || release.created_at).toLocaleDateString("it-IT");
    info.append(version, date);
    const button = document.createElement("button");
    button.className = "secondary";
    button.textContent = "Installa";
    button.disabled = !hasRequiredAssets(release);
    button.addEventListener("click", () => installRelease(release));
    row.append(info, button);
    list.append(row);
  }
}

async function fetchManifest(release) {
  const asset = assetFor(release, "manifest.json");
  if (!asset) throw new Error("La release non contiene manifest.json.");
  const response = await fetch(asset.browser_download_url);
  if (!response.ok) throw new Error(`Impossibile scaricare manifest.json (HTTP ${response.status}).`);
  return response.json();
}

async function installRelease(release) {
  if (!("serial" in navigator)) {
    setStatus("Web Serial non è supportato. Usa Chrome o Edge su desktop con HTTPS.", "error");
    return;
  }
  $("flash-log").textContent = "";
  try {
    setProgress(0, `Installazione ${release.tag_name}`, "Seleziona la porta USB dell’ESP32.");
    // requestPort must be called directly from the user's click, before any awaited fetch.
    const selectedPort = await navigator.serial.requestPort();
    const manifest = await fetchManifest(release);
    if (!manifest.builds?.length) throw new Error("Manifest non valido: nessun build trovato.");
    const build = manifest.builds[0];
    if (build.chipFamily !== "ESP32") throw new Error("Firmware incompatibile: il manifest non dichiara ESP32.");
    if (!build.parts?.length) throw new Error("Manifest non valido: nessuna partizione definita.");

    const manifestAsset = assetFor(release, "manifest.json");
    transport = new Transport(selectedPort, true);
    const terminal = {
      clean() {},
      writeLine(data) { log(data); },
      write(data) { log(data); }
    };
    esploader = new ESPLoader({
      transport,
      baudrate: 460800,
      terminal,
      debugLogging: false
    });

    setProgress(2, "Connessione", "Rilevamento ESP32 in corso…");
    const chip = await esploader.main();
    log(`Chip rilevato: ${chip}`);
    if (!String(chip).toUpperCase().startsWith("ESP32")) {
      throw new Error(`Chip non compatibile con questo firmware: ${chip}`);
    }

    const fileParts = [];
    for (const part of build.parts) {
      if (!part.path || !Number.isInteger(Number(part.offset)) || Number(part.offset) < 0) {
        throw new Error("Manifest non valido: percorso o offset di una partizione non valido.");
      }
      const url = new URL(part.path, manifestAsset.browser_download_url);
      const response = await fetch(url);
      if (!response.ok) throw new Error(`Download firmware fallito: ${part.path} (HTTP ${response.status}).`);
      const data = new Uint8Array(await response.arrayBuffer());
      if (!data.length) throw new Error(`Il file firmware è vuoto: ${part.path}`);
      fileParts.push({ data, address: Number(part.offset) });
    }

    setProgress(8, "Scrittura firmware", "Non scollegare il dispositivo.");
    await esploader.writeFlash({
      fileArray: fileParts,
      flashSize: "keep",
      flashMode: "keep",
      flashFreq: "keep",
      eraseAll: false,
      compress: true,
      reportProgress(fileIndex, written, total) {
        const percent = total ? 8 + (written / total) * 88 : 8;
        setProgress(percent, "Scrittura firmware", `File ${fileIndex + 1}: ${Math.round(written / Math.max(total, 1) * 100)}%`);
      }
    });

    setProgress(98, "Riavvio", "Firmware scritto. Riavvio ESP32…");
    await esploader.after("hard_reset");
    setProgress(100, "Installazione completata", "Puoi scollegare l’ESP32.");
    setStatus(`Installazione ${release.tag_name} completata.`, "success");
  } catch (error) {
    console.error(error);
    log(`ERRORE: ${error.message}`);
    setProgress(0, "Installazione non completata", error.message);
    setStatus(error.message || "Errore durante il flash.", "error");
    if (/cancel|permission/i.test(error.message)) setStatus("Operazione annullata o accesso alla porta USB negato.", "error");
  } finally {
    try { await transport?.disconnect(); } catch {}
    transport = null;
    esploader = null;
  }
}

$("install-latest").addEventListener("click", () => {
  if (latestRelease) installRelease(latestRelease);
});
$("versions-toggle").addEventListener("click", () => {
  const button = $("versions-toggle");
  const expanded = button.getAttribute("aria-expanded") === "true";
  button.setAttribute("aria-expanded", String(!expanded));
  $("versions-content").hidden = expanded;
});
loadReleases();
