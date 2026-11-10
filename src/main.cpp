#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <FastLED.h>
#include <WiFiManager.h>
#include <time.h>
#include <esp32_cert_bundle.h>

#include "config.h"

// Optional compile-time defaults for developers with a private local config.h.
// Production firmware ships with empty defaults and is configured from the web UI.
#ifndef MS_TENANT_ID
#define MS_TENANT_ID ""
#endif
#ifndef MS_CLIENT_ID
#define MS_CLIENT_ID ""
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
uint16_t activeLeds = LED_COUNT;
uint32_t pollIntervalMs = PRESENCE_POLL_MS;
uint8_t brightnessPct = (LED_BRIGHTNESS * 100 + 127) / 255;
String manualState;  // empty = automatic (follows Teams presence)

void pollPresence();
WebServer server(WEB_PORT);
Preferences prefs;

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
bool wifiReady = false;

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
  fill_solid(leds, LED_MAX_COUNT, CRGB::Black);
  fill_solid(leds, activeLeds, c);
  FastLED.show();
}

// A manual state locks the LEDs: automatic updates are ignored until it is cleared.
void setSolid(const CRGB& c) {
  if (manualState.length()) return;
  showColor(c);
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
  if (manualState.length() && stateColor(manualState, c)) showColor(c);
}

void setErrorBlink() {
  static bool on = false;
  on = !on;
  setSolid(on ? CRGB::Blue : CRGB::Black);
}

String htmlEscape(String s) {
  s.replace("&", "&amp;");
  s.replace("<", "&lt;");
  s.replace(">", "&gt;");
  s.replace("\"", "&quot;");
  return s;
}

String settingsFormHtml() {
  static const char* const states[][2] = {
    {"", "Automatico (segue Teams)"}, {"Available", "Disponibile (verde)"},
    {"Busy", "Occupato (rosso)"}, {"DoNotDisturb", "Non disturbare (viola)"},
    {"Away", "Assente (giallo)"}, {"BeRightBack", "Torno subito (arancione)"},
    {"Offline", "Offline (spento)"}, {"Error", "Errore (blu)"}};

  String html = F(
    "<hr><h2>Microsoft Entra ID</h2>"
    "<p class='muted'>Inserisci gli ID dell'App Registration della tua organizzazione. "
    "Sono identificativi pubblici, non password o segreti.</p>"
    "<form method='post' action='/entra'>"
    "<label>Directory (tenant) ID</label>"
    "<input type='text' name='tenant' minlength='36' maxlength='36' "
    "autocomplete='off' autocapitalize='none' spellcheck='false' required value='"
  );
  html += htmlEscape(entraTenantId);
  html += F("'>"
            "<label>Application (client) ID</label>"
            "<input type='text' name='client' minlength='36' maxlength='36' "
            "autocomplete='off' autocapitalize='none' spellcheck='false' required value='");
  html += htmlEscape(entraClientId);
  html += F("'>"
            "<button>Salva configurazione Microsoft</button></form>"
            "<p class='muted'>Dopo il salvataggio potrai collegare l'account Microsoft.</p>");
  html += F("<h2>Impostazioni dispositivo</h2><form method='post' action='/settings'>");
  html += "<label>Numero di LED (1-" + String(LED_MAX_COUNT) + ")</label>";
  html += "<input type='number' name='leds' min='1' max='" + String(LED_MAX_COUNT) +
          "' value='" + String(activeLeds) + "'>";
  html += "<label>Luminosità: <span id='bv'>" + String(brightnessPct) + "</span>%</label>";
  html += "<input type='range' name='brightness' min='1' max='100' value='" +
          String(brightnessPct) + "' oninput=\"document.getElementById('bv').textContent=this.value\">";
  html += "<label>Intervallo di aggiornamento (secondi, " + String(PRESENCE_POLL_MIN_S) +
          "-" + String(PRESENCE_POLL_MAX_S) + ")</label>";
  html += "<input type='number' name='refresh' min='" + String(PRESENCE_POLL_MIN_S) +
          "' max='" + String(PRESENCE_POLL_MAX_S) + "' value='" +
          String(pollIntervalMs / 1000UL) + "'>";
  html += "<label>Stato LED</label><select name='state'>";
  for (auto& s : states) {
    html += "<option value='" + String(s[0]) + "'";
    if (manualState == s[0]) html += " selected";
    html += ">" + String(s[1]) + "</option>";
  }
  html += F("</select><p class='muted'>Con uno stato manuale i LED ignorano la presenza "
            "di Teams finché non torni su Automatico.</p><button>Salva</button></form>");
  return html;
}

