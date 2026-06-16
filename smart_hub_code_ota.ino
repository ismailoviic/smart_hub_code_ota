#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Update.h>

#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_arduino_version.h>

// =====================================================
// BOOTSTRAP CONFIG
// CHANGE ONLY FOR FIRST MANUAL USB UPLOAD
// =====================================================

#define BOOTSTRAP_HUB_ID      "HUB-L1"
#define BOOTSTRAP_FACTORY_ID  "FACTORY-001"
#define BOOTSTRAP_LINE_ID     "LINE-001"

// For HUB-L2, use:
// #define BOOTSTRAP_HUB_ID      "HUB-L2"
// #define BOOTSTRAP_FACTORY_ID  "FACTORY-001"
// #define BOOTSTRAP_LINE_ID     "LINE-002"

// true  = first manual upload to force-write Hub identity into NVS
// false = OTA universal firmware, identity will NOT change
#define BOOTSTRAP_FORCE_OVERWRITE false

// =====================================================
// OTA URL
// =====================================================

#define HUB_BIN_URL "https://raw.githubusercontent.com/ismailoviic/smart_hub_code_ota/main/build/esp32.esp32.esp32/smart_hub_code_ota.ino.bin"

// =====================================================
// PINS
// =====================================================

#define LED_RED_PIN     14
#define LED_GREEN_PIN   27
#define LED_BLUE_PIN    26

// =====================================================
// SETTINGS
// IMPORTANT: Buttons and Hub must use same ESP-NOW channel.
// If Hub also connects to Wi-Fi, your router should be on this same channel.
// =====================================================

#define AP_PASSWORD "12345678"
#define ESPNOW_CHANNEL 8

IPAddress apIP(192, 168, 4, 1);
IPAddress netMsk(255, 255, 255, 0);

Preferences prefs;
WebServer server(80);
DNSServer dnsServer;

const byte DNS_PORT = 53;

// =====================================================
// HUB CONFIG STORED IN NVS
// =====================================================

String hubId;
String factoryId;
String lineId;
String wifiSSID;
String wifiPassword;
String apiUrl;
String apiToken;

// =====================================================
// STATUS / DIAGNOSTICS
// =====================================================

unsigned long lastPacketMs = 0;
unsigned long lastBackendMs = 0;

uint32_t totalPacketsReceived = 0;
uint32_t totalEventsQueued = 0;
uint32_t totalEventsPosted = 0;
uint32_t totalEventsFailed = 0;
uint32_t totalQueueDrops = 0;

int lastHttpCode = 0;
String lastBackendStatus = "No backend request yet";
String lastBackendResponse = "";
String lastReceivedSummary = "No ESP-NOW packet yet";
String lastWifiStatus = "Not connected";

// =====================================================
// SIMPLE OLD ESP-NOW PROTOCOL SUPPORT
// =====================================================

#define SIMPLE_MSG_HELLO        1
#define SIMPLE_MSG_ACK          2
#define SIMPLE_MSG_BUTTON_PRESS 3
#define SIMPLE_MSG_RFID_SCAN    4

typedef struct __attribute__((packed)) {
  uint8_t type;
  char from[16];
  char to[16];
  uint32_t counter;
  char text[32];
} SimplePacket;

// =====================================================
// NEW BUTTON PROTOCOL SUPPORT
// =====================================================

#define PROTOCOL_VERSION 1

enum MessageType {
  MSG_HELLO = 1,
  MSG_ACK = 2,
  MSG_BADGE_SCAN = 3,
  MSG_PIECE_DONE = 4,
  MSG_BATTERY_STATUS = 5,
  MSG_HEARTBEAT = 6,
  MSG_ERROR = 7,
  MSG_CONFIG_UPDATE = 8,
  MSG_OTA_AVAILABLE = 9,
  MSG_OTA_START = 10,
  MSG_OTA_STATUS = 11
};

typedef struct __attribute__((packed)) {
  uint8_t protocolVersion;
  uint8_t messageType;
  uint8_t batteryPercent;
  uint8_t reserved1;

  uint32_t bootId;
  uint32_t sequence;
  uint32_t uptimeSeconds;

  uint16_t batteryMv;
  uint16_t eventValue;

  char deviceId[16];
  char targetHubId[16];
  char badgeUid[24];
  char firmwareVersion[16];
} ButtonToHubPacket;

typedef struct __attribute__((packed)) {
  uint8_t protocolVersion;
  uint8_t messageType;
  uint8_t ackForMessageType;
  uint8_t statusCode;

  uint32_t ackSequence;
  uint32_t hubUptimeSeconds;

  char hubId[16];
  char message[32];
  char targetVersion[16];
} HubToButtonAck;

