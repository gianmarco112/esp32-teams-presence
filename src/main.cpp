#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <FastLED.h>
#include "led/LedRenderer.h"
#include <WiFiManager.h>
#include <Update.h>
#include <mbedtls/md.h>
#include <time.h>
#include <esp32_cert_bundle.h>

#include "config.h"
#include "build_version.h"

// Optional compile-time defaults for developers with a private local config.h.
// Production firmware ships with empty defaults and is configured from the web UI.
#ifndef MS_TENANT_ID
#define MS_TENANT_ID ""
#endif
#ifndef MS_CLIENT_ID
#define MS_CLIENT_ID ""
#endif

#ifndef LED_CHIPSET
#define LED_CHIPSET WS2812B
#endif

#ifndef LED_MAX_COUNT
#define LED_MAX_COUNT 300
#endif
#ifndef PRESENCE_POLL_MIN_S
#define PRESENCE_POLL_MIN_S 10
#endif
#ifndef PRESENCE_POLL_MAX_S
#define PRESENCE_POLL_MAX_S 3600
#endif

CRGB leds[LED_MAX_COUNT];
LedRenderer ledRenderer;
LedGeometry ledGeometry = LedGeometry::Strip;
LedEffect ledEffect = LedEffect::Static;
uint16_t matrixWidth = 8;
uint16_t matrixHeight = 8;
bool matrixSerpentine = true;
uint16_t activeLeds = LED_COUNT;
uint32_t pollIntervalMs = PRESENCE_POLL_MS;
uint8_t brightnessPct = (LED_BRIGHTNESS * 100 + 127) / 255;
String manualState;  // empty = automatic (follows Teams presence)

void pollPresence();
WebServer server(WEB_PORT);
Preferences prefs;
WiFiManager wifiManager;
bool wifiPortalActive = false;
bool servicesStarted = false;

String accessToken;
String refreshToken;
String entraTenantId;
String entraClientId;
String authUser = "Non autenticato";
String presenceText = "In attesa";
String activityText = "-";
String deviceCode;
String userCode;
String verificationUri;
String oauthError;
uint32_t deviceCodeExpires = 0;
uint32_t deviceCodeStartedAt = 0;
uint32_t devicePollInterval = 5;
uint32_t nextDevicePoll = 0;
uint32_t tokenExpiryEpoch = 0;
uint32_t lastPresencePoll = 0;
bool deviceLoginPending = false;
bool graphReady = false;
bool presenceReady = false;  // True only after a successful Microsoft Graph /me/presence response.
bool wifiReady = false;

const char* otaManifestUrl = "https://gianmarco112.github.io/esp32-teams-presence/latest.json";
const char* otaFirmwarePrefix = "https://gianmarco112.github.io/esp32-teams-presence/firmware/";

struct LedPhase {
  CRGB color;
  uint16_t durationMs;
};

const LedPhase BOOT_PATTERN[] = {
  {CRGB::White, 120}, {CRGB::Black, 140},
  {CRGB::White, 120}, {CRGB::Black, 500}
};
const LedPhase WIFI_CONNECTING_PATTERN[] = {
  {CRGB::Yellow, 250}, {CRGB::Black, 450}
};
const LedPhase WIFI_PORTAL_PATTERN[] = {
  {CRGB::Cyan, 180}, {CRGB::Black, 160},
  {CRGB::Cyan, 180}, {CRGB::Black, 900}
};
const LedPhase WIFI_FAILED_PATTERN[] = {
  {CRGB::Red, 120}, {CRGB::Black, 130},
  {CRGB::Red, 120}, {CRGB::Black, 130},
  {CRGB::Red, 120}, {CRGB::Black, 1000}
};
const LedPhase WIFI_CONNECTED_PATTERN[] = {
  {CRGB::Green, 130}, {CRGB::Black, 130},
  {CRGB::Green, 130}, {CRGB::Black, 700}
};
const LedPhase WAITING_MICROSOFT_PATTERN[] = {
  {CRGB::Blue, 850}, {CRGB::Black, 1150}
};
const LedPhase LOGIN_PENDING_PATTERN[] = {
  {CRGB::Purple, 350}, {CRGB::Black, 350}
};
const LedPhase GRAPH_ERROR_PATTERN[] = {
  {CRGB::Magenta, 140}, {CRGB::Black, 130},
  {CRGB::Magenta, 140}, {CRGB::Black, 130},
  {CRGB::Magenta, 140}, {CRGB::Black, 900}
};

const LedPhase* currentLedPattern = nullptr;
size_t currentLedPatternLength = 0;
size_t currentLedPhase = 0;
uint32_t currentLedPhaseStarted = 0;

const char* graphPresenceUrl = "https://graph.microsoft.com/v1.0/me/presence";

String encodeForm(const String& s) {
  const char hex[] = "0123456789ABCDEF";
  String out;
  for (size_t i = 0; i < s.length(); i++) {
    uint8_t c = (uint8_t)s[i];
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_' ||
        c == '.' || c == '~') {
      out += (char)c;
    } else if (c == ' ') {
      out += '+';
    } else {
      out += '%';
      out += hex[c >> 4];
      out += hex[c & 0x0F];
    }
  }
  return out;
}

bool isGuid(const String& value) {
  if (value.length() != 36) return false;
  for (size_t i = 0; i < value.length(); i++) {
    const char c = value[i];
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (c != '-') return false;
      continue;
    }
    const bool digit = c >= '0' && c <= '9';
    const bool lower = c >= 'a' && c <= 'f';
    const bool upper = c >= 'A' && c <= 'F';
    if (!digit && !lower && !upper) return false;
  }
  return true;
}

bool entraConfigured() {
  return isGuid(entraTenantId) && isGuid(entraClientId);
}

String tokenUrl() {
  return String("https://login.microsoftonline.com/") +
         entraTenantId + "/oauth2/v2.0/token";
}

String deviceCodeUrl() {
  return String("https://login.microsoftonline.com/") +
         entraTenantId + "/oauth2/v2.0/devicecode";
}

bool httpsRequest(const String& url, const String& method,
                  const String& body, const String& bearer,
                  String& response, int& status) {
  WiFiClientSecure client;
  // Uses the ESP-IDF certificate bundle. Do not disable TLS verification.
  client.setCACertBundle(x509_crt_bundle, x509_crt_bundle_len);
  HTTPClient http;
  http.setTimeout(15000);
  if (!http.begin(client, url)) return false;

  http.addHeader("Accept", "application/json");
  if (method == "POST") {
    http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  }
  if (bearer.length()) http.addHeader("Authorization", "Bearer " + bearer);

  status = (method == "POST") ? http.POST(body) : http.GET();
  if (status > 0) response = http.getString();
  http.end();
  return status > 0;
}

void showColor(const CRGB& c) {
  ledRenderer.stop();
  fill_solid(leds, LED_MAX_COUNT, CRGB::Black);
  fill_solid(leds, activeLeds, c);
  FastLED.show();
}

void showPresenceColor(const CRGB& c) {
  ledRenderer.start(c, ledEffect, ledGeometry, matrixWidth, matrixHeight, matrixSerpentine);
  ledRenderer.renderNow(millis());
  FastLED.show();
}

void setLedPattern(const LedPhase* phases, size_t count, const char* description) {
  currentLedPattern = phases;
  currentLedPatternLength = count;
  currentLedPhase = 0;
  currentLedPhaseStarted = millis();
  if (count > 0) showColor(phases[0].color);
  Serial.printf("[LED] %s\n", description);
}

void stopLedPattern() {
  currentLedPattern = nullptr;
  currentLedPatternLength = 0;
}

void updateLedPattern() {
  if (currentLedPattern && currentLedPatternLength > 0) {
    const uint32_t now = millis();
    if ((uint32_t)(now - currentLedPhaseStarted) >=
        currentLedPattern[currentLedPhase].durationMs) {
      currentLedPhase = (currentLedPhase + 1) % currentLedPatternLength;
      currentLedPhaseStarted = now;
      showColor(currentLedPattern[currentLedPhase].color);
    }
    return;
  }
  if (ledRenderer.update(millis())) FastLED.show();
}

// A manual state locks normal presence indications. Boot and Wi-Fi diagnostics bypass that lock.
void setSolid(const CRGB& c) {
  if (manualState.length()) return;
  stopLedPattern();
  showPresenceColor(c);
}

