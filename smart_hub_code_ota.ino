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
// FIRMWARE VERSION
// Bump this string after building and pushing new .bin to GitHub.
// Also update version.txt in the repo root to match.
// =====================================================

#define HUB_FIRMWARE_VERSION "1.5.0"

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
// OTA URLS + AUTO-CHECK INTERVAL
// =====================================================

#define HUB_BIN_URL     "https://raw.githubusercontent.com/ismailoviic/smart_hub_code_ota/main/build/esp32dev/firmware.bin"
#define HUB_VERSION_URL "https://raw.githubusercontent.com/ismailoviic/smart_hub_code_ota/main/version.txt"

#define BTN_VERSION_URL "https://raw.githubusercontent.com/ismailoviic/smart_button_code_ota/main/version.txt"

#define OTA_CHECK_INTERVAL_MS 10000  // 10 seconds for dev — change to 3600000 for production

// =====================================================
// BACKEND API (hardcoded — no longer configurable via portal)
// =====================================================

#define HUB_API_URL "http://13.38.147.196/api/v1/iot/ingest"

#define HEARTBEAT_INTERVAL_MS 10000  // HUB_PING sent to backend every 10 seconds

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

#define AP_PASSWORD    "12345678"
#define ESPNOW_CHANNEL 8  // fallback only — real channel follows the Wi-Fi router once STA is connected

#define CHANNEL_CHECK_INTERVAL_MS 5000

// =====================================================
// PROTOCOL CONSTANTS
// =====================================================

#define SIMPLE_MSG_HELLO        1
#define SIMPLE_MSG_ACK          2
#define SIMPLE_MSG_BUTTON_PRESS 3
#define SIMPLE_MSG_RFID_SCAN    4

#define PROTOCOL_VERSION 1

#define EVENT_QUEUE_SIZE   8
#define MAX_KNOWN_BUTTONS 16

// =====================================================
// STRUCTS / TYPES
// All defined here so PlatformIO auto-prototypes can find them.
// =====================================================

typedef struct __attribute__((packed)) {
  uint8_t  type;
  char     from[16];
  char     to[16];
  uint32_t counter;
  char     text[32];
} SimplePacket;

enum MessageType {
  MSG_HELLO          = 1,
  MSG_ACK            = 2,
  MSG_BADGE_SCAN     = 3,
  MSG_PIECE_DONE     = 4,
  MSG_BATTERY_STATUS = 5,
  MSG_HEARTBEAT      = 6,
  MSG_ERROR          = 7,
  MSG_CONFIG_UPDATE  = 8,
  MSG_OTA_AVAILABLE  = 9,
  MSG_OTA_START      = 10,
  MSG_OTA_STATUS     = 11
};

typedef struct __attribute__((packed)) {
  uint8_t  protocolVersion;
  uint8_t  messageType;
  uint8_t  batteryPercent;
  uint8_t  reserved1;
  uint32_t bootId;
  uint32_t sequence;
  uint32_t uptimeSeconds;
  uint16_t batteryMv;
  uint16_t eventValue;
  char     deviceId[16];
  char     targetHubId[16];
  char     badgeUid[24];
  char     firmwareVersion[16];
} ButtonToHubPacket;

typedef struct __attribute__((packed)) {
  uint8_t  protocolVersion;
  uint8_t  messageType;
  uint8_t  ackForMessageType;
  uint8_t  statusCode;
  uint32_t ackSequence;
  uint32_t hubUptimeSeconds;
  char     hubId[16];
  char     message[32];
  char     targetVersion[16];
} HubToButtonAck;

struct BackendEvent {
  char     eventType[24];
  char     buttonId[16];
  char     buttonMac[18];
  char     badgeUid[24];
  char     firmwareVersion[16];
  uint32_t bootId;
  uint32_t sequence;
  uint32_t uptimeSeconds;
  uint16_t batteryMv;
  uint8_t  batteryPercent;
  uint16_t eventValue;
};

struct KnownButton {
  uint8_t  mac[6];
  char     name[16];
  bool     active;
  uint16_t batteryMv;
};

// =====================================================
// GLOBALS
// =====================================================

IPAddress apIP(192, 168, 4, 1);
IPAddress netMsk(255, 255, 255, 0);

Preferences prefs;
WebServer   server(80);
DNSServer   dnsServer;

const byte DNS_PORT = 53;

String hubId;
String factoryId;
String lineId;
String wifiSSID;
String wifiPassword;

BackendEvent eventQueue[EVENT_QUEUE_SIZE];
volatile int queueHead = 0;
volatile int queueTail = 0;