// =====================================================
// BACKEND EVENT QUEUE
// Do not do HTTP inside ESP-NOW callback.
// Queue event, then POST from loop().
// =====================================================

struct BackendEvent {
  char eventType[24];
  char buttonId[16];
  char buttonMac[18];
  char badgeUid[24];
  char firmwareVersion[16];

  uint32_t bootId;
  uint32_t sequence;
  uint32_t uptimeSeconds;

  uint16_t batteryMv;
  uint8_t batteryPercent;
  uint16_t eventValue;
};

#define EVENT_QUEUE_SIZE 8

BackendEvent eventQueue[EVENT_QUEUE_SIZE];
volatile int queueHead = 0;
volatile int queueTail = 0;

// =====================================================
// LED
// =====================================================

void setLED(bool r, bool g, bool b) {
  digitalWrite(LED_RED_PIN, r ? HIGH : LOW);
  digitalWrite(LED_GREEN_PIN, g ? HIGH : LOW);
  digitalWrite(LED_BLUE_PIN, b ? HIGH : LOW);
}

void ledBlue()   { setLED(false, false, true); }
void ledGreen()  { setLED(false, true, false); }
void ledYellow() { setLED(true, true, false); }
void ledPurple() { setLED(true, false, true); }
void ledRed()    { setLED(true, false, false); }

void blinkPurpleDuringOTA() {
  ledPurple();
  delay(150);
  setLED(false, false, false);
  delay(150);
}

// =====================================================
// HELPERS
// =====================================================

void safeCopy(char *dest, const char *src, size_t maxLen) {
  strncpy(dest, src, maxLen);
  dest[maxLen - 1] = '\0';
}

String macToString(const uint8_t *mac) {
  char buffer[18];
  snprintf(
    buffer,
    sizeof(buffer),
    "%02X:%02X:%02X:%02X:%02X:%02X",
    mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]
  );
  return String(buffer);
}

String messageTypeToText(uint8_t type) {
  switch (type) {
    case MSG_HELLO: return "HELLO";
    case MSG_ACK: return "ACK";
    case MSG_BADGE_SCAN: return "BADGE_SCAN";
    case MSG_PIECE_DONE: return "PIECE_DONE";
    case MSG_BATTERY_STATUS: return "BATTERY_STATUS";
    case MSG_HEARTBEAT: return "HEARTBEAT";
    case MSG_ERROR: return "ERROR";
    case MSG_CONFIG_UPDATE: return "CONFIG_UPDATE";
    case MSG_OTA_AVAILABLE: return "OTA_AVAILABLE";
    case MSG_OTA_START: return "OTA_START";
    case MSG_OTA_STATUS: return "OTA_STATUS";
    default: return "UNKNOWN";
  }
}

String simpleTypeToText(uint8_t type) {
  switch (type) {
    case SIMPLE_MSG_HELLO: return "HELLO";
    case SIMPLE_MSG_ACK: return "ACK";
    case SIMPLE_MSG_BUTTON_PRESS: return "PIECE_DONE";
    case SIMPLE_MSG_RFID_SCAN: return "BADGE_SCAN";
    default: return "UNKNOWN";
  }
}

String jsonEscape(String s) {
  s.replace("\\", "\\\\");
  s.replace("\"", "\\\"");
  s.replace("\n", "\\n");
  s.replace("\r", "\\r");
  return s;
}

String htmlEscape(String value) {
  value.replace("&", "&amp;");
  value.replace("<", "&lt;");
  value.replace(">", "&gt;");
  value.replace("\"", "&quot;");
  value.replace("'", "&#39;");
  return value;
}

// =====================================================
// EVENT QUEUE
// =====================================================

bool enqueueBackendEvent(const BackendEvent &ev) {
  int nextHead = (queueHead + 1) % EVENT_QUEUE_SIZE;

  if (nextHead == queueTail) {
    totalQueueDrops++;
    lastBackendStatus = "Queue full, event dropped";
    return false;
  }

  eventQueue[queueHead] = ev;
  queueHead = nextHead;
  totalEventsQueued++;
  return true;
}

bool dequeueBackendEvent(BackendEvent &ev) {
  if (queueTail == queueHead) {
    return false;
  }

  ev = eventQueue[queueTail];
  queueTail = (queueTail + 1) % EVENT_QUEUE_SIZE;
  return true;
}

// =====================================================
// NVS CONFIG
// =====================================================