bool stateColor(const String& s, CRGB& c) {
  if (s == "Available") c = CRGB::Green;
  else if (s == "Busy") c = CRGB::Red;
  else if (s == "DoNotDisturb") c = CRGB::Purple;
  else if (s == "Away") c = CRGB::Yellow;
  else if (s == "BeRightBack") c = CRGB::Orange;
  else if (s == "Offline") c = CRGB::Black;
  else if (s == "Error") c = CRGB::Blue;
  else return false;
  return true;
}

void applyManualState() {
  CRGB c;
  if (manualState.length() && stateColor(manualState, c)) showPresenceColor(c);
}

void setErrorBlink() {
  if (manualState.length()) return;
  setLedPattern(GRAPH_ERROR_PATTERN,
                sizeof(GRAPH_ERROR_PATTERN) / sizeof(GRAPH_ERROR_PATTERN[0]),
                "Graph/API error: 3 magenta flashes");
}

String htmlEscape(String s) {
  s.replace("&", "&amp;");
  s.replace("<", "&lt;");
  s.replace(">", "&gt;");
  s.replace("\"", "&quot;");
  return s;
}

String entraSettingsHtml() {
  const bool operational = graphReady && presenceReady && !deviceLoginPending;
  String html = F("<details class='card section' ");
  if (!operational) html += F("open ");
  html += F("><summary><span class='section-icon'>ID</span><span class='summary-copy'>"
            "<strong>Microsoft Entra ID</strong><small>Configurazione dell'applicazione e dell'accesso</small></span>"
            "<span class='summary-status'>");
  html += operational ? F("<span class='pill success'>Configurato</span>") :
            (entraConfigured() ? F("<span class='pill neutral'>Configurazione</span>") :
                                 F("<span class='pill warning'>Da configurare</span>"));
  html += F("</span><span class='chevron' aria-hidden='true'>⌄</span></summary>"
            "<div class='section-body'><p class='muted'>Gli identificativi dell'app Entra sono pubblici: non inserire qui password o client secret.</p>"
            "<form method='post' action='/entra' class='form-grid'>"
            "<div class='field full'><label>Directory (tenant) ID</label>"
            "<input type='text' name='tenant' minlength='36' maxlength='36' autocomplete='off' "
            "autocapitalize='none' spellcheck='false' required value='");
  html += htmlEscape(entraTenantId);
  html += F("'></div><div class='field full'><label>Application (client) ID</label>"
            "<input type='text' name='client' minlength='36' maxlength='36' autocomplete='off' "
            "autocapitalize='none' spellcheck='false' required value='");
  html += htmlEscape(entraClientId);
  html += F("'></div><div class='field full actions'><button class='button primary' type='submit'>"
            "Salva configurazione <span aria-hidden='true'>→</span></button></div></form>"
            "<div class='hint'><span class='hint-icon'>i</span><span>Per funzionare, l'app registrata deve avere il permesso Microsoft Graph delegato <b>Presence.Read</b> "
            "e i flussi client pubblici abilitati.</span></div></div></details>");
  return html;
}

String settingsFormHtml() {
  static const char* const states[][2] = {
    {"", "Automatico · segue Teams"}, {"Available", "Disponibile · verde"},
    {"Busy", "Occupato · rosso"}, {"DoNotDisturb", "Non disturbare · viola"},
    {"Away", "Assente · giallo"}, {"BeRightBack", "Torno subito · arancione"},
    {"Offline", "Offline · spento"}, {"Error", "Errore · blu"}};

  String html = F(
    "<section class='card section'><div class='section-heading'>"
    "<span class='section-icon'>LED</span><span class='summary-copy'><strong>Illuminazione e dispositivo</strong>"
    "<small>Personalizza il comportamento della striscia LED</small></span></div>"
    "<div class='section-body'><form method='post' action='/settings' class='form-grid'>"
    "<div class='field'><label>Numero di LED</label><input type='number' name='leds' min='1' max='"
  );
  html += String(LED_MAX_COUNT) + "' value='" + String(activeLeds) + "'>";
  html += F("</div><div class='field'><label>Luminosità <output id='bv'>");
  html += String(brightnessPct);
  html += F("%</output></label><input type='range' name='brightness' min='1' max='100' value='");
  html += String(brightnessPct);
  html += F("' oninput=\"document.getElementById('bv').textContent=this.value+'%'\">"
            "</div><div class='field'><label>Intervallo di aggiornamento</label><div class='input-unit'>"
            "<input type='number' name='refresh' min='");
  html += String(PRESENCE_POLL_MIN_S) + "' max='" + String(PRESENCE_POLL_MAX_S) +
          "' value='" + String(pollIntervalMs / 1000UL) + "'><span>secondi</span></div></div>";
  html += F("<div class='field'><label>Modalità LED</label><select name='state'>");
  for (auto& s : states) {
    html += "<option value='" + String(s[0]) + "'";
    if (manualState == s[0]) html += " selected";
    html += ">" + String(s[1]) + "</option>";
  }
  html += F("</select></div><div class='field'><label>Geometria LED</label><select name='geometry'>");
  const char* geometryNames[] = {"strip", "ring", "matrix"};
  const char* geometryLabels[] = {"Striscia", "Cerchio / Ring", "Matrice"};
  for (uint8_t i = 0; i < 3; i++) {
    html += "<option value='" + String(geometryNames[i]) + "'";
    if ((uint8_t)ledGeometry == i) html += " selected";
    html += ">" + String(geometryLabels[i]) + "</option>";
  }
  html += F("</select></div><div class='field'><label>Animazione</label><select name='effect'>");
  const char* effectNames[] = {"static", "pulse", "rainbow", "wave", "comet", "rotation", "radar"};
  const char* effectLabels[] = {"Colore fisso", "Pulse / Respiro", "Rainbow", "Onda", "Cometa", "Rotazione", "Radar"};
  for (uint8_t i = 0; i < 7; i++) {
    html += "<option value='" + String(effectNames[i]) + "'";
    if ((uint8_t)ledEffect == i) html += " selected";
    html += ">" + String(effectLabels[i]) + "</option>";
  }
  html += F("</select></div><div class='field'><label>Larghezza matrice</label><input type='number' name='width' min='1' max='");
  html += String(LED_MAX_COUNT) + "' value='" + String(matrixWidth) + "'></div>";
  html += F("<div class='field'><label>Altezza matrice</label><input type='number' name='height' min='1' max='");
  html += String(LED_MAX_COUNT) + "' value='" + String(matrixHeight) + "'></div>";
  html += F("<div class='field'><label>Cablaggio matrice</label><select name='mapping'>");
  html += String("<option value='serpentine'") + (matrixSerpentine ? " selected" : "") + ">Serpentino</option>";
  html += String("<option value='linear'") + (!matrixSerpentine ? " selected" : "") + ">Lineare</option>";
  html += F("</select></div><div class='field full'><p class='field-note'>Dimensioni e cablaggio sono usati per la geometria matrice. Le animazioni si applicano agli stati Teams; i lampeggi diagnostici restano invariati.</p>"
            "<p class='field-note'>In modalità manuale i LED mantengono il colore scelto e non seguono temporaneamente la presenza Teams.</p>"
            "<button class='button primary' type='submit'>Salva impostazioni <span aria-hidden='true'>→</span></button>"
            "</div></form></div></section>");
  return html;
}