KnownButton knownButtons[MAX_KNOWN_BUTTONS];

unsigned long lastPacketMs    = 0;
unsigned long lastBackendMs   = 0;
unsigned long lastOtaCheckMs  = 0;
unsigned long lastHeartbeatMs = 0;
unsigned long lastChannelCheckMs = 0;

// Real operating channel for AP + ESP-NOW. ESP32 has one radio, so once STA
// joins the real Wi-Fi router, the radio (and the co-located softAP) is
// forced onto the router's channel — ESPNOW_CHANNEL is only a fallback for
// when there's no Wi-Fi configured at all. Buttons don't join any router, so
// they discover this channel by hopping (see button firmware).
int activeChannel = ESPNOW_CHANNEL;

uint32_t totalPacketsReceived = 0;
uint32_t totalEventsQueued    = 0;
uint32_t totalEventsPosted    = 0;
uint32_t totalEventsFailed    = 0;
uint32_t totalQueueDrops      = 0;

int    lastHttpCode        = 0;
String lastBackendStatus   = "No backend request yet";
String lastBackendResponse = "";
String lastReceivedSummary = "No ESP-NOW packet yet";
String lastWifiStatus      = "Not connected";

// =====================================================
// LIVE EVENT LOG (for the /logs debug view on the portal)
// =====================================================

#define LOG_BUFFER_SIZE 60
String eventLog[LOG_BUFFER_SIZE];
int logNext  = 0;
int logCount = 0;

void addLog(const String &entry) {
  String stamped = "[" + String(millis() / 1000) + "s] " + entry;
  eventLog[logNext] = stamped;
  logNext = (logNext + 1) % LOG_BUFFER_SIZE;
  if (logCount < LOG_BUFFER_SIZE) logCount++;
  Serial.println(stamped);
}

// =====================================================
// LED
// =====================================================

void setLED(bool r, bool g, bool b) {
  digitalWrite(LED_RED_PIN,   r ? HIGH : LOW);
  digitalWrite(LED_GREEN_PIN, g ? HIGH : LOW);
  digitalWrite(LED_BLUE_PIN,  b ? HIGH : LOW);
}

void ledBlue()   { setLED(false, false, true);  }
void ledGreen()  { setLED(false, true,  false); }
void ledYellow() { setLED(true,  true,  false); }
void ledPurple() { setLED(true,  false, true);  }
void ledRed()    { setLED(true,  false, false); }
void ledCyan()   { setLED(false, true,  true);  }  // OTA in progress: green + blue

bool backendOnline = false;

void updateConnectionLED() {
  if (backendOnline) ledGreen(); else ledBlue();
}