void saveBootstrapIdentityIfNeeded() {
  prefs.begin("hub_cfg", false);

  bool initialized = prefs.getBool("initialized", false);

  if (!initialized || BOOTSTRAP_FORCE_OVERWRITE) {
    Serial.println("[NVS] Writing bootstrap Hub identity.");

    prefs.putString("hubid", BOOTSTRAP_HUB_ID);
    prefs.putString("factory", BOOTSTRAP_FACTORY_ID);
    prefs.putString("line", BOOTSTRAP_LINE_ID);

    if (!initialized) {
      prefs.putString("ssid", "");
      prefs.putString("pass", "");
      prefs.putString("apiurl", "");
      prefs.putString("apitoken", "");
    }

    prefs.putBool("initialized", true);
  }

  prefs.end();
}

void loadConfig() {
  saveBootstrapIdentityIfNeeded();

  prefs.begin("hub_cfg", true);

  hubId = prefs.getString("hubid", BOOTSTRAP_HUB_ID);
  factoryId = prefs.getString("factory", BOOTSTRAP_FACTORY_ID);
  lineId = prefs.getString("line", BOOTSTRAP_LINE_ID);
  wifiSSID = prefs.getString("ssid", "");
  wifiPassword = prefs.getString("pass", "");
  apiUrl = prefs.getString("apiurl", "");
  apiToken = prefs.getString("apitoken", "");

  prefs.end();

  Serial.println();
  Serial.println("========== LOADED HUB CONFIG ==========");
  Serial.print("Hub ID: "); Serial.println(hubId);
  Serial.print("Factory ID: "); Serial.println(factoryId);
  Serial.print("Line ID: "); Serial.println(lineId);
  Serial.print("Wi-Fi SSID: "); Serial.println(wifiSSID);
  Serial.print("API URL: "); Serial.println(apiUrl);
  Serial.println("=======================================");
}

void saveConfig(
  String newHubId,
  String newFactoryId,
  String newLineId,
  String ssid,
  String pass,
  String newApiUrl,
  String newApiToken
) {
  prefs.begin("hub_cfg", false);

  prefs.putString("hubid", newHubId);
  prefs.putString("factory", newFactoryId);
  prefs.putString("line", newLineId);
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.putString("apiurl", newApiUrl);
  prefs.putString("apitoken", newApiToken);
  prefs.putBool("initialized", true);

  prefs.end();

  Serial.println("[NVS] Hub config saved.");

  loadConfig();
}

// =====================================================
// WIFI FOR BACKEND
// =====================================================

bool ensureWiFiConnected(unsigned long timeoutMs = 12000) {
  if (wifiSSID.length() == 0) {
    lastWifiStatus = "No Wi-Fi SSID saved";
    return false;
  }

  if (WiFi.status() == WL_CONNECTED) {
    lastWifiStatus = "Connected: " + WiFi.localIP().toString();
    return true;
  }

  Serial.print("[WIFI] Connecting to: ");
  Serial.println(wifiSSID);

  WiFi.mode(WIFI_AP_STA);
  WiFi.begin(wifiSSID.c_str(), wifiPassword.c_str());

  unsigned long start = millis();

  while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) {
    dnsServer.processNextRequest();
    server.handleClient();
    delay(50);
  }

  if (WiFi.status() == WL_CONNECTED) {
    lastWifiStatus = "Connected: " + WiFi.localIP().toString();
    Serial.print("[WIFI] Connected. IP: ");
    Serial.println(WiFi.localIP());
    return true;
  }

  lastWifiStatus = "Wi-Fi failed";
  Serial.println("[WIFI] Connection failed.");
  return false;
}

// =====================================================
// BACKEND / SUPABASE POST
// =====================================================

String buildBackendJson(const BackendEvent &ev) {
  String json = "{";

  json += "\"source\":\"esp32_hub\",";
  json += "\"factory_id\":\"" + jsonEscape(factoryId) + "\",";
  json += "\"line_id\":\"" + jsonEscape(lineId) + "\",";
  json += "\"hub_id\":\"" + jsonEscape(hubId) + "\",";

  json += "\"button_id\":\"" + jsonEscape(String(ev.buttonId)) + "\",";
  json += "\"button_mac\":\"" + jsonEscape(String(ev.buttonMac)) + "\",";
  json += "\"event_type\":\"" + jsonEscape(String(ev.eventType)) + "\",";

  json += "\"badge_uid\":\"" + jsonEscape(String(ev.badgeUid)) + "\",";
  json += "\"firmware_version\":\"" + jsonEscape(String(ev.firmwareVersion)) + "\",";

  json += "\"boot_id\":" + String(ev.bootId) + ",";
  json += "\"sequence\":" + String(ev.sequence) + ",";
  json += "\"uptime_seconds\":" + String(ev.uptimeSeconds) + ",";

  json += "\"battery_mv\":" + String(ev.batteryMv) + ",";
  json += "\"battery_percent\":" + String(ev.batteryPercent) + ",";
  json += "\"event_value\":" + String(ev.eventValue);

  json += "}";

  return json;
}