String pageHtml() {
  String html;
  html.reserve(10500);
  html += F("<!doctype html><html lang='it'><head><meta charset='utf-8'>"
            "<meta name='viewport' content='width=device-width,initial-scale=1'>"
            "<meta name='theme-color' content='#0b1020'>");
  // Refresh only during the Device Code flow so the displayed code remains current.
  if (deviceLoginPending) html += F("<meta http-equiv='refresh' content='5'>");
  html += F("<title>Teams Presence · ESP32</title><style>"
    ":root{color-scheme:dark;--bg:#0b1020;--surface:#121b2d;--surface2:#18243a;--line:rgba(160,180,220,.14);"
    "--text:#edf3ff;--muted:#91a1bc;--blue:#6c8cff;--green:#38d99a;--amber:#f5bb58;--red:#ff7185}"
    "*{box-sizing:border-box}body{margin:0;min-height:100vh;background:radial-gradient(ellipse at 12% 0%,rgba(70,104,210,.18),transparent 38%),"
    "radial-gradient(ellipse at 100% 28%,rgba(22,181,156,.09),transparent 32%),var(--bg);color:var(--text);"
    "font:15px/1.55 Inter,ui-sans-serif,system-ui,-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif}"
    ".shell{width:min(920px,100%);margin:0 auto;padding:30px 22px 42px}.topbar{display:flex;align-items:center;justify-content:space-between;"
    "gap:16px;margin-bottom:24px}.brand{display:flex;align-items:center;gap:13px}.brandmark{width:46px;height:46px;border-radius:15px;"
    "display:grid;place-items:center;background:linear-gradient(145deg,#6889ff,#7156d9);box-shadow:0 8px 30px rgba(85,108,255,.28);"
    "font-size:22px;font-weight:800}.brand h1{font-size:21px;letter-spacing:-.5px;margin:0;line-height:1.2}.brand h1 span{color:#91aaff}"
    ".eyebrow{font-size:10px;letter-spacing:1.7px;font-weight:800;color:#92a6ca;margin-bottom:4px}"
    ".top-pill,.pill{display:inline-flex;align-items:center;gap:7px;border:1px solid var(--line);border-radius:999px;padding:7px 11px;"
    "font-size:11px;font-weight:750;color:#b7c5dc;background:rgba(255,255,255,.035);white-space:nowrap}"
    ".dot{width:7px;height:7px;border-radius:50%;background:var(--green);box-shadow:0 0 12px currentColor}"
    ".hero{display:grid;grid-template-columns:minmax(0,1.3fr) minmax(220px,.7fr);gap:22px;padding:25px;"
    "border:1px solid rgba(139,164,255,.2);border-radius:22px;background:linear-gradient(115deg,rgba(39,57,95,.94),rgba(20,31,52,.96));"
    "box-shadow:0 18px 55px rgba(0,0,0,.2);margin-bottom:16px}.presence-line{display:flex;align-items:center;gap:12px;margin:8px 0 3px}"
    ".presence-title{font-size:clamp(26px,5vw,37px);line-height:1.12;letter-spacing:-1px;margin:0;font-weight:800}"
    ".presence-title.available{color:#61e4ad}.presence-title.busy{color:#ff8595}.presence-title.dnd{color:#c39cff}"
    ".presence-title.away{color:#ffd071}.presence-title.neutral{color:#e9efff}"
    ".presence-orb{width:13px;height:13px;border-radius:50%;background:#a5b5d2;box-shadow:0 0 0 6px rgba(165,181,210,.1)}"
    ".presence-orb.available{background:#43d99c;box-shadow:0 0 0 6px rgba(67,217,156,.13)}"
    ".presence-orb.busy{background:#ff7185;box-shadow:0 0 0 6px rgba(255,113,133,.13)}"
    ".presence-orb.dnd{background:#b58aff}.presence-orb.away{background:#f5bb58}"
    ".hero-sub{color:#b3c1d9;margin:8px 0 0}.hero-stats{display:grid;gap:10px;align-content:center}"
    ".metric{border:1px solid rgba(180,197,230,.13);border-radius:14px;padding:12px 14px;background:rgba(7,13,27,.22)}"
    ".metric span{display:block;color:#91a1bc;font-size:11px;margin-bottom:2px}.metric strong{font-size:14px;font-weight:750}"
    ".section{border:1px solid var(--line);border-radius:18px;background:linear-gradient(145deg,rgba(20,31,51,.96),rgba(15,24,41,.96));"
    "margin:13px 0;overflow:hidden;box-shadow:0 10px 30px rgba(0,0,0,.12)}"
    ".section-heading,details>summary{display:flex;align-items:center;gap:13px;padding:18px 20px}"
    "details>summary{cursor:pointer;list-style:none;user-select:none}details>summary::-webkit-details-marker{display:none}"
    ".summary-copy{display:flex;flex:1;min-width:0;flex-direction:column}.summary-copy strong{font-size:15px;font-weight:760}"
    ".summary-copy small{font-size:12px;color:var(--muted);margin-top:2px}.section-icon{width:37px;height:37px;flex:0 0 37px;"
    "border-radius:12px;display:grid;place-items:center;background:rgba(108,140,255,.13);color:#9cb1ff;font-size:10px;font-weight:850;letter-spacing:.4px}"
    ".chevron{color:#8294b4;font-size:20px;transition:transform .2s}details[open] .chevron{transform:rotate(180deg)}"
    ".section-body{padding:0 20px 20px;border-top:1px solid rgba(160,180,220,.09)}"
    ".muted{color:var(--muted);font-size:13px}.pill.success{color:#72e5b2;border-color:rgba(56,217,154,.28);background:rgba(56,217,154,.08)}"
    ".pill.warning{color:#ffd17b;border-color:rgba(245,187,88,.28);background:rgba(245,187,88,.08)}"
    ".pill.neutral{color:#aebfff}.form-grid{display:grid;grid-template-columns:1fr 1fr;gap:15px 18px;margin-top:14px}"
    ".field{min-width:0}.field.full{grid-column:1/-1}label{display:flex;justify-content:space-between;gap:10px;"
    "font-size:12px;font-weight:700;color:#c4d0e4;margin:0 0 7px}output{color:#9db1ff}"
    "input,select{width:100%;border:1px solid rgba(151,172,212,.2);border-radius:11px;padding:11px 12px;"
    "background:#0d1729;color:var(--text);font:inherit;outline:none;transition:border .15s,box-shadow .15s}"
    "input:focus,select:focus{border-color:#829cff;box-shadow:0 0 0 3px rgba(108,140,255,.13)}"
    "input[type=range]{padding:8px 0;border:0;background:transparent;accent-color:#829cff}"
    ".input-unit{display:flex;align-items:center;gap:10px}.input-unit span{color:var(--muted);font-size:12px;white-space:nowrap}"
    ".button{display:inline-flex;justify-content:center;align-items:center;gap:10px;border:0;border-radius:11px;"
    "padding:11px 15px;font:700 13px inherit;text-decoration:none;cursor:pointer;transition:transform .15s,filter .15s}"
    ".button:hover{filter:brightness(1.1);transform:translateY(-1px)}.button.primary{background:linear-gradient(110deg,#6586ff,#8066e9);"
    "color:white;box-shadow:0 7px 20px rgba(90,113,245,.2)}.button.danger{background:rgba(255,113,133,.1);"
    "color:#ff9aaa;border:1px solid rgba(255,113,133,.25)}.actions{display:flex;align-items:center;justify-content:flex-start}"
    ".field-note{font-size:12px;color:var(--muted);margin:0 0 12px}.hint{display:flex;gap:10px;margin-top:17px;padding:12px 13px;"
    "border-radius:12px;background:rgba(108,140,255,.07);color:#aab9d3;font-size:12px}.hint-icon{width:19px;height:19px;flex:0 0 19px;"
    "border-radius:50%;display:grid;place-items:center;background:rgba(108,140,255,.2);color:#a8b9ff;font-weight:800}"
    ".account-card{display:flex;align-items:center;justify-content:space-between;gap:15px;padding:17px 20px}"
    ".account-copy{display:flex;align-items:center;gap:12px;min-width:0}.account-mark{width:38px;height:38px;flex:0 0 38px;"
    "display:grid;place-items:center;border-radius:12px;background:rgba(56,217,154,.1);color:#62e2ac;font-size:17px}"
    ".account-card.pending .account-mark{background:rgba(245,187,88,.1);color:#ffd078}"
    ".account-card strong{display:block;font-size:14px}.account-card small{display:block;color:var(--muted);font-size:12px;margin-top:2px}"
    ".alert{border:1px solid rgba(255,113,133,.22);border-radius:13px;padding:12px 14px;background:rgba(255,113,133,.07);"
    "color:#ffb0ba;font-size:13px;margin:13px 0}.login-box{padding:18px 20px}.login-box .code{font-size:clamp(30px,8vw,42px);"
    "letter-spacing:5px;font-weight:850;color:#fff;background:rgba(108,140,255,.12);border:1px dashed rgba(145,166,255,.35);"
    "padding:13px 18px;border-radius:14px;display:inline-block;margin:10px 0}.login-box a{color:#aebfff}"
    ".network-line{display:flex;justify-content:space-between;gap:16px;align-items:center;margin:0 0 14px}"
    ".network-line span{color:var(--muted);font-size:12px}.network-line strong{font-size:13px;text-align:right;overflow-wrap:anywhere}"
    ".footer{text-align:center;color:#657795;font-size:11px;margin-top:24px}"
    "@media(max-width:650px){.shell{padding:20px 14px 32px}.topbar{align-items:flex-start}.top-pill{font-size:10px;padding:6px 8px}"
    ".hero{grid-template-columns:1fr;padding:20px}.hero-stats{grid-template-columns:1fr 1fr}.form-grid{grid-template-columns:1fr}"
    ".field.full{grid-column:auto}.summary-status{display:none}.account-card{align-items:flex-start;flex-direction:column}.account-card form{width:100%}"
    ".account-card .button{width:100%}.network-line{align-items:flex-start;flex-direction:column;gap:3px}.network-line strong{text-align:left}}"
    "</style></head><body><div class='shell'>"
    "<header class='topbar'><div class='brand'><div class='brandmark'>TP</div><div>"
    "<div class='eyebrow'>SMART PRESENCE · ESP32</div><h1>Teams <span>Presence</span></h1>"
    "</div></div><div class='top-pill'><i class='dot'></i> Dispositivo online</div></header>"
  );

  String presenceClass = "neutral";
  if (presenceText == "Available") presenceClass = "available";
  else if (presenceText == "Busy") presenceClass = "busy";
  else if (presenceText == "DoNotDisturb") presenceClass = "dnd";
  else if (presenceText == "Away" || presenceText == "BeRightBack") presenceClass = "away";

  html += F("<section class='hero'><div><div class='eyebrow'>PRESENZA MICROSOFT TEAMS</div><div class='presence-line'><span class='presence-orb ");
  html += presenceClass + "'></span><h2 class='presence-title " + presenceClass + "'>";
  html += htmlEscape(presenceText);
  html += F("</h2></div><p class='hero-sub'>Attività: ");
  html += htmlEscape(activityText);
  html += F("</p></div><div class='hero-stats'><div class='metric'><span>Account Microsoft</span><strong>");
  html += presenceReady ? F("Collegato e operativo") : (graphReady ? F("Token acquisito · verifica Graph") : F("Non collegato"));
  html += F("</strong></div><div class='metric'><span>Aggiornamento presenza</span><strong>Ogni ");
  html += String(pollIntervalMs / 1000UL) + " secondi</strong></div></div></section>";

  if (oauthError.length()) {
    html += F("<div class='alert'><b>Attenzione</b><br>");
    html += htmlEscape(oauthError);
    html += F("</div>");
  }

  if (deviceLoginPending) {
    html += F("<section class='card section login-box'><div class='eyebrow'>AUTENTICAZIONE MICROSOFT</div>"
              "<h2 style='margin:4px 0 6px;font-size:19px'>Completa l'accesso</h2>"
              "<p class='muted'>Apri la pagina Microsoft, inserisci il codice visualizzato e conferma l'accesso all'account aziendale.</p>"
              "<p><a href='");
    html += htmlEscape(verificationUri);
    html += F("' target='_blank' rel='noopener'>Apri Microsoft Device Login ↗</a></p><div class='code'>");
    html += htmlEscape(userCode);
    html += F("</div><p class='muted'>Il codice viene verificato automaticamente. Lascia acceso l'ESP32.</p></section>");
  } else if (presenceReady) {
    html += F("<section class='card account-card'><div class='account-copy'><span class='account-mark'>✓</span>"
              "<div><strong>Account Microsoft collegato</strong><small>");
    html += htmlEscape(authUser);
    html += F(" · Microsoft Graph risponde correttamente</small></div></div>"
              "<form method='post' action='/logout' onsubmit=\"return confirm('Vuoi scollegare l’account Microsoft?')\">"
              "<button class='button danger' type='submit'>Disconnetti</button></form></section>");
  } else if (entraConfigured()) {
    html += F("<section class='card account-card pending'><div class='account-copy'><span class='account-mark'>!</span>"
              "<div><strong>Account non operativo</strong><small>Collega o autorizza nuovamente l'account Microsoft per leggere la presenza.</small></div></div>"
              "<form method='post' action='/login'><button class='button primary' type='submit'>Collega Microsoft →</button></form></section>");
  } else {
    html += F("<section class='card account-card pending'><div class='account-copy'><span class='account-mark'>!</span>"
              "<div><strong>Configurazione necessaria</strong><small>Inserisci Tenant ID e Client ID per iniziare.</small></div></div></section>");
  }

  html += entraSettingsHtml();
  html += F("<section class='card section'><div class='section-heading'><span class='section-icon'>OTA</span>"
            "<span class='summary-copy'><strong>Aggiornamento firmware</strong><small>Installa nuove versioni direttamente via Wi-Fi</small></span>"
            "<span class='pill neutral'>");
  html += FIRMWARE_VERSION;
  html += F("</span></div><div class='section-body'><p class='muted'>Controlla le release pubblicate e aggiorna il dispositivo senza collegare il cavo USB.</p>"
            "<div id='otaStatus' class='field-note' role='status'>Versione installata: ");
  html += FIRMWARE_VERSION;
  html += F("</div><div class='actions' style='gap:10px;flex-wrap:wrap'><button id='otaCheck' class='button' "
            "style='background:#263653;color:#dce6f8' type='button'>Controlla aggiornamenti</button>"
            "<button id='otaInstall' class='button primary' type='button' hidden>Aggiorna firmware →</button></div>"
            "<p class='field-note' style='margin-top:13px'>Durante l'installazione non spegnere l'ESP32. Le impostazioni personali non vengono cancellate.</p>"
            "<script>"
            "(()=>{const check=document.getElementById('otaCheck'),install=document.getElementById('otaInstall'),status=document.getElementById('otaStatus');"
            "let available=false;const message=(text)=>{status.textContent=text};"
            "check.addEventListener('click',async()=>{check.disabled=true;install.hidden=true;message('Controllo delle release GitHub in corso…');"
            "try{const r=await fetch('/api/ota/check',{cache:'no-store'});const d=await r.json();if(!r.ok||!d.ok)throw new Error(d.message||'Controllo non riuscito');"
            "available=!!d.available;if(available){message('Disponibile '+d.latestVersion+(d.channel==='prerelease'?' · versione di test':'')+'.');install.hidden=false;}"
            "else{message('Firmware aggiornato · '+d.currentVersion);}}catch(e){message('Errore: '+e.message);}finally{check.disabled=false;}});"
            "install.addEventListener('click',async()=>{if(!available||!confirm('Installare il nuovo firmware? Il dispositivo si riavvierà automaticamente.'))return;"
            "check.disabled=true;install.disabled=true;message('Download e verifica firmware in corso. Non spegnere il dispositivo…');"
            "try{const r=await fetch('/api/ota/update',{method:'POST'});const t=await r.text();if(!r.ok)throw new Error(t);message(t);}"
            "catch(e){message('Aggiornamento non completato: '+e.message+' · Puoi riprovare.');check.disabled=false;install.disabled=false;}});})();"
            "</script></div></section>");
  html += settingsFormHtml();

  html += F("<section class='card section'><div class='section-heading'><span class='section-icon'>NET</span>"
            "<span class='summary-copy'><strong>Connessione di rete</strong><small>Stato e riconfigurazione Wi-Fi</small></span>"
            "<span class='pill success'>");
  html += WiFi.isConnected() ? F("Connesso") : F("Disconnesso");
  html += F("</span></div><div class='section-body'><div class='network-line'><span>Rete Wi-Fi</span><strong>");
  html += htmlEscape(WiFi.SSID().length() ? WiFi.SSID() : String("Non disponibile"));
  html += F("</strong></div><div class='network-line'><span>Indirizzo del dispositivo</span><strong>");
  html += WiFi.localIP().toString();
  html += F("</strong></div><form method='post' action='/wifi' onsubmit=\"return confirm('Il dispositivo si riavvia e apre il portale di configurazione Wi-Fi. Continuare?')\">"
            "<button class='button' style='background:#263653;color:#dce6f8' type='submit'>Cambia rete Wi-Fi</button></form></div></section>"
            "<footer class='footer'>Teams Presence · Configurazione locale ESP32</footer></div></body></html>");
  return html;
}
void handleRoot() {
  server.send(200, "text/html; charset=utf-8", pageHtml());
}