String pageHtml() {
  String html = F(
    "<!doctype html><html lang='it'><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>");
  // Auto-refresh only while waiting for login, so form edits are not discarded.
  if (deviceLoginPending) html += F("<meta http-equiv='refresh' content='5'>");
  html += F(
    "<title>Teams Presence</title><style>"
    "label{display:block;margin:16px 0 6px}input,select{width:100%;padding:10px;"
    "border-radius:8px;box-sizing:border-box}"
    "body{font-family:system-ui;max-width:620px;margin:35px auto;padding:0 18px;"
    "background:#101722;color:#eef3fa}main{background:#1b2736;padding:24px;"
    "border-radius:16px}a,button{color:white;background:#2563eb;padding:12px 16px;"
    "border:0;border-radius:9px;text-decoration:none;display:inline-block}"
    ".muted{color:#a9b8ca}.code{font-size:2rem;letter-spacing:3px;font-weight:700}"
    "</style></head><body><main><h1>Teams Presence</h1>"
  );
  html += "<p class='muted'>Dispositivo ESP32-WROOM-32D</p>";
  html += "<h2>Stato: " + htmlEscape(presenceText) + "</h2>";
  html += "<p>Attività: " + htmlEscape(activityText) + "</p>";
  html += "<p>Account: " + htmlEscape(authUser) + "</p>";
  if (oauthError.length()) {
    html += "<p style='color:#fca5a5'>Errore: " + htmlEscape(oauthError) + "</p>";
  }

  if (!entraConfigured()) {
    html += "<hr><h2>Configura Microsoft</h2><p>Inserisci Tenant ID e Client ID nella sezione qui sotto, quindi salva la configurazione.</p>";
  } else if (deviceLoginPending) {
    html += "<hr><h2>Accedi a Microsoft</h2><p>Apri il link e inserisci il codice:</p>";
    html += "<p><a target='_blank' href='" + htmlEscape(verificationUri) + "'>Apri Microsoft Device Login</a></p>";
    html += "<p class='code'>" + htmlEscape(userCode) + "</p>";
    html += "<p class='muted'>Lascia acceso questo dispositivo durante l'autorizzazione.</p>";
  } else if (!graphReady) {
    html += "<hr><p>Microsoft non è ancora collegato.</p>";
    html += "<form method='post' action='/login'><button>Collega Microsoft</button></form>";
  } else {
    html += "<hr><p>Account Microsoft collegato.</p>";
    html += "<form method='post' action='/logout'><button>Disconnetti account</button></form>";
  }

  html += F("<hr><p class='muted'>Wi-Fi: ");
  html += WiFi.isConnected() ? "connesso" : "disconnesso";
  html += "</p><p class='muted'>Stato LED: " +
          (manualState.length() ? "manuale (" + manualState + ")" : String("automatico")) +
          " &middot; aggiornamento ogni " + String(pollIntervalMs / 1000UL) + " s</p>";
  html += settingsFormHtml();
  html += F("<hr><h2>Rete Wi-Fi</h2><p class='muted'>Rete attuale: ");
  html += htmlEscape(WiFi.SSID());
  html += F("</p><form method='post' action='/wifi' "
            "onsubmit=\"return confirm('Il dispositivo si riavvia e apre il portale Wi-Fi. Continuare?')\">"
            "<button>Cambia rete Wi-Fi</button></form>");
  html += F("</main></body></html>");
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
  setSolid(CRGB::Purple);
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
    deviceLoginPending = false;
    deviceCode = "";
    userCode = "";
    authUser = "Non autenticato";
    presenceText = "Configurazione Microsoft salvata";
    activityText = "-";
    setSolid(CRGB::Black);
  }
  oauthError = "";
  handleSettingsRedirect();
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
  }
  if (server.hasArg("refresh")) {
    uint32_t sec = constrain(server.arg("refresh").toInt(),
                             PRESENCE_POLL_MIN_S, PRESENCE_POLL_MAX_S);
    pollIntervalMs = sec * 1000UL;
    prefs.putUInt("pollSec", sec);
  }
  if (server.hasArg("state")) {
    String s = server.arg("state");
    CRGB c;
    manualState = stateColor(s, c) ? s : "";
    prefs.putString("manual", manualState);
  }

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
    setSolid(CRGB::Blue);
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
    setSolid(CRGB::Blue);
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
      presenceText = "Sessione scaduta: riconnettere Microsoft";
      setSolid(CRGB::Blue);
      return;
    }
  }

  String response;
  int status = 0;
  if (!httpsRequest(graphPresenceUrl, "GET", "", accessToken, response, status)) {
    presenceText = "Errore connessione Graph";
    setErrorBlink();
    return;
  }

  if (status == 401) {
    if (!refreshAccessToken()) {
      graphReady = false;
      presenceText = "Autenticazione richiesta";
      setSolid(CRGB::Blue);
      return;
    }
    if (!httpsRequest(graphPresenceUrl, "GET", "", accessToken, response, status)) return;
  }

  if (status != 200) {
    presenceText = "Graph HTTP " + String(status);
    setErrorBlink();
    return;
  }

  JsonDocument doc;
  if (deserializeJson(doc, response)) {
    presenceText = "JSON Graph non valido";
    setErrorBlink();
    return;
  }

  String availability = doc["availability"] | "Unknown";
  activityText = doc["activity"] | "Unknown";
  presenceText = availability;
  applyPresenceColor(availability);
  lastPresencePoll = millis();
}