bool sendEventToBackend(const BackendEvent &ev) {
  if (apiUrl.length() == 0) {
    lastBackendStatus = "API URL missing";
    totalEventsFailed++;
    return false;
  }

  if (!ensureWiFiConnected()) {
    lastBackendStatus = "Wi-Fi not connected";
    totalEventsFailed++;
    return false;
  }

  String json = buildBackendJson(ev);

  Serial.println();
  Serial.println("========== BACKEND POST ==========");
  Serial.print("URL: ");
  Serial.println(apiUrl);
  Serial.println(json);
  Serial.println("==================================");

  HTTPClient http;
  WiFiClient normalClient;
  WiFiClientSecure secureClient;

  bool isHttps = apiUrl.startsWith("https://");

  bool beginOk = false;

  if (isHttps) {
    secureClient.setInsecure();
    beginOk = http.begin(secureClient, apiUrl);
  } else {
    beginOk = http.begin(normalClient, apiUrl);
  }

  if (!beginOk) {
    lastBackendStatus = "HTTP begin failed";
    totalEventsFailed++;
    return false;
  }

  http.setTimeout(15000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.addHeader("Content-Type", "application/json");

  if (apiToken.length() > 0) {
    http.addHeader("x-device-token", apiToken);
    http.addHeader("Authorization", "Bearer " + apiToken);
  }

  int code = http.POST(json);
  String response = http.getString();

  http.end();

  lastHttpCode = code;
  lastBackendResponse = response;
  if (lastBackendResponse.length() > 500) {
    lastBackendResponse = lastBackendResponse.substring(0, 500);
  }

  lastBackendMs = millis();

  Serial.print("[BACKEND] HTTP code: ");
  Serial.println(code);
  Serial.print("[BACKEND] Response: ");
  Serial.println(response);

  if (code >= 200 && code < 300) {
    lastBackendStatus = "POST OK";
    totalEventsPosted++;
    ledGreen();
    delay(80);
    ledBlue();
    return true;
  }

  lastBackendStatus = "POST failed";
  totalEventsFailed++;
  ledYellow();
  delay(120);
  ledBlue();
  return false;
}

void processBackendQueue() {
  BackendEvent ev;

  if (!dequeueBackendEvent(ev)) {
    return;
  }

  sendEventToBackend(ev);
}

// =====================================================
// OTA
// =====================================================

bool connectWiFiForOTA() {
  if (wifiSSID.length() == 0) {
    Serial.println("[OTA] No Wi-Fi SSID saved.");
    return false;
  }

  Serial.println("[OTA] Connecting to Wi-Fi...");
  Serial.print("[OTA] SSID: ");
  Serial.println(wifiSSID);

  esp_now_deinit();

  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiSSID.c_str(), wifiPassword.c_str());

  unsigned long start = millis();

  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    blinkPurpleDuringOTA();
    Serial.print(".");
  }

  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("[OTA] Wi-Fi connected.");
    Serial.print("[OTA] IP: ");
    Serial.println(WiFi.localIP());
    return true;
  }

  Serial.println("[OTA] Wi-Fi failed.");
  return false;
}

bool performOTA() {
  Serial.println();
  Serial.println("========== HUB OTA START ==========");
  Serial.print("[OTA] URL: ");
  Serial.println(HUB_BIN_URL);

  HTTPClient http;
  WiFiClientSecure client;
  client.setInsecure();

  if (!http.begin(client, HUB_BIN_URL)) {
    Serial.println("[OTA] HTTP begin failed.");
    return false;
  }

  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(30000);
  http.addHeader("Cache-Control", "no-cache");

  int httpCode = http.GET();

  Serial.print("[OTA] HTTP code: ");
  Serial.println(httpCode);

  if (httpCode != HTTP_CODE_OK) {
    Serial.println("[OTA] Download failed.");
    http.end();
    return false;
  }

  int contentLength = http.getSize();

  Serial.print("[OTA] Content length: ");
  Serial.println(contentLength);

  if (!Update.begin(contentLength > 0 ? contentLength : UPDATE_SIZE_UNKNOWN)) {
    Serial.println("[OTA] Update.begin failed.");
    Update.printError(Serial);
    http.end();
    return false;
  }

  WiFiClient *stream = http.getStreamPtr();

  size_t written = Update.writeStream(*stream);

  Serial.print("[OTA] Written bytes: ");
  Serial.println(written);

  if (!Update.end()) {
    Serial.println("[OTA] Update.end failed.");
    Update.printError(Serial);
    http.end();
    return false;
  }

  if (!Update.isFinished()) {
    Serial.println("[OTA] Update not finished.");
    http.end();
    return false;
  }

  http.end();

  Serial.println("[OTA] SUCCESS. Restarting.");
  ledPurple();
  delay(1000);
  ESP.restart();

  return true;
}