void handleLogin() {
  if (!entraConfigured()) {
    oauthError = "Configura prima Tenant ID e Client ID nella pagina del dispositivo.";
    server.send(400, "text/plain; charset=utf-8", oauthError);
    return;
  }
  if (!WiFi.isConnected()) {
    server.send(503, "text/plain", "Wi-Fi non connesso");
    return;
  }

  String body = "client_id=" + encodeForm(entraClientId) +
                "&scope=" + encodeForm("https://graph.microsoft.com/Presence.Read offline_access");

  String response;
  int status = 0;
  if (!httpsRequest(deviceCodeUrl(), "POST", body, "", response, status) || status != 200) {
    oauthError = "Richiesta device code fallita (HTTP " + String(status) + ")";
    server.send(502, "text/plain", oauthError);
    return;
  }

  JsonDocument doc;
  if (deserializeJson(doc, response)) {
    server.send(502, "text/plain", "Risposta OAuth non valida");
    return;
  }

  deviceCode = doc["device_code"] | "";
  userCode = doc["user_code"] | "";
  verificationUri = doc["verification_uri"] | "https://microsoft.com/devicelogin";
  deviceCodeExpires = doc["expires_in"] | 900;
  deviceCodeStartedAt = millis();
  devicePollInterval = doc["interval"] | 5;

  if (!deviceCode.length() || !userCode.length()) {
    server.send(502, "text/plain", "Device code mancante");
    return;
  }

  deviceLoginPending = true;
  nextDevicePoll = millis() + devicePollInterval * 1000UL;
  oauthError = "";
  if (!manualState.length()) {
    setLedPattern(LOGIN_PENDING_PATTERN,
                  sizeof(LOGIN_PENDING_PATTERN) / sizeof(LOGIN_PENDING_PATTERN[0]),
                  "Microsoft Device Code login pending: purple pulse");
  }
  server.sendHeader("Location", "/");
  server.send(303);
}