void blinkPurpleDuringOTA() {
  ledCyan();
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
  snprintf(buffer, sizeof(buffer),
    "%02X:%02X:%02X:%02X:%02X:%02X",
    mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(buffer);
}

String messageTypeToText(uint8_t type) {
  switch (type) {
    case MSG_HELLO:          return "HELLO";
    case MSG_ACK:            return "ACK";
    case MSG_BADGE_SCAN:     return "BADGE_SCAN";
    case MSG_PIECE_DONE:     return "PIECE_DONE";
    case MSG_BATTERY_STATUS: return "BATTERY_STATUS";
    case MSG_HEARTBEAT:      return "HEARTBEAT";
    case MSG_ERROR:          return "ERROR";
    case MSG_CONFIG_UPDATE:  return "CONFIG_UPDATE";
    case MSG_OTA_AVAILABLE:  return "OTA_AVAILABLE";
    case MSG_OTA_START:      return "OTA_START";
    case MSG_OTA_STATUS:     return "OTA_STATUS";
    default:                 return "UNKNOWN";
  }
}

String simpleTypeToText(uint8_t type) {
  switch (type) {
    case SIMPLE_MSG_HELLO:        return "HELLO";
    case SIMPLE_MSG_ACK:          return "ACK";
    case SIMPLE_MSG_BUTTON_PRESS: return "PIECE_DONE";
    case SIMPLE_MSG_RFID_SCAN:    return "BADGE_SCAN";
    default:                      return "UNKNOWN";
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
  value.replace("&",  "&amp;");
  value.replace("<",  "&lt;");
  value.replace(">",  "&gt;");
  value.replace("\"", "&quot;");
  value.replace("'",  "&#39;");
  return value;
}

// =====================================================
// KNOWN BUTTONS
// =====================================================

uint16_t getButtonBatteryMv(const char *name) {
  for (int i = 0; i < MAX_KNOWN_BUTTONS; i++) {
    if (knownButtons[i].active && strcmp(knownButtons[i].name, name) == 0)
      return knownButtons[i].batteryMv;
  }
  return 0;
}

uint8_t getButtonBatteryPct(uint16_t mv) {
  if (mv >= 4200) return 100;
  if (mv <= 3000) return 0;
  return (uint8_t)((mv - 3000UL) * 100 / 1200);
}

void registerButton(const uint8_t *mac, const char *name, uint16_t batteryMv = 0) {
  for (int i = 0; i < MAX_KNOWN_BUTTONS; i++) {
    if (knownButtons[i].active && memcmp(knownButtons[i].mac, mac, 6) == 0) {
      if (batteryMv > 0) knownButtons[i].batteryMv = batteryMv;
      return;
    }
  }
  for (int i = 0; i < MAX_KNOWN_BUTTONS; i++) {
    if (!knownButtons[i].active) {
      memcpy(knownButtons[i].mac, mac, 6);
      safeCopy(knownButtons[i].name, name, sizeof(knownButtons[i].name));
      knownButtons[i].active    = true;
      knownButtons[i].batteryMv = batteryMv;
      Serial.print("[BUTTONS] Registered: ");
      Serial.print(name);
      Serial.print(" / ");
      Serial.println(macToString(mac));
      return;
    }
  }
  Serial.println("[BUTTONS] Known button table full.");
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
  if (queueTail == queueHead) return false;
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
    prefs.putString("hubid",   BOOTSTRAP_HUB_ID);
    prefs.putString("factory", BOOTSTRAP_FACTORY_ID);
    prefs.putString("line",    BOOTSTRAP_LINE_ID);
    if (!initialized) {
      prefs.putString("ssid",     "");
      prefs.putString("pass",     "");
    }
    prefs.putBool("initialized", true);
  }
  prefs.end();
}

void loadConfig() {
  saveBootstrapIdentityIfNeeded();
  prefs.begin("hub_cfg", true);
  hubId       = prefs.getString("hubid",    BOOTSTRAP_HUB_ID);
  factoryId   = prefs.getString("factory",  BOOTSTRAP_FACTORY_ID);
  lineId      = prefs.getString("line",     BOOTSTRAP_LINE_ID);
  wifiSSID    = prefs.getString("ssid",     "");
  wifiPassword= prefs.getString("pass",     "");
  prefs.end();

  Serial.println();
  Serial.println("========== LOADED HUB CONFIG ==========");
  Serial.print("Hub ID: ");      Serial.println(hubId);
  Serial.print("Factory ID: "); Serial.println(factoryId);
  Serial.print("Line ID: ");    Serial.println(lineId);
  Serial.print("Wi-Fi SSID: "); Serial.println(wifiSSID);
  Serial.print("API URL: ");    Serial.println(HUB_API_URL);
  Serial.println("=======================================");
}

void saveConfig(
  String newHubId, String newFactoryId, String newLineId,
  String ssid, String pass
) {
  prefs.begin("hub_cfg", false);
  prefs.putString("hubid",       newHubId);
  prefs.putString("factory",     newFactoryId);
  prefs.putString("line",        newLineId);
  prefs.putString("ssid",        ssid);
  prefs.putString("pass",        pass);
  prefs.putBool("initialized",   true);
  prefs.end();
  Serial.println("[NVS] Hub config saved.");
  loadConfig();
}

// =====================================================
// WIFI
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
  json += "\"line_id\":\""    + jsonEscape(lineId)    + "\",";
  json += "\"hub_id\":\""     + jsonEscape(hubId)     + "\",";
  json += "\"hub_firmware_version\":\"" + jsonEscape(String(HUB_FIRMWARE_VERSION)) + "\",";
  json += "\"hub_uptime_seconds\":"     + String(millis() / 1000)                  + ",";
  json += "\"hub_battery_mv\":0,";
  json += "\"hub_battery_pct\":0,";
  json += "\"button_id\":\""  + jsonEscape(String(ev.buttonId))  + "\",";
  json += "\"button_mac\":\"" + jsonEscape(String(ev.buttonMac)) + "\",";
  json += "\"button_firmware_version\":\"" + jsonEscape(String(ev.firmwareVersion)) + "\",";
  json += "\"button_battery_mv\":"   + String(ev.batteryMv)      + ",";
  json += "\"button_battery_pct\":"  + String(ev.batteryPercent) + ",";
  json += "\"event_type\":\"" + jsonEscape(String(ev.eventType)) + "\",";
  json += "\"badge_uid\":\""  + jsonEscape(String(ev.badgeUid))  + "\",";
  json += "\"boot_id\":"      + String(ev.bootId)        + ",";
  json += "\"sequence\":"     + String(ev.sequence)      + ",";
  json += "\"uptime_seconds\":"+ String(ev.uptimeSeconds)+ ",";
  json += "\"event_value\":"  + String(ev.eventValue);
  json += "}";
  return json;
}