void startOTAFromPortal() {
  Serial.println("[PORTAL] Hub OTA requested.");

  server.send(200, "text/html", "<h2>Hub OTA started</h2><p>Hub is updating from GitHub. Reconnect to the Hub Wi-Fi after 1 minute.</p>");

  delay(1000);

  if (!connectWiFiForOTA()) {
    Serial.println("[OTA] Wi-Fi failed. Restarting.");
    delay(3000);
    ESP.restart();
  }

  if (!performOTA()) {
    Serial.println("[OTA] Update failed. Restarting.");
    delay(3000);
    ESP.restart();
  }
}

// =====================================================
// WEB PORTAL
// =====================================================

String htmlPage() {
  String html = "";

  html += "<!DOCTYPE html><html><head>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<title>Smart Hub Setup</title>";
  html += "<style>";
  html += "body{font-family:Arial;background:#f4f4f4;padding:20px;}";
  html += ".card{background:white;padding:20px;border-radius:12px;max-width:520px;margin:auto;box-shadow:0 2px 8px rgba(0,0,0,.15);}";
  html += "input{width:100%;padding:12px;margin:8px 0;box-sizing:border-box;}";
  html += "button{width:100%;padding:14px;margin-top:10px;background:#111;color:white;border:0;border-radius:6px;font-size:16px;}";
  html += ".info{background:#eee;padding:10px;border-radius:6px;margin-bottom:10px;font-size:14px;word-wrap:break-word;}";
  html += ".ok{color:green;font-weight:bold;}";
  html += ".bad{color:#b00020;font-weight:bold;}";
  html += "</style></head><body><div class='card'>";

  html += "<h2>Smart Hub Setup</h2>";

  html += "<div class='info'>";
  html += "<b>Hub:</b> " + htmlEscape(hubId);
  html += "<br><b>Factory:</b> " + htmlEscape(factoryId);
  html += "<br><b>Line:</b> " + htmlEscape(lineId);
  html += "<br><b>Setup:</b> http://192.168.4.1";
  html += "<br><b>Wi-Fi:</b> " + htmlEscape(lastWifiStatus);
  html += "<br><b>OTA URL:</b><br>" + htmlEscape(String(HUB_BIN_URL));
  html += "</div>";

  html += "<h3>Status</h3>";
  html += "<div class='info'>";
  html += "<b>Last ESP-NOW:</b><br>" + htmlEscape(lastReceivedSummary);
  html += "<br><br><b>Backend Status:</b> " + htmlEscape(lastBackendStatus);
  html += "<br><b>Last HTTP Code:</b> " + String(lastHttpCode);
  html += "<br><b>Last Backend Response:</b><br>" + htmlEscape(lastBackendResponse);
  html += "<br><br><b>Packets RX:</b> " + String(totalPacketsReceived);
  html += "<br><b>Events Queued:</b> " + String(totalEventsQueued);
  html += "<br><b>Events Posted:</b> " + String(totalEventsPosted);
  html += "<br><b>Events Failed:</b> " + String(totalEventsFailed);
  html += "<br><b>Queue Drops:</b> " + String(totalQueueDrops);
  html += "</div>";

  html += "<h3>Config</h3>";
  html += "<form action='/save' method='POST'>";

  html += "<label>Hub ID</label>";
  html += "<input name='hubid' value='" + htmlEscape(hubId) + "'>";

  html += "<label>Factory ID</label>";
  html += "<input name='factory' value='" + htmlEscape(factoryId) + "'>";

  html += "<label>Line ID</label>";
  html += "<input name='line' value='" + htmlEscape(lineId) + "'>";

  html += "<label>Wi-Fi SSID</label>";
  html += "<input name='ssid' value='" + htmlEscape(wifiSSID) + "'>";

  html += "<label>Wi-Fi Password</label>";
  html += "<input name='pass' type='password' value='" + htmlEscape(wifiPassword) + "'>";

  html += "<label>Supabase / Backend API URL</label>";
  html += "<input name='apiurl' placeholder='https://.../functions/v1/iot-event-ingest' value='" + htmlEscape(apiUrl) + "'>";

  html += "<label>API Token</label>";
  html += "<input name='apitoken' type='password' value='" + htmlEscape(apiToken) + "'>";

  html += "<button type='submit'>Save Config</button>";
  html += "</form>";

  html += "<form action='/test' method='POST'>";
  html += "<button type='submit'>Send Test Event To Backend</button>";
  html += "</form>";

  html += "<form action='/update' method='POST'>";
  html += "<button type='submit'>Update Hub From GitHub Now</button>";
  html += "</form>";

  html += "<form action='/restart' method='POST'>";
  html += "<button type='submit'>Restart Hub</button>";
  html += "</form>";

  html += "</div></body></html>";

  return html;
}