void handleEntraSave() {
  String tenant = server.arg("tenant");
  String client = server.arg("client");
  tenant.trim();
  client.trim();

  if (!isGuid(tenant) || !isGuid(client)) {
    server.send(400, "text/plain; charset=utf-8",
                "Tenant ID e Client ID devono essere GUID validi (36 caratteri).");
    return;
  }

  const bool changed = tenant != entraTenantId || client != entraClientId;
  entraTenantId = tenant;
  entraClientId = client;
  prefs.putString("tenantId", entraTenantId);
  prefs.putString("clientId", entraClientId);

  if (changed) {
    // Tokens are bound to the previous Entra application/tenant: force a fresh sign-in.
    prefs.remove("refresh");
    refreshToken = "";
    accessToken = "";
    tokenExpiryEpoch = 0;
    graphReady = false;
    presenceReady = false;
    deviceLoginPending = false;
    deviceCode = "";
    userCode = "";
    authUser = "Non autenticato";
    presenceText = "Configurazione Microsoft salvata";
    activityText = "-";
    setSolid(CRGB::Black);
  }
  oauthError = "";
  server.sendHeader("Location", "/");
  server.send(303);
}

void handleSettingsRedirect() {
  server.sendHeader("Location", "/");
  server.send(303);
}

void handleSettingsSave() {
  if (server.hasArg("leds")) {
    activeLeds = constrain(server.arg("leds").toInt(), 1, LED_MAX_COUNT);
    prefs.putUShort("leds", activeLeds);
  }
  if (server.hasArg("brightness")) {
    brightnessPct = constrain(server.arg("brightness").toInt(), 1, 100);
    prefs.putUChar("bright", brightnessPct);
    FastLED.setBrightness((brightnessPct * 255 + 50) / 100);
    if (manualState.length()) applyManualState();
  }
  if (server.hasArg("refresh")) {
    uint32_t sec = constrain(server.arg("refresh").toInt(),
                             PRESENCE_POLL_MIN_S, PRESENCE_POLL_MAX_S);
    pollIntervalMs = sec * 1000UL;
    prefs.putUInt("pollSec", sec);
  }
  if (server.hasArg("geometry")) {
    String value = server.arg("geometry");
    if (value == "ring") ledGeometry = LedGeometry::Ring;
    else if (value == "matrix") ledGeometry = LedGeometry::Matrix;
    else ledGeometry = LedGeometry::Strip;
    prefs.putUChar("geometry", (uint8_t)ledGeometry);
  }
  if (server.hasArg("effect")) {
    String value = server.arg("effect");
    if (value == "pulse") ledEffect = LedEffect::Pulse;
    else if (value == "rainbow") ledEffect = LedEffect::Rainbow;
    else if (value == "wave") ledEffect = LedEffect::Wave;
    else if (value == "comet") ledEffect = LedEffect::Comet;
    else if (value == "rotation") ledEffect = LedEffect::Rotation;
    else if (value == "radar") ledEffect = LedEffect::Radar;
    else ledEffect = LedEffect::Static;
    prefs.putUChar("effect", (uint8_t)ledEffect);
  }
  if (server.hasArg("width")) {
    matrixWidth = constrain(server.arg("width").toInt(), 1, LED_MAX_COUNT);
    prefs.putUShort("width", matrixWidth);
  }
  if (server.hasArg("height")) {
    matrixHeight = constrain(server.arg("height").toInt(), 1, LED_MAX_COUNT);
    prefs.putUShort("height", matrixHeight);
  }
  if (server.hasArg("mapping")) {
    matrixSerpentine = server.arg("mapping") == "serpentine";
    prefs.putBool("serpentine", matrixSerpentine);
  }
  if (server.hasArg("state")) {
    String s = server.arg("state");
    CRGB c;
    manualState = stateColor(s, c) ? s : "";
    prefs.putString("manual", manualState);
  }

  ledRenderer.begin(leds, LED_MAX_COUNT, activeLeds);
  if (manualState.length()) {
    applyManualState();
  } else if (graphReady) {
    lastPresencePoll = 0;  // refresh LEDs on the next loop
    pollPresence();
  } else {
    showColor(CRGB::Black);
  }
  server.sendHeader("Location", "/");
  server.send(303);
}

void handleWifiChange() {
  prefs.putBool("wifiPortal", true);
  server.send(200, "text/html; charset=utf-8",
    "<!doctype html><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<body style='font-family:system-ui;max-width:620px;margin:35px auto;padding:0 18px'>"
    "<h2>Cambio rete Wi-Fi</h2>"
    "<p>Il dispositivo si riavvia. Collegati alla rete <b>TeamsPresence-Setup</b> e "
    "scegli la nuova rete (portale su http://192.168.4.1/).</p>"
    "<p>Se non completi la procedura entro 3 minuti, il dispositivo si riconnette "
    "alla rete precedente.</p></body>");
  delay(500);
  ESP.restart();
}

void handleLogout() {
  prefs.remove("refresh");
  refreshToken = "";
  accessToken = "";
  graphReady = false;
  presenceReady = false;
  authUser = "Non autenticato";
  presenceText = "Account scollegato";
  activityText = "-";
  deviceLoginPending = false;
  setSolid(CRGB::Black);
  server.sendHeader("Location", "/");
  server.send(303);
}

bool parseTokenResponse(const String& response, int status) {
  JsonDocument doc;
  if (deserializeJson(doc, response)) return false;
  if (status != 200) {
    String err = doc["error"] | "oauth_error";
    oauthError = err + ": " + String((const char*)(doc["error_description"] | ""));
    if (err == "invalid_grant") {
      prefs.remove("refresh");
      refreshToken = "";
    }
    return false;
  }

  accessToken = doc["access_token"] | "";
  if (!accessToken.length()) return false;

  String newRefresh = doc["refresh_token"] | "";
  if (newRefresh.length()) {
    refreshToken = newRefresh;
    prefs.putString("refresh", refreshToken);
  }

  // NTP epoch is used for expiry; refresh slightly before expiration.
  uint32_t expiresIn = doc["expires_in"] | 3600;
  tokenExpiryEpoch = (uint32_t)time(nullptr) + (expiresIn > 120 ? expiresIn - 120 : expiresIn);
  graphReady = true;
  oauthError = "";
  return true;
}

void pollPresence();