bool sendEventToBackend(const BackendEvent &ev) {
  if (!ensureWiFiConnected()) {
    lastBackendStatus = "Wi-Fi not connected";
    totalEventsFailed++;
    backendOnline = false;
    return false;
  }
  String json = buildBackendJson(ev);
  Serial.println();
  Serial.println("========== BACKEND POST ==========");
  Serial.print("URL: "); Serial.println(HUB_API_URL);
  Serial.println(json);
  Serial.println("==================================");

  HTTPClient http;
  WiFiClient       normalClient;
  WiFiClientSecure secureClient;
  String apiUrl = HUB_API_URL;
  bool isHttps = apiUrl.startsWith("https://");
  bool beginOk = isHttps
    ? (secureClient.setInsecure(), http.begin(secureClient, apiUrl))
    : http.begin(normalClient, apiUrl);

  if (!beginOk) {
    lastBackendStatus = "HTTP begin failed";
    totalEventsFailed++;
    backendOnline = false;
    return false;
  }
  http.setTimeout(15000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.addHeader("Content-Type", "application/json");
  int    code     = http.POST(json);
  String response = http.getString();
  http.end();

  lastHttpCode        = code;
  lastBackendResponse = response;
  if (lastBackendResponse.length() > 500)
    lastBackendResponse = lastBackendResponse.substring(0, 500);
  lastBackendMs = millis();

  Serial.print("[BACKEND] HTTP code: "); Serial.println(code);
  Serial.print("[BACKEND] Response: ");  Serial.println(response);

  if (code >= 200 && code < 300) {
    lastBackendStatus = "POST OK";
    totalEventsPosted++;
    backendOnline = true;
    updateConnectionLED();
    return true;
  }
  lastBackendStatus = "POST failed";
  totalEventsFailed++;
  backendOnline = false;
  ledYellow(); delay(120);
  updateConnectionLED();
  return false;
}

void processBackendQueue() {
  BackendEvent ev;
  if (!dequeueBackendEvent(ev)) return;
  sendEventToBackend(ev);
}

void sendHeartbeat() {
  BackendEvent ev = {};
  safeCopy(ev.eventType,       "HUB_PING", sizeof(ev.eventType));
  safeCopy(ev.buttonId,        "",         sizeof(ev.buttonId));
  safeCopy(ev.buttonMac,       "",         sizeof(ev.buttonMac));
  safeCopy(ev.badgeUid,        "",         sizeof(ev.badgeUid));
  safeCopy(ev.firmwareVersion, "",         sizeof(ev.firmwareVersion));
  ev.bootId        = 0;
  ev.sequence      = 0;
  ev.uptimeSeconds = millis() / 1000;
  ev.batteryMv     = 0;
  ev.batteryPercent= 0;
  ev.eventValue    = 0;
  enqueueBackendEvent(ev);
}

// =====================================================
// AUTO OTA VERSION CHECK
// =====================================================

String fetchRemoteVersion(const char *url) {
  if (WiFi.status() != WL_CONNECTED) return "";
  HTTPClient       http;
  WiFiClientSecure client;
  client.setInsecure();
  if (!http.begin(client, url)) return "";
  http.setTimeout(8000);
  http.addHeader("Cache-Control", "no-cache");
  int    code    = http.GET();
  String version = "";
  if (code == HTTP_CODE_OK) {
    version = http.getString();
    version.trim();
  }
  http.end();
  return version;
}

void sendOtaStartToAllButtons() {
  for (int i = 0; i < MAX_KNOWN_BUTTONS; i++) {
    if (!knownButtons[i].active) continue;
    if (esp_now_is_peer_exist(knownButtons[i].mac)) {
      // already a peer, fine
    } else {
      esp_now_peer_info_t peerInfo = {};
      memcpy(peerInfo.peer_addr, knownButtons[i].mac, 6);
      peerInfo.channel = 0;  // use whatever channel the radio is currently on
      peerInfo.encrypt = false;
      if (esp_now_add_peer(&peerInfo) != ESP_OK) continue;
    }
    SimplePacket pkt;
    pkt.type = SIMPLE_MSG_ACK;
    safeCopy(pkt.from, hubId.c_str(),          sizeof(pkt.from));
    safeCopy(pkt.to,   knownButtons[i].name,   sizeof(pkt.to));
    pkt.counter = 0;
    safeCopy(pkt.text, "OTA_START",             sizeof(pkt.text));
    esp_now_send(knownButtons[i].mac, (uint8_t *)&pkt, sizeof(pkt));
    Serial.print("[AUTO-OTA] Sent OTA_START to: ");
    Serial.println(knownButtons[i].name);
  }
}

bool connectWiFiForOTA();
bool performOTA();

void checkForUpdates() {
  if (!ensureWiFiConnected(5000)) {
    Serial.println("[AUTO-OTA] No Wi-Fi, skipping version check.");
    return;
  }

  Serial.println("[AUTO-OTA] Checking hub version...");
  String remoteHubVersion = fetchRemoteVersion(HUB_VERSION_URL);
  if (remoteHubVersion.length() > 0 && remoteHubVersion != HUB_FIRMWARE_VERSION) {
    Serial.println("[AUTO-OTA] Hub version changed: " + String(HUB_FIRMWARE_VERSION) + " -> " + remoteHubVersion);
    if (connectWiFiForOTA()) performOTA();
    return;
  }
  Serial.println("[AUTO-OTA] Hub up to date (" + String(HUB_FIRMWARE_VERSION) + ").");

  Serial.println("[AUTO-OTA] Checking button version...");
  String remoteButtonVersion = fetchRemoteVersion(BTN_VERSION_URL);
  if (remoteButtonVersion.length() > 0) {
    prefs.begin("hub_cfg", true);
    String storedButtonVersion = prefs.getString("btn_ver", "");
    prefs.end();
    if (storedButtonVersion != remoteButtonVersion) {
      Serial.println("[AUTO-OTA] Button version changed: " + storedButtonVersion + " -> " + remoteButtonVersion);
      prefs.begin("hub_cfg", false);
      prefs.putString("btn_ver", remoteButtonVersion);
      prefs.end();
      sendOtaStartToAllButtons();
    } else {
      Serial.println("[AUTO-OTA] Buttons up to date (" + remoteButtonVersion + ").");
    }
  }
}

// =====================================================
// OTA (manual + auto)
// =====================================================

bool connectWiFiForOTA() {
  if (wifiSSID.length() == 0) {
    Serial.println("[OTA] No Wi-Fi SSID saved.");
    return false;
  }
  Serial.print("[OTA] Connecting to Wi-Fi: "); Serial.println(wifiSSID);
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
    Serial.print("[OTA] Connected. IP: "); Serial.println(WiFi.localIP());
    return true;
  }
  Serial.println("[OTA] Wi-Fi failed.");
  return false;
}