void handleRoot() {
  ensureWiFiConnected(100);
  server.send(200, "text/html", htmlPage());
}

void handleSave() {
  saveConfig(
    server.arg("hubid"),
    server.arg("factory"),
    server.arg("line"),
    server.arg("ssid"),
    server.arg("pass"),
    server.arg("apiurl"),
    server.arg("apitoken")
  );

  server.send(200, "text/html", "<h2>Saved</h2><p>Config saved.</p><p>Restarting Hub...</p>");

  delay(800);
  ESP.restart();
}

void handleTestEvent() {
  BackendEvent ev;

  safeCopy(ev.eventType, "TEST_EVENT", sizeof(ev.eventType));
  safeCopy(ev.buttonId, "TEST-BUTTON", sizeof(ev.buttonId));
  safeCopy(ev.buttonMac, "00:00:00:00:00:00", sizeof(ev.buttonMac));
  safeCopy(ev.badgeUid, "TEST-BADGE", sizeof(ev.badgeUid));
  safeCopy(ev.firmwareVersion, "hub_test", sizeof(ev.firmwareVersion));

  ev.bootId = 0;
  ev.sequence = millis();
  ev.uptimeSeconds = millis() / 1000;
  ev.batteryMv = 0;
  ev.batteryPercent = 0;
  ev.eventValue = 1;

  enqueueBackendEvent(ev);

  server.send(200, "text/html", "<h2>Test Event Queued</h2><p>Wait 2 seconds, then go back and refresh.</p><a href='/'>Back</a>");
}

void handleRestart() {
  server.send(200, "text/html", "<h2>Restarting...</h2>");
  delay(800);
  ESP.restart();
}

void handleCaptive() {
  server.sendHeader("Location", "http://192.168.4.1/", true);
  server.send(302, "text/plain", "");
}

void setupPortal() {
  Serial.println();
  Serial.println("========== HUB PORTAL START ==========");
  Serial.print("[PORTAL] SSID: ");
  Serial.println(hubId);
  Serial.print("[PORTAL] Password: ");
  Serial.println(AP_PASSWORD);
  Serial.println("[PORTAL] URL: http://192.168.4.1");
  Serial.println("======================================");

  WiFi.mode(WIFI_AP_STA);
  WiFi.softAPConfig(apIP, apIP, netMsk);
  WiFi.softAP(hubId.c_str(), AP_PASSWORD, ESPNOW_CHANNEL);

  dnsServer.start(DNS_PORT, "*", apIP);

  server.on("/", HTTP_GET, handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/test", HTTP_POST, handleTestEvent);
  server.on("/update", HTTP_POST, startOTAFromPortal);
  server.on("/restart", HTTP_POST, handleRestart);

  server.on("/generate_204", HTTP_GET, handleRoot);
  server.on("/hotspot-detect.html", HTTP_GET, handleRoot);
  server.on("/fwlink", HTTP_GET, handleRoot);

  server.onNotFound(handleCaptive);

  server.begin();
}

// =====================================================
// ESP-NOW ACK SENDERS
// =====================================================

bool addPeerIfNeeded(const uint8_t *peerMac) {
  if (esp_now_is_peer_exist(peerMac)) return true;

  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, peerMac, 6);
  peerInfo.channel = ESPNOW_CHANNEL;
  peerInfo.encrypt = false;

  esp_err_t result = esp_now_add_peer(&peerInfo);

  if (result == ESP_OK) {
    Serial.print("[ESP-NOW] Added peer: ");
    Serial.println(macToString(peerMac));
    return true;
  }

  Serial.print("[ESP-NOW] Failed to add peer. Code: ");
  Serial.println(result);
  return false;
}

void sendSimpleAck(const uint8_t *buttonMac, const char *buttonName, uint32_t counter) {
  if (!addPeerIfNeeded(buttonMac)) return;

  SimplePacket ack;

  ack.type = SIMPLE_MSG_ACK;
  safeCopy(ack.from, hubId.c_str(), sizeof(ack.from));
  safeCopy(ack.to, buttonName, sizeof(ack.to));
  ack.counter = counter;
  safeCopy(ack.text, "ACK", sizeof(ack.text));

  esp_now_send(buttonMac, (uint8_t *)&ack, sizeof(ack));
}