void pollDeviceCode() {
  if (!deviceLoginPending || (int32_t)(millis() - nextDevicePoll) < 0) return;

  if ((uint32_t)(millis() - deviceCodeStartedAt) >= deviceCodeExpires * 1000UL) {
    deviceLoginPending = false;
    oauthError = "Codice scaduto. Avvia nuovamente il login.";
    if (!manualState.length()) {
      setLedPattern(WAITING_MICROSOFT_PATTERN,
                    sizeof(WAITING_MICROSOFT_PATTERN) / sizeof(WAITING_MICROSOFT_PATTERN[0]),
                    "Microsoft login required: slow blue pulse");
    }
    return;
  }

  String body =
    "client_id=" + encodeForm(entraClientId) +
    "&grant_type=" + encodeForm("urn:ietf:params:oauth:grant-type:device_code") +
    "&device_code=" + encodeForm(deviceCode);

  String response;
  int status = 0;
  bool ok = httpsRequest(tokenUrl(), "POST", body, "", response, status);
  nextDevicePoll = millis() + devicePollInterval * 1000UL;

  if (!ok) return;

  JsonDocument doc;
  if (deserializeJson(doc, response)) return;

  if (status == 200) {
    if (parseTokenResponse(response, status)) {
      deviceLoginPending = false;
      authUser = "Account autenticato (endpoint /me)";
      presenceText = "Autenticato – lettura presenza...";
      setSolid(CRGB::Green);
      pollPresence();
    }
    return;
  }

  String err = doc["error"] | "";
  if (err == "authorization_pending") return;
  if (err == "slow_down") {
    devicePollInterval += 5;
    return;
  }

  oauthError = err + ": " + String((const char*)(doc["error_description"] | ""));
  if (err == "authorization_declined" || err == "expired_token" ||
      err == "bad_verification_code") {
    deviceLoginPending = false;
    if (!manualState.length()) {
      setLedPattern(WAITING_MICROSOFT_PATTERN,
                    sizeof(WAITING_MICROSOFT_PATTERN) / sizeof(WAITING_MICROSOFT_PATTERN[0]),
                    "Microsoft authorization failed: slow blue pulse");
    }
  }
}

bool refreshAccessToken() {
  if (!entraConfigured() || !refreshToken.length()) return false;

  String body =
    "client_id=" + encodeForm(entraClientId) +
    "&grant_type=refresh_token" +
    "&refresh_token=" + encodeForm(refreshToken) +
    "&scope=" + encodeForm("https://graph.microsoft.com/Presence.Read offline_access");

  String response;
  int status = 0;
  if (!httpsRequest(tokenUrl(), "POST", body, "", response, status)) return false;
  return parseTokenResponse(response, status);
}

void applyPresenceColor(const String& availability) {
  CRGB c;
  if (availability != "Error" && stateColor(availability, c)) setSolid(c);
  else setErrorBlink();
}

void pollPresence() {
  if (!graphReady || !accessToken.length()) return;
  lastPresencePoll = millis();

  uint32_t nowEpoch = (uint32_t)time(nullptr);
  if (nowEpoch > 1700000000UL && nowEpoch >= tokenExpiryEpoch) {
    if (!refreshAccessToken()) {
      graphReady = false;
      presenceReady = false;
      presenceText = "Sessione scaduta: riconnettere Microsoft";
      if (!manualState.length()) {
        setLedPattern(WAITING_MICROSOFT_PATTERN,
                      sizeof(WAITING_MICROSOFT_PATTERN) / sizeof(WAITING_MICROSOFT_PATTERN[0]),
                      "Microsoft session expired: slow blue pulse");
      }
      return;
    }
  }

  String response;
  int status = 0;
  if (!httpsRequest(graphPresenceUrl, "GET", "", accessToken, response, status)) {
    presenceReady = false;
    presenceText = "Errore connessione Graph";
    setErrorBlink();
    return;
  }

  if (status == 401) {
    if (!refreshAccessToken()) {
      graphReady = false;
      presenceReady = false;
      presenceText = "Autenticazione richiesta";
      if (!manualState.length()) {
        setLedPattern(WAITING_MICROSOFT_PATTERN,
                      sizeof(WAITING_MICROSOFT_PATTERN) / sizeof(WAITING_MICROSOFT_PATTERN[0]),
                      "Microsoft authentication required: slow blue pulse");
      }
      return;
    }
    if (!httpsRequest(graphPresenceUrl, "GET", "", accessToken, response, status)) {
      presenceReady = false;
      presenceText = "Errore connessione Graph";
      setErrorBlink();
      return;
    }
  }

  if (status != 200) {
    presenceReady = false;
    presenceText = "Graph HTTP " + String(status);
    setErrorBlink();
    return;
  }

  JsonDocument doc;
  if (deserializeJson(doc, response)) {
    presenceReady = false;
    presenceText = "JSON Graph non valido";
    setErrorBlink();
    return;
  }

  String availability = doc["availability"] | "Unknown";
  activityText = doc["activity"] | "Unknown";
  presenceText = availability;
  presenceReady = (availability != "Unknown" && availability.length() > 0);
  applyPresenceColor(availability);
  lastPresencePoll = millis();
}


bool isHexSha256(const String& value) {
  if (value.length() != 64) return false;
  for (size_t i = 0; i < value.length(); i++) {
    const char c = value[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
          (c >= 'A' && c <= 'F'))) return false;
  }
  return true;
}

bool parseFirmwareVersion(String value, int& major, int& minor, int& patch,
                          String& prerelease) {
  value.trim();
  if (value.startsWith("v") || value.startsWith("V")) value.remove(0, 1);
  const int plus = value.indexOf('+');
  if (plus >= 0) value = value.substring(0, plus);
  const int dash = value.indexOf('-');
  prerelease = dash >= 0 ? value.substring(dash + 1) : "";
  String core = dash >= 0 ? value.substring(0, dash) : value;

  int values[3] = {0, 0, 0};
  int part = 0;
  String number;
  for (size_t i = 0; i <= core.length(); i++) {
    const char c = i < core.length() ? core[i] : '.';
    if (c == '.') {
      if (!number.length() || part >= 3) return false;
      for (size_t j = 0; j < number.length(); j++) {
        if (number[j] < '0' || number[j] > '9') return false;
      }
      values[part++] = number.toInt();
      number = "";
    } else {
      number += c;
    }
  }
  if (part != 3) return false;
  major = values[0];
  minor = values[1];
  patch = values[2];
  return true;
}

bool prereleaseIsNewer(const String& latest, const String& current) {
  if (!latest.length()) return current.length() > 0;  // Stable is newer than a prerelease.
  if (!current.length()) return false;

  const int latestDot = latest.lastIndexOf('.');
  const int currentDot = current.lastIndexOf('.');
  const String latestPrefix = latestDot >= 0 ? latest.substring(0, latestDot) : "";
  const String currentPrefix = currentDot >= 0 ? current.substring(0, currentDot) : "";
  const String latestNumber = latestDot >= 0 ? latest.substring(latestDot + 1) : latest;
  const String currentNumber = currentDot >= 0 ? current.substring(currentDot + 1) : current;
  if (latestPrefix == currentPrefix && latestNumber.length() && currentNumber.length()) {
    bool numeric = true;
    for (size_t i = 0; i < latestNumber.length(); i++)
      if (latestNumber[i] < '0' || latestNumber[i] > '9') numeric = false;
    for (size_t i = 0; i < currentNumber.length(); i++)
      if (currentNumber[i] < '0' || currentNumber[i] > '9') numeric = false;
    if (numeric) return latestNumber.toInt() > currentNumber.toInt();
  }
  return latest > current;
}

bool isNewerFirmwareVersion(const String& latest, const String& current) {
  if (!latest.length() || latest == current) return false;
  int lMajor, lMinor, lPatch, cMajor, cMinor, cPatch;
  String lPre, cPre;
  if (!parseFirmwareVersion(latest, lMajor, lMinor, lPatch, lPre) ||
      !parseFirmwareVersion(current, cMajor, cMinor, cPatch, cPre)) {
    // Do not offer updates for unknown/development version schemes: that could downgrade a dev build.
    return false;
  }
  if (lMajor != cMajor) return lMajor > cMajor;
  if (lMinor != cMinor) return lMinor > cMinor;
  if (lPatch != cPatch) return lPatch > cPatch;
  return prereleaseIsNewer(lPre, cPre);
}