bool performOTA() {
  Serial.println();
  Serial.println("========== HUB OTA START ==========");
  Serial.print("[OTA] URL: "); Serial.println(HUB_BIN_URL);
  HTTPClient       http;
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
  Serial.print("[OTA] HTTP code: "); Serial.println(httpCode);
  if (httpCode != HTTP_CODE_OK) {
    Serial.println("[OTA] Download failed.");
    http.end();
    return false;
  }
  int contentLength = http.getSize();
  Serial.print("[OTA] Content length: "); Serial.println(contentLength);
  if (!Update.begin(contentLength > 0 ? contentLength : UPDATE_SIZE_UNKNOWN)) {
    Serial.println("[OTA] Update.begin failed.");
    Update.printError(Serial);
    http.end();
    return false;
  }
  WiFiClient *stream  = http.getStreamPtr();
  size_t      written = Update.writeStream(*stream);
  Serial.print("[OTA] Written bytes: "); Serial.println(written);
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
  ledCyan();
  delay(1000);
  ESP.restart();
  return true;
}

void startOTAFromPortal() {
  Serial.println("[PORTAL] Hub OTA requested.");
  server.send(200, "text/html", "<h2>Hub OTA started</h2><p>Hub is updating from GitHub. Reconnect after 1 minute.</p>");
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
  html += ".ok{color:green;font-weight:bold;}.bad{color:#b00020;font-weight:bold;}";
  html += "</style></head><body><div class='card'>";
  html += "<h2>Smart Hub Setup</h2>";
  html += "<div class='info'>";
  html += "<b>Hub:</b> "     + htmlEscape(hubId);
  html += "<br><b>Factory:</b> " + htmlEscape(factoryId);
  html += "<br><b>Line:</b> "    + htmlEscape(lineId);
  html += "<br><b>Firmware:</b> " + String(HUB_FIRMWARE_VERSION);
  html += "<br><b>Setup:</b> http://192.168.4.1";
  html += "<br><b>Wi-Fi:</b> "   + htmlEscape(lastWifiStatus);
  html += "<br><b>OTA URL:</b><br>" + htmlEscape(String(HUB_BIN_URL));
  html += "<br><b>Backend API URL:</b><br>" + htmlEscape(String(HUB_API_URL));
  html += "</div>";
  html += "<h3>Status</h3>";
  html += "<div class='info'>";
  html += "<b>Last ESP-NOW:</b><br>" + htmlEscape(lastReceivedSummary);
  html += "<br><br><b>Backend Status:</b> " + htmlEscape(lastBackendStatus);
  html += "<br><b>Last HTTP Code:</b> "      + String(lastHttpCode);
  html += "<br><b>Last Backend Response:</b><br>" + htmlEscape(lastBackendResponse);
  html += "<br><br><b>Packets RX:</b> "  + String(totalPacketsReceived);
  html += "<br><b>Events Queued:</b> "   + String(totalEventsQueued);
  html += "<br><b>Events Posted:</b> "   + String(totalEventsPosted);
  html += "<br><b>Events Failed:</b> "   + String(totalEventsFailed);
  html += "<br><b>Queue Drops:</b> "     + String(totalQueueDrops);
  html += "</div>";
  html += "<h3>Known Buttons</h3>";
  html += "<div class='info'>";
  bool anyButton = false;
  for (int i = 0; i < MAX_KNOWN_BUTTONS; i++) {
    if (!knownButtons[i].active) continue;
    anyButton = true;
    uint16_t mv  = knownButtons[i].batteryMv;
    uint8_t  pct = (mv >= 4200) ? 100 : (mv <= 3000) ? 0 : (uint8_t)((mv - 3000UL) * 100 / 1200);
    String battStr = (mv > 0)
      ? (String(mv) + " mV (" + String(pct) + "%)")
      : String("--");
    html += "<b>" + htmlEscape(String(knownButtons[i].name)) + "</b>";
    html += " &nbsp; " + htmlEscape(macToString(knownButtons[i].mac));
    html += " &nbsp; Battery: " + battStr;
    html += "<br>";
  }
  if (!anyButton) html += "No buttons seen yet.";
  html += "</div>";
  html += "<h3>Config</h3>";
  html += "<form action='/save' method='POST'>";
  html += "<label>Hub ID</label><input name='hubid' value='"    + htmlEscape(hubId)      + "'>";
  html += "<label>Factory ID</label><input name='factory' value='" + htmlEscape(factoryId) + "'>";
  html += "<label>Line ID</label><input name='line' value='"    + htmlEscape(lineId)     + "'>";
  html += "<label>Wi-Fi SSID</label><input name='ssid' value='" + htmlEscape(wifiSSID)   + "'>";
  html += "<label>Wi-Fi Password</label><input name='pass' type='password' value='" + htmlEscape(wifiPassword) + "'>";
  html += "<button type='submit'>Save Config</button></form>";
  html += "<form action='/test' method='POST'><button type='submit'>Send Test Event To Backend</button></form>";
  html += "<form action='/update' method='POST'><button type='submit'>Update Hub From GitHub Now</button></form>";
  html += "<form action='/restart' method='POST'><button type='submit'>Restart Hub</button></form>";
  html += "</div></body></html>";
  return html;
}