void sendProtocolAck(
  const uint8_t *buttonMac,
  const char *buttonName,
  uint8_t ackForMessageType,
  uint32_t ackSequence,
  uint8_t statusCode,
  const char *message
) {
  if (!addPeerIfNeeded(buttonMac)) return;

  HubToButtonAck ack;

  ack.protocolVersion = PROTOCOL_VERSION;
  ack.messageType = MSG_ACK;
  ack.ackForMessageType = ackForMessageType;
  ack.statusCode = statusCode;
  ack.ackSequence = ackSequence;
  ack.hubUptimeSeconds = millis() / 1000;

  safeCopy(ack.hubId, hubId.c_str(), sizeof(ack.hubId));
  safeCopy(ack.message, message, sizeof(ack.message));
  safeCopy(ack.targetVersion, "", sizeof(ack.targetVersion));

  esp_now_send(buttonMac, (uint8_t *)&ack, sizeof(ack));
}

// =====================================================
// ESP-NOW RECEIVE HANDLERS
// =====================================================

void handleSimplePacket(const uint8_t *senderMac, const uint8_t *incomingData, int len) {
  SimplePacket packet;
  memcpy(&packet, incomingData, sizeof(packet));

  totalPacketsReceived++;
  lastPacketMs = millis();

  String fromMac = macToString(senderMac);

  lastReceivedSummary =
    "SimplePacket from " + String(packet.from) +
    " / MAC " + fromMac +
    " / Type " + simpleTypeToText(packet.type) +
    " / Counter " + String(packet.counter) +
    " / Text " + String(packet.text);

  if (strcmp(packet.to, hubId.c_str()) != 0) {
    lastReceivedSummary += " / IGNORED: wrong hub";
    return;
  }

  BackendEvent ev;

  if (packet.type == SIMPLE_MSG_HELLO) {
    safeCopy(ev.eventType, "HELLO", sizeof(ev.eventType));
  } else if (packet.type == SIMPLE_MSG_BUTTON_PRESS) {
    safeCopy(ev.eventType, "PIECE_DONE", sizeof(ev.eventType));
  } else if (packet.type == SIMPLE_MSG_RFID_SCAN) {
    safeCopy(ev.eventType, "BADGE_SCAN", sizeof(ev.eventType));
  } else {
    safeCopy(ev.eventType, "UNKNOWN", sizeof(ev.eventType));
  }

  safeCopy(ev.buttonId, packet.from, sizeof(ev.buttonId));
  safeCopy(ev.buttonMac, fromMac.c_str(), sizeof(ev.buttonMac));

  if (packet.type == SIMPLE_MSG_RFID_SCAN) {
    safeCopy(ev.badgeUid, packet.text, sizeof(ev.badgeUid));
  } else {
    safeCopy(ev.badgeUid, "", sizeof(ev.badgeUid));
  }

  safeCopy(ev.firmwareVersion, "simple_protocol", sizeof(ev.firmwareVersion));

  ev.bootId = 0;
  ev.sequence = packet.counter;
  ev.uptimeSeconds = millis() / 1000;
  ev.batteryMv = 0;
  ev.batteryPercent = 0;
  ev.eventValue = packet.counter;

  enqueueBackendEvent(ev);

  sendSimpleAck(senderMac, packet.from, packet.counter);

  ledGreen();
}

void handleProtocolPacket(const uint8_t *senderMac, const uint8_t *incomingData, int len) {
  ButtonToHubPacket packet;
  memcpy(&packet, incomingData, sizeof(packet));

  totalPacketsReceived++;
  lastPacketMs = millis();

  String fromMac = macToString(senderMac);
  String eventType = messageTypeToText(packet.messageType);

  lastReceivedSummary =
    "ButtonToHubPacket from " + String(packet.deviceId) +
    " / MAC " + fromMac +
    " / Type " + eventType +
    " / Sequence " + String(packet.sequence) +
    " / Firmware " + String(packet.firmwareVersion);

  if (packet.protocolVersion != PROTOCOL_VERSION) {
    lastReceivedSummary += " / IGNORED: wrong protocol";
    sendProtocolAck(senderMac, packet.deviceId, packet.messageType, packet.sequence, 1, "BAD_PROTOCOL");
    return;
  }

  if (strcmp(packet.targetHubId, hubId.c_str()) != 0) {
    lastReceivedSummary += " / IGNORED: wrong hub";
    return;
  }

  BackendEvent ev;

  safeCopy(ev.eventType, eventType.c_str(), sizeof(ev.eventType));
  safeCopy(ev.buttonId, packet.deviceId, sizeof(ev.buttonId));
  safeCopy(ev.buttonMac, fromMac.c_str(), sizeof(ev.buttonMac));
  safeCopy(ev.badgeUid, packet.badgeUid, sizeof(ev.badgeUid));
  safeCopy(ev.firmwareVersion, packet.firmwareVersion, sizeof(ev.firmwareVersion));

  ev.bootId = packet.bootId;
  ev.sequence = packet.sequence;
  ev.uptimeSeconds = packet.uptimeSeconds;
  ev.batteryMv = packet.batteryMv;
  ev.batteryPercent = packet.batteryPercent;
  ev.eventValue = packet.eventValue;

  enqueueBackendEvent(ev);

  sendProtocolAck(senderMac, packet.deviceId, packet.messageType, packet.sequence, 0, "OK");

  ledGreen();
}