bool fetchOtaManifest(JsonDocument& manifest, String& error) {
  String response;
  int status = 0;
  if (!httpsRequest(String(otaManifestUrl), "GET", "", "", response, status)) {
    error = "Impossibile raggiungere il servizio aggiornamenti.";
    return false;
  }
  if (status != 200) {
    error = "Manifest aggiornamenti HTTP " + String(status) + ".";
    return false;
  }
  if (deserializeJson(manifest, response)) {
    error = "Manifest aggiornamenti non valido.";
    return false;
  }

  const String version = manifest["version"] | "";
  const String firmwareUrl = manifest["firmware_url"] | "";
  const String digest = manifest["sha256"] | "";
  const uint32_t size = manifest["size"] | 0U;
  if (!version.length() || !firmwareUrl.startsWith(otaFirmwarePrefix) ||
      !isHexSha256(digest) || size == 0) {
    error = "Il manifest non supera i controlli di validazione.";
    return false;
  }
  return true;
}

void handleOtaCheck() {
  JsonDocument result;
  result["currentVersion"] = FIRMWARE_VERSION;
  JsonDocument manifest;
  String error;
  if (!fetchOtaManifest(manifest, error)) {
    result["ok"] = false;
    result["available"] = false;
    result["message"] = error;
    String payload;
    serializeJson(result, payload);
    server.send(502, "application/json; charset=utf-8", payload);
    return;
  }

  const String latest = manifest["version"] | "";
  const bool available = isNewerFirmwareVersion(latest, String(FIRMWARE_VERSION));
  result["ok"] = true;
  result["available"] = available;
  result["latestVersion"] = latest;
  result["channel"] = manifest["channel"] | "stable";
  result["size"] = manifest["size"] | 0U;
  result["message"] = available ? "È disponibile un nuovo firmware." : "Il firmware è già aggiornato.";
  String payload;
  serializeJson(result, payload);
  server.send(200, "application/json; charset=utf-8", payload);
}