void handleRoot() {
  ensureWiFiConnected(100);
  server.send(200, "text/html", htmlPage());
}

void handleSave() {
  saveConfig(
    server.arg("hubid"), server.arg("factory"), server.arg("line"),
    server.arg("ssid"),  server.arg("pass")
  );
  server.send(200, "text/html", "<h2>Saved</h2><p>Config saved. Restarting Hub...</p>");
  delay(800);
  ESP.restart();
}

void handleTestEvent() {
  BackendEvent ev;
  safeCopy(ev.eventType,       "TEST_EVENT",        sizeof(ev.eventType));
  safeCopy(ev.buttonId,        "TEST-BUTTON",       sizeof(ev.buttonId));
  safeCopy(ev.buttonMac,       "00:00:00:00:00:00", sizeof(ev.buttonMac));
  safeCopy(ev.badgeUid,        "TEST-BADGE",        sizeof(ev.badgeUid));
  safeCopy(ev.firmwareVersion, "hub_test",          sizeof(ev.firmwareVersion));
  ev.bootId        = 0;
  ev.sequence      = millis();
  ev.uptimeSeconds = millis() / 1000;
  ev.batteryMv     = 0;
  ev.batteryPercent= 0;
  ev.eventValue    = 1;
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
  Serial.print("[PORTAL] SSID: ");     Serial.println(hubId);
  Serial.print("[PORTAL] Password: "); Serial.println(AP_PASSWORD);
  Serial.println("[PORTAL] URL: http://192.168.4.1");
  Serial.println("======================================");
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAPConfig(apIP, apIP, netMsk);
  WiFi.softAP(hubId.c_str(), AP_PASSWORD, ESPNOW_CHANNEL);
  dnsServer.start(DNS_PORT, "*", apIP);
  server.on("/",                    HTTP_GET,  handleRoot);
  server.on("/save",                HTTP_POST, handleSave);
  server.on("/test",                HTTP_POST, handleTestEvent);
  server.on("/update",              HTTP_POST, startOTAFromPortal);
  server.on("/restart",             HTTP_POST, handleRestart);
  server.on("/generate_204",        HTTP_GET,  handleRoot);
  server.on("/hotspot-detect.html", HTTP_GET,  handleRoot);
  server.on("/fwlink",              HTTP_GET,  handleRoot);
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
  peerInfo.channel = 0;  // use whatever channel the radio is currently on
  peerInfo.encrypt = false;
  esp_err_t result = esp_now_add_peer(&peerInfo);
  if (result == ESP_OK) {
    Serial.print("[ESP-NOW] Added peer: "); Serial.println(macToString(peerMac));
    return true;
  }
  Serial.print("[ESP-NOW] Failed to add peer. Code: "); Serial.println(result);
  return false;
}