void setup() {
  Serial.begin(115200);
  delay(300);

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

  FastLED.addLeds<WS2812B, LED_PIN, GRB>(leds, LED_MAX_COUNT);
  brightnessPct = constrain(prefs.getUChar("bright", brightnessPct), 1, 100);
  FastLED.setBrightness((brightnessPct * 255 + 50) / 100);
  if (manualState.length()) applyManualState();
  else setSolid(CRGB::Blue);

  WiFi.mode(WIFI_STA);
  WiFiManager wm;
  wm.setConfigPortalTimeout(180);
  wm.setConnectTimeout(30);
  bool connected;
  if (prefs.getBool("wifiPortal", false)) {
    prefs.putBool("wifiPortal", false);
    // Old credentials are kept until a new network is saved; on timeout, reconnect to them.
    connected = wm.startConfigPortal("TeamsPresence-Setup");
    if (!connected) connected = wm.autoConnect("TeamsPresence-Setup");
  } else {
    connected = wm.autoConnect("TeamsPresence-Setup");
  }

  if (!connected) {
    Serial.println("Wi-Fi non configurato o timeout. Riavvia per riprovare.");
    setSolid(CRGB::Red);
    return;
  }

  wifiReady = true;
  Serial.print("Wi-Fi connesso, IP: ");
  Serial.println(WiFi.localIP());

  configTime(0, 0, "pool.ntp.org", "time.google.com");
  time_t now = 0;
  for (int i = 0; i < 30; i++) {
    time(&now);
    if (now > 1700000000) break;
    delay(500);
  }

  server.on("/", HTTP_GET, handleRoot);
  server.on("/login", HTTP_POST, handleLogin);
  server.on("/entra", HTTP_POST, handleEntraSave);
  server.on("/logout", HTTP_POST, handleLogout);
  server.on("/wifi", HTTP_POST, handleWifiChange);
  server.on("/settings", HTTP_GET, handleSettingsRedirect);
  server.on("/settings", HTTP_POST, handleSettingsSave);
  server.begin();

  Serial.print("Pagina configurazione: http://");
  Serial.println(WiFi.localIP());

  if (refreshToken.length() && entraConfigured()) {
    if (refreshAccessToken()) {
      authUser = "Account autenticato (endpoint /me)";
      graphReady = true;
      setSolid(CRGB::Green);
    } else {
      graphReady = false;
      Serial.println("Refresh token non valido: accedere dalla pagina web.");
    }
  } else if (refreshToken.length() && !entraConfigured()) {
    // A refresh token without its matching tenant/client configuration cannot be used safely.
    prefs.remove("refresh");
    refreshToken = "";
  }
}

void loop() {
  if (!wifiReady) {
    delay(1000);
    return;
  }

  server.handleClient();
  pollDeviceCode();

  if (graphReady && millis() - lastPresencePoll >= pollIntervalMs) {
    pollPresence();
  }

  if (!graphReady && !deviceLoginPending && !oauthError.length()) {
    // Await user action in the local web page.
  }

  delay(5);
}