bool performOtaUpdate(String& error) {
  JsonDocument manifest;
  if (!fetchOtaManifest(manifest, error)) return false;

  const String latest = manifest["version"] | "";
  if (!isNewerFirmwareVersion(latest, String(FIRMWARE_VERSION))) {
    error = "Non è disponibile una versione più recente.";
    return false;
  }

  const String firmwareUrl = manifest["firmware_url"] | "";
  String expectedSha = manifest["sha256"] | "";
  expectedSha.toLowerCase();
  const uint32_t expectedSize = manifest["size"] | 0U;

  WiFiClientSecure client;
  client.setCACertBundle(x509_crt_bundle, x509_crt_bundle_len);
  HTTPClient http;
  http.setTimeout(30000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  if (!http.begin(client, firmwareUrl)) {
    error = "Impossibile iniziare il download del firmware.";
    return false;
  }

  const int status = http.GET();
  if (status != HTTP_CODE_OK) {
    error = "Download firmware fallito (HTTP " + String(status) + ").";
    http.end();
    return false;
  }

  const int contentLength = http.getSize();
  if (contentLength <= 0 || (uint32_t)contentLength != expectedSize) {
    error = "Dimensione firmware inattesa: download interrotto.";
    http.end();
    return false;
  }

  if (!Update.begin(expectedSize, U_FLASH)) {
    error = "Spazio OTA insufficiente o partizione non disponibile.";
    Update.printError(Serial);
    http.end();
    return false;
  }

  const mbedtls_md_info_t* mdInfo = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  mbedtls_md_context_t shaContext;
  mbedtls_md_init(&shaContext);
  bool shaReady = mdInfo && mbedtls_md_setup(&shaContext, mdInfo, 0) == 0 &&
                  mbedtls_md_starts(&shaContext) == 0;
  if (!shaReady) {
    error = "Impossibile inizializzare SHA-256.";
    Update.abort();
    mbedtls_md_free(&shaContext);
    http.end();
    return false;
  }

  Serial.printf("[OTA] Downloading %s (%u bytes)\n", latest.c_str(), expectedSize);
  showColor(CRGB::Blue);
  auto* stream = http.getStreamPtr();
  if (!stream) {
    Update.abort();
    mbedtls_md_free(&shaContext);
    http.end();
    error = "Connessione download non disponibile.";
    return false;
  }
  uint8_t buffer[1024];
  uint32_t remaining = expectedSize;
  uint32_t written = 0;
  uint32_t lastDataAt = millis();
  bool transferOk = true;

  while (remaining > 0) {
    const int available = stream->available();
    if (available <= 0) {
      if (!http.connected() && stream->available() <= 0) {
        transferOk = false;
        error = "Download interrotto prima del completamento.";
        break;
      }
      if ((uint32_t)(millis() - lastDataAt) > 30000UL) {
        transferOk = false;
        error = "Timeout durante il download del firmware.";
        break;
      }
      delay(2);
      continue;
    }

    const size_t chunk = (size_t)min((uint32_t)available,
                                     (uint32_t)sizeof(buffer));
    const int count = stream->read(buffer, chunk);
    if (count <= 0) continue;
    lastDataAt = millis();

    if (mbedtls_md_update(&shaContext, buffer, (size_t)count) != 0 ||
        Update.write(buffer, (size_t)count) != (size_t)count) {
      transferOk = false;
      error = "Errore durante la scrittura del firmware nella partizione OTA.";
      break;
    }
    written += (uint32_t)count;
    remaining -= (uint32_t)count;
  }

  unsigned char digest[32];
  const bool hashOk = transferOk && written == expectedSize &&
                      mbedtls_md_finish(&shaContext, digest) == 0;
  mbedtls_md_free(&shaContext);
  http.end();

  if (!transferOk || !hashOk) {
    Update.abort();
    if (error.length() == 0) error = "Download firmware incompleto.";
    return false;
  }

  char actualSha[65];
  for (size_t i = 0; i < sizeof(digest); i++) {
    snprintf(actualSha + (i * 2), 3, "%02x", digest[i]);
  }
  actualSha[64] = '\0';
  if (expectedSha != String(actualSha)) {
    Update.abort();
    error = "SHA-256 non corrispondente: firmware rifiutato.";
    return false;
  }

  if (!Update.end()) {
    Update.printError(Serial);
    error = "La verifica finale del firmware non è riuscita.";
    return false;
  }

  Serial.printf("[OTA] Firmware %s verified and installed. Rebooting.\n", latest.c_str());
  return true;
}

void handleOtaUpdate() {
  if (!WiFi.isConnected()) {
    server.send(503, "text/plain; charset=utf-8", "Wi-Fi non connesso.");
    return;
  }

  String error;
  if (!performOtaUpdate(error)) {
    server.send(502, "text/plain; charset=utf-8", error);
    return;
  }

  server.send(200, "text/plain; charset=utf-8",
              "Aggiornamento installato correttamente. Riavvio in corso...");
  delay(1200);
  ESP.restart();
}


void startApplicationServices() {
  if (servicesStarted || !WiFi.isConnected()) return;

  wifiReady = true;
  wifiPortalActive = false;
  Serial.println("[WiFi] Connected successfully.");
  Serial.print("[WiFi] SSID: ");
  Serial.println(WiFi.SSID());
  Serial.print("[WiFi] IP address: http://");
  Serial.println(WiFi.localIP());
  setLedPattern(WIFI_CONNECTED_PATTERN,
                sizeof(WIFI_CONNECTED_PATTERN) / sizeof(WIFI_CONNECTED_PATTERN[0]),
                "Wi-Fi connected: 2 green flashes");

  configTime(0, 0, "pool.ntp.org", "time.google.com");
  time_t now = 0;
  for (int i = 0; i < 30; i++) {
    time(&now);
    if (now > 1700000000) break;
    updateLedPattern();
    delay(500);
  }
  if (now <= 1700000000) {
    Serial.println("[TIME] NTP not synchronised yet; TLS requests may fail until time is available.");
  } else {
    Serial.println("[TIME] NTP synchronised.");
  }

  server.on("/", HTTP_GET, handleRoot);
  server.on("/login", HTTP_POST, handleLogin);
  server.on("/entra", HTTP_POST, handleEntraSave);
  server.on("/logout", HTTP_POST, handleLogout);
  server.on("/wifi", HTTP_POST, handleWifiChange);
  server.on("/api/ota/check", HTTP_GET, handleOtaCheck);
  server.on("/api/ota/update", HTTP_POST, handleOtaUpdate);
  server.on("/settings", HTTP_GET, handleSettingsRedirect);
  server.on("/settings", HTTP_POST, handleSettingsSave);
  server.begin();
  servicesStarted = true;

  Serial.print("[HTTP] Configuration page: http://");
  Serial.println(WiFi.localIP());

  if (refreshToken.length() && entraConfigured()) {
    Serial.println("[Microsoft] Trying to restore the saved Microsoft session...");
    if (refreshAccessToken()) {
      authUser = "Account autenticato (endpoint /me)";
      graphReady = true;
      Serial.println("[Microsoft] Session restored.");
      if (manualState.length()) {
        stopLedPattern();
        applyManualState();
      } else {
        setSolid(CRGB::Green);
      }
      pollPresence();
      return;
    }
    graphReady = false;
    Serial.println("[Microsoft] Refresh token invalid or expired; sign in from the local page.");
  } else if (refreshToken.length() && !entraConfigured()) {
    Serial.println("[Microsoft] Removing a refresh token without matching Entra IDs.");
    prefs.remove("refresh");
    refreshToken = "";
  }

  if (manualState.length()) {
    stopLedPattern();
    applyManualState();
  } else {
    setLedPattern(WAITING_MICROSOFT_PATTERN,
                  sizeof(WAITING_MICROSOFT_PATTERN) / sizeof(WAITING_MICROSOFT_PATTERN[0]),
                  "Wi-Fi ready; waiting for Microsoft setup/login: slow blue pulse");
  }
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("====================================");
  Serial.println(" Teams Presence - ESP32 boot");
  Serial.println(" Serial diagnostics: 115200 baud");
  Serial.println("====================================");
  Serial.printf("[BOOT] Flash size: %u bytes\n", ESP.getFlashChipSize());
  Serial.printf("[BOOT] Free heap: %u bytes\n", ESP.getFreeHeap());

  prefs.begin("teams-presence", false);
  const String defaultTenant = isGuid(String(MS_TENANT_ID)) ? String(MS_TENANT_ID) : "";
  const String defaultClient = isGuid(String(MS_CLIENT_ID)) ? String(MS_CLIENT_ID) : "";
  entraTenantId = prefs.getString("tenantId", defaultTenant);
  entraClientId = prefs.getString("clientId", defaultClient);
  refreshToken = prefs.getString("refresh", "");
  activeLeds = constrain(prefs.getUShort("leds", LED_COUNT), 1, LED_MAX_COUNT);
  pollIntervalMs = constrain(prefs.getUInt("pollSec", PRESENCE_POLL_MS / 1000UL),
                             PRESENCE_POLL_MIN_S, PRESENCE_POLL_MAX_S) * 1000UL;
  manualState = prefs.getString("manual", "");

  FastLED.addLeds<LED_CHIPSET, LED_PIN, GRB>(leds, LED_MAX_COUNT);
  brightnessPct = constrain(prefs.getUChar("bright", brightnessPct), 1, 100);
  FastLED.setBrightness((brightnessPct * 255 + 50) / 100);
  ledGeometry = (LedGeometry)constrain(prefs.getUChar("geometry", 0), 0, 2);
  ledEffect = (LedEffect)constrain(prefs.getUChar("effect", 0), 0, 6);
  matrixWidth = constrain(prefs.getUShort("width", 8), 1, LED_MAX_COUNT);
  matrixHeight = constrain(prefs.getUShort("height", 8), 1, LED_MAX_COUNT);
  matrixSerpentine = prefs.getBool("serpentine", true);
  ledRenderer.begin(leds, LED_MAX_COUNT, activeLeds);
  setLedPattern(BOOT_PATTERN,
                sizeof(BOOT_PATTERN) / sizeof(BOOT_PATTERN[0]),
                "Boot: 2 white flashes");
  // Play the short boot signature before starting the Wi-Fi connection attempt.
  for (uint16_t i = 0; i < 100; i++) {
    updateLedPattern();
    delay(10);
  }

  // Hold the ESP32 BOOT button (GPIO0) while powering on to forget old Wi-Fi credentials.
  pinMode(0, INPUT_PULLUP);
  delay(20);
  if (digitalRead(0) == LOW) {
    Serial.println("[WiFi] BOOT button held: erasing saved Wi-Fi credentials.");
    wifiManager.resetSettings();
    prefs.putBool("wifiPortal", false);
  }

  WiFi.mode(WIFI_STA);
  wifiManager.setConfigPortalBlocking(false);
  wifiManager.setConfigPortalTimeout(600);  // Keep the setup AP available for up to 10 minutes.
  wifiManager.setConnectTimeout(25);
  wifiManager.setDebugOutput(true, WM_DEBUG_VERBOSE);

  wifiManager.setAPCallback([](WiFiManager* manager) {
    wifiPortalActive = true;
    Serial.println();
    Serial.println("[WiFi] SETUP ACCESS POINT STARTED.");
    Serial.print("[WiFi] SSID: ");
    Serial.println(manager->getConfigPortalSSID());
    Serial.print("[WiFi] Portal address: http://");
    Serial.println(WiFi.softAPIP());
    Serial.println("[WiFi] Connect your phone/PC to this AP, then open http://192.168.4.1/");
    setLedPattern(WIFI_PORTAL_PATTERN,
                  sizeof(WIFI_PORTAL_PATTERN) / sizeof(WIFI_PORTAL_PATTERN[0]),
                  "Wi-Fi setup AP active: 2 cyan flashes");
  });

  wifiManager.setConfigPortalTimeoutCallback([]() {
    wifiPortalActive = false;
    Serial.println("[WiFi] Setup portal timed out. Restart and retry; hold BOOT at power-on to erase saved Wi-Fi credentials.");
    setLedPattern(WIFI_FAILED_PATTERN,
                  sizeof(WIFI_FAILED_PATTERN) / sizeof(WIFI_FAILED_PATTERN[0]),
                  "Wi-Fi setup failed/timed out: 3 red flashes");
  });

  const bool forcePortal = prefs.getBool("wifiPortal", false);
  prefs.putBool("wifiPortal", false);
  Serial.printf("[WiFi] Saved Wi-Fi credentials: %s\n",
                wifiManager.getWiFiIsSaved() ? "yes" : "no");
  setLedPattern(WIFI_CONNECTING_PATTERN,
                sizeof(WIFI_CONNECTING_PATTERN) / sizeof(WIFI_CONNECTING_PATTERN[0]),
                "Trying saved Wi-Fi credentials: slow yellow pulse");

  bool connected = false;
  if (forcePortal) {
    Serial.println("[WiFi] User requested Wi-Fi reconfiguration; starting setup AP.");
    wifiManager.startConfigPortal("TeamsPresence-Setup");
    wifiPortalActive = wifiManager.getConfigPortalActive();
  } else {
    connected = wifiManager.autoConnect("TeamsPresence-Setup");
    wifiPortalActive = wifiManager.getConfigPortalActive();
  }

  if (connected && WiFi.isConnected()) {
    startApplicationServices();
  } else if (wifiPortalActive) {
    Serial.println("[WiFi] Setup portal is running in non-blocking mode.");
    Serial.println("[WiFi] Network name: TeamsPresence-Setup (open network, no password)");
    Serial.println("[WiFi] Portal address: http://192.168.4.1/");
  } else {
    wifiReady = false;
    Serial.println("[WiFi] No connection and no active setup portal. Check serial log above.");
    setLedPattern(WIFI_FAILED_PATTERN,
                  sizeof(WIFI_FAILED_PATTERN) / sizeof(WIFI_FAILED_PATTERN[0]),
                  "Wi-Fi unavailable: 3 red flashes");
  }
}

void loop() {
  if (wifiPortalActive) {
    // WiFiManager is non-blocking so the LED can show a repeating cyan portal code.
    if (wifiManager.process()) {
      Serial.println("[WiFi] Credentials accepted; starting application services.");
      startApplicationServices();
    }
    updateLedPattern();
    delay(5);
    return;
  }

  if (!wifiReady) {
    updateLedPattern();
    delay(10);
    return;
  }

  server.handleClient();
  pollDeviceCode();

  if (graphReady && millis() - lastPresencePoll >= pollIntervalMs) {
    pollPresence();
  }

  updateLedPattern();
  delay(5);
}