void sendSimpleAck(const uint8_t *buttonMac, const char *buttonName, uint32_t counter) {
  if (!addPeerIfNeeded(buttonMac)) return;
  SimplePacket ack;
  ack.type = SIMPLE_MSG_ACK;
  safeCopy(ack.from, hubId.c_str(), sizeof(ack.from));
  safeCopy(ack.to,   buttonName,    sizeof(ack.to));
  ack.counter = counter;
  safeCopy(ack.text, "ACK",         sizeof(ack.text));
  esp_now_send(buttonMac, (uint8_t *)&ack, sizeof(ack));
}

void sendProtocolAck(
  const uint8_t *buttonMac, const char *buttonName,
  uint8_t ackForMessageType, uint32_t ackSequence,
  uint8_t statusCode, const char *message
) {
  if (!addPeerIfNeeded(buttonMac)) return;
  HubToButtonAck ack;
  ack.protocolVersion    = PROTOCOL_VERSION;
  ack.messageType        = MSG_ACK;
  ack.ackForMessageType  = ackForMessageType;
  ack.statusCode         = statusCode;
  ack.ackSequence        = ackSequence;
  ack.hubUptimeSeconds   = millis() / 1000;
  safeCopy(ack.hubId,         hubId.c_str(), sizeof(ack.hubId));
  safeCopy(ack.message,       message,       sizeof(ack.message));
  safeCopy(ack.targetVersion, "",            sizeof(ack.targetVersion));
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
    uint16_t battMv = 0;
    sscanf(packet.text, "HELLO:%hu", &battMv);
    registerButton(senderMac, packet.from, battMv);
    safeCopy(ev.eventType, "HELLO",      sizeof(ev.eventType));
  } else if (packet.type == SIMPLE_MSG_BUTTON_PRESS) {
    safeCopy(ev.eventType, "PIECE_DONE", sizeof(ev.eventType));
  } else if (packet.type == SIMPLE_MSG_RFID_SCAN) {
    safeCopy(ev.eventType, "BADGE_SCAN", sizeof(ev.eventType));
  } else {
    safeCopy(ev.eventType, "UNKNOWN",    sizeof(ev.eventType));
  }

  safeCopy(ev.buttonId,  packet.from,    sizeof(ev.buttonId));
  safeCopy(ev.buttonMac, fromMac.c_str(),sizeof(ev.buttonMac));

  if (packet.type == SIMPLE_MSG_RFID_SCAN)
    safeCopy(ev.badgeUid, packet.text, sizeof(ev.badgeUid));
  else
    safeCopy(ev.badgeUid, "",          sizeof(ev.badgeUid));

  safeCopy(ev.firmwareVersion, "simple_protocol", sizeof(ev.firmwareVersion));
  ev.bootId        = 0;
  ev.sequence      = packet.counter;
  ev.uptimeSeconds = millis() / 1000;
  ev.batteryMv      = getButtonBatteryMv(packet.from);
  ev.batteryPercent = getButtonBatteryPct(ev.batteryMv);
  ev.eventValue     = packet.counter;

  enqueueBackendEvent(ev);
  sendSimpleAck(senderMac, packet.from, packet.counter);
  ledGreen();
}