void handleReceivedData(const uint8_t *senderMac, const uint8_t *incomingData, int len) {
  if (len == sizeof(SimplePacket)) {
    handleSimplePacket(senderMac, incomingData, len);
    return;
  }

  if (len == sizeof(ButtonToHubPacket)) {
    handleProtocolPacket(senderMac, incomingData, len);
    return;
  }

  totalPacketsReceived++;
  lastReceivedSummary =
    "Invalid ESP-NOW packet size: " + String(len) +
    " from " + macToString(senderMac);

  ledYellow();
}

#if ESP_ARDUINO_VERSION_MAJOR >= 3
void onDataRecv(const esp_now_recv_info_t *recvInfo, const uint8_t *incomingData, int len) {
  handleReceivedData(recvInfo->src_addr, incomingData, len);
}
#else
void onDataRecv(const uint8_t *macAddr, const uint8_t *incomingData, int len) {
  handleReceivedData(macAddr, incomingData, len);
}
#endif

#if ESP_ARDUINO_VERSION_MAJOR >= 3
void onDataSent(const wifi_tx_info_t *tx_info, esp_now_send_status_t status) {
  Serial.print("[ESP-NOW] Send callback: ");
  Serial.println(status == ESP_NOW_SEND_SUCCESS ? "LOW LEVEL SUCCESS" : "LOW LEVEL FAIL");
}
#else
void onDataSent(const uint8_t *macAddr, esp_now_send_status_t status) {
  Serial.print("[ESP-NOW] Send callback: ");
  Serial.println(status == ESP_NOW_SEND_SUCCESS ? "LOW LEVEL SUCCESS" : "LOW LEVEL FAIL");
}
#endif

bool setupEspNow() {
  esp_wifi_set_ps(WIFI_PS_NONE);
  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);

  Serial.print("[ESP-NOW] Hub STA MAC: ");
  Serial.println(WiFi.macAddress());
  Serial.print("[ESP-NOW] Hub AP MAC: ");
  Serial.println(WiFi.softAPmacAddress());

  if (esp_now_init() != ESP_OK) {
    Serial.println("[ESP-NOW] Init failed.");
    return false;
  }

  esp_now_register_send_cb(onDataSent);
  esp_now_register_recv_cb(onDataRecv);

  Serial.println("[ESP-NOW] Hub ready.");
  return true;
}

// =====================================================
// SETUP + LOOP
// =====================================================

void setup() {
  Serial.begin(115200);
  delay(1000);

  pinMode(LED_RED_PIN, OUTPUT);
  pinMode(LED_GREEN_PIN, OUTPUT);
  pinMode(LED_BLUE_PIN, OUTPUT);

  ledBlue();

  Serial.println();
  Serial.println("====================================");
  Serial.println("ESS SMART HUB - OTA + BACKEND");
  Serial.println("====================================");
  Serial.print("BOOTSTRAP_FORCE_OVERWRITE: ");
  Serial.println(BOOTSTRAP_FORCE_OVERWRITE ? "true" : "false");
  Serial.print("OTA URL: ");
  Serial.println(HUB_BIN_URL);
  Serial.println("====================================");

  loadConfig();

  setupPortal();

  ensureWiFiConnected(8000);

  if (!setupEspNow()) {
    Serial.println("[BOOT] ESP-NOW failed.");
    ledYellow();
  }

  Serial.println("[BOOT] Hub is running.");
  Serial.println("[BOOT] Portal is always active.");
  Serial.println("[BOOT] Waiting for Buttons...");
}

void loop() {
  dnsServer.processNextRequest();
  server.handleClient();

  processBackendQueue();

  if (millis() - lastPacketMs > 1500) {
    ledBlue();
  }
}