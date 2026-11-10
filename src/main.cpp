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

#if !defined(MS_TENANT_ID) || !defined(MS_CLIENT_ID)
#error "Copy include/config.h.example to include/config.h and configure Tenant ID and Client ID"
#endif

CRGB leds[LED_COUNT];
WebServer server(WEB_PORT);
Preferences prefs;

String accessToken;
String refreshToken;
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

String tokenUrl() {
  return String("https://login.microsoftonline.com/") +
         MS_TENANT_ID + "/oauth2/v2.0/token";
}

String deviceCodeUrl() {
  return String("https://login.microsoftonline.com/") +
         MS_TENANT_ID + "/oauth2/v2.0/devicecode";
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

void setSolid(const CRGB& color) {
  fill_solid(leds, LED_COUNT, color);
  FastLED.show();
}

void setErrorBlink() {
  static bool on = false;
  on = !on;
  setSolid(on ? CRGB::Blue : CRGB::Black);
}

String htmlEscape(String value) {
  value.replace("&", "&amp;");
  value.replace("<", "&lt;");
  value.replace(">", "&gt;");
  value.replace("\"", "&quot;");
  return value;
}

String pageHtml() {
  String html = F(
    "<!doctype html><html lang='it'><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Teams Presence</title><style>"
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

  if (deviceLoginPending) {
    html += "<hr><h2>Accedi a Microsoft</h2><p>Apri il link e inserisci il codice:</p>";
    html += "<p><a target='_blank' href='" + htmlEscape(verificationUri) + "'>Apri Microsoft Device Login</a></p>";
    html += "<p class='code'>" + htmlEscape(userCode) + "</p>";
    html += "<p class='muted'>Lascia acceso questo dispositivo durante l'autorizzazione.</p>";
  } else if (!graphReady) {
    html += "<hr><p>Microsoft non è ancora collegato.</p>";
    html += "<form method='post' action='/login'><button>Collega Microsoft</button></form>";
  }
else {
    html += "<hr><p>Account Microsoft collegato.</p>";
    html += "<form method='post' action='/logout'><button>Disconnetti account</button></form>";
  }

  html += F("<hr><p class='muted'>Wi-Fi: ");
  html += WiFi.isConnected() ? "connesso" : "disconnesso";
  html += F("</p><p class='muted'>Aggiornamento presenza: circa ogni 60 secondi.</p>"
            "</main></body></html>");
  return html;
}

void handleRoot() {
  server.send(200, "text/html; charset=utf-8", pageHtml());
}

void handleLogin() {
  if (!WiFi.isConnected()) {
    server.send(503, "text/plain", "Wi-Fi non connesso");
    return;
  }

  String body = "client_id=" + encodeForm(MS_CLIENT_ID) +
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

void pollDeviceCode() {
  if (!deviceLoginPending || (int32_t)(millis() - nextDevicePoll) < 0) return;

  if ((uint32_t)(millis() - deviceCodeStartedAt) >= deviceCodeExpires * 1000UL) {
    deviceLoginPending = false;
    oauthError = "Codice scaduto. Avvia nuovamente il login.";
    setSolid(CRGB::Blue);
    return;
  }

  String body =
    "client_id=" + encodeForm(MS_CLIENT_ID) +
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
  if (!refreshToken.length()) return false;

  String body =
    "client_id=" + encodeForm(MS_CLIENT_ID) +
    "&grant_type=refresh_token" +
    "&refresh_token=" + encodeForm(refreshToken) +
    "&scope=" + encodeForm("https://graph.microsoft.com/Presence.Read offline_access");

  String response;
  int status = 0;
  if (!httpsRequest(tokenUrl(), "POST", body, "", response, status)) return false;
  return parseTokenResponse(response, status);
}

void applyPresenceColor(const String& availability) {
  if (availability == "Available") setSolid(CRGB::Green);
  else if (availability == "Busy") setSolid(CRGB::Red);
  else if (availability == "DoNotDisturb") setSolid(CRGB::Purple);
  else if (availability == "Away") setSolid(CRGB::Yellow);
  else if (availability == "BeRightBack") setSolid(CRGB::Orange);
  else if (availability == "Offline") setSolid(CRGB::Black);
  else setErrorBlink();
}

void pollPresence() {
  if (!graphReady || !accessToken.length()) return;

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

  FastLED.addLeds<WS2812B, LED_PIN, GRB>(leds, LED_COUNT);
  FastLED.setBrightness(LED_BRIGHTNESS);
  setSolid(CRGB::Blue);

  prefs.begin("teams-presence", false);
  refreshToken = prefs.getString("refresh", "");

  WiFi.mode(WIFI_STA);
  WiFiManager wm;
  wm.setConfigPortalTimeout(180);
  wm.setConnectTimeout(30);
  bool connected = wm.autoConnect("TeamsPresence-Setup");

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
  server.on("/logout", HTTP_POST, handleLogout);
  server.begin();

  Serial.print("Pagina configurazione: http://");
  Serial.println(WiFi.localIP());

  if (refreshToken.length()) {
    if (refreshAccessToken()) {
      authUser = "Account autenticato (endpoint /me)";
      graphReady = true;
      setSolid(CRGB::Green);
    } else {
      graphReady = false;
      Serial.println("Refresh token non valido: accedere dalla pagina web.");
    }
  }
}

void loop() {
  if (!wifiReady) {
    delay(1000);
    return;
  }

  server.handleClient();
  pollDeviceCode();

  if (graphReady && millis() - lastPresencePoll >= PRESENCE_POLL_MS) {
    pollPresence();
  }

  if (!graphReady && !deviceLoginPending && !oauthError.length()) {
    // Await user action in the local web page.
  }

  delay(5);
}