void handleProtocolPacket(const uint8_t *senderMac, const uint8_t *incomingData, int len) {
  ButtonToHubPacket packet;
  memcpy(&packet, incomingData, sizeof(packet));
  totalPacketsReceived++;
  lastPacketMs = millis();
  String fromMac   = macToString(senderMac);
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
  safeCopy(ev.eventType,       eventType.c_str(),       sizeof(ev.eventType));
  safeCopy(ev.buttonId,        packet.deviceId,         sizeof(ev.buttonId));
  safeCopy(ev.buttonMac,       fromMac.c_str(),         sizeof(ev.buttonMac));
  safeCopy(ev.badgeUid,        packet.badgeUid,         sizeof(ev.badgeUid));
  safeCopy(ev.firmwareVersion, packet.firmwareVersion,  sizeof(ev.firmwareVersion));
  ev.bootId        = packet.bootId;
  ev.sequence      = packet.sequence;
  ev.uptimeSeconds = packet.uptimeSeconds;
  ev.batteryMv     = packet.batteryMv;
  ev.batteryPercent= packet.batteryPercent;
  ev.eventValue    = packet.eventValue;

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

static void onDataRecv(const uint8_t *macAddr, const uint8_t *incomingData, int len) {
  handleReceivedData(macAddr, incomingData, len);
}

static void onDataSent(const uint8_t *macAddr, esp_now_send_status_t status) {
  Serial.print("[ESP-NOW] Send callback: ");
  Serial.println(status == ESP_NOW_SEND_SUCCESS ? "SUCCESS" : "FAIL");
}

bool setupEspNow() {
  esp_wifi_set_ps(WIFI_PS_NONE);

  // If STA is connected to the real router, the radio is already locked to
  // that channel — match it instead of fighting it. Otherwise fall back to
  // the fixed ESPNOW_CHANNEL (e.g. portal-only mode with no Wi-Fi).
  activeChannel = (WiFi.status() == WL_CONNECTED) ? WiFi.channel() : ESPNOW_CHANNEL;
  esp_wifi_set_channel(activeChannel, WIFI_SECOND_CHAN_NONE);
  Serial.print("[ESP-NOW] Operating channel: "); Serial.println(activeChannel);

  Serial.print("[ESP-NOW] Hub STA MAC: "); Serial.println(WiFi.macAddress());
  Serial.print("[ESP-NOW] Hub AP MAC: ");  Serial.println(WiFi.softAPmacAddress());
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
  pinMode(LED_RED_PIN,   OUTPUT);
  pinMode(LED_GREEN_PIN, OUTPUT);
  pinMode(LED_BLUE_PIN,  OUTPUT);
  ledBlue();
  Serial.println();
  Serial.println("====================================");
  Serial.println("ESS SMART HUB - OTA + BACKEND");
  Serial.println("====================================");
  Serial.print("FIRMWARE VERSION: "); Serial.println(HUB_FIRMWARE_VERSION);
  Serial.print("OTA URL: ");          Serial.println(HUB_BIN_URL);
  Serial.println("====================================");
  loadConfig();
  setupPortal();
  ensureWiFiConnected(8000);
  if (!setupEspNow()) {
    Serial.println("[BOOT] ESP-NOW failed.");
    ledYellow();
  }
  Serial.println("[BOOT] Hub is running.");
  Serial.println("[BOOT] Portal always active.");
  Serial.println("[BOOT] Waiting for Buttons...");
}

void loop() {
  dnsServer.processNextRequest();
  server.handleClient();
  processBackendQueue();
  if (millis() - lastHeartbeatMs >= HEARTBEAT_INTERVAL_MS) {
    lastHeartbeatMs = millis();
    sendHeartbeat();
  }
  if (millis() - lastOtaCheckMs >= OTA_CHECK_INTERVAL_MS) {
    lastOtaCheckMs = millis();
    checkForUpdates();
  }
  if (millis() - lastChannelCheckMs >= CHANNEL_CHECK_INTERVAL_MS) {
    lastChannelCheckMs = millis();
    if (WiFi.status() == WL_CONNECTED && WiFi.channel() != activeChannel) {
      activeChannel = WiFi.channel();
      esp_wifi_set_channel(activeChannel, WIFI_SECOND_CHAN_NONE);
      Serial.print("[ESP-NOW] Router channel changed, now operating on: ");
      Serial.println(activeChannel);
    }
  }
  if (millis() - lastPacketMs > 1500) {
    updateConnectionLED();
  }
}
