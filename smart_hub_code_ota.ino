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

#define BOOTSTRAP_HUB_ID "HUB-L1"

// #define BOOTSTRAP_HUB_ID "HUB-L2"

// true  = first manual upload to write Hub identity into NVS
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
// =====================================================

#define AP_PASSWORD "12345678"
#define ESPNOW_CHANNEL 8

IPAddress apIP(192, 168, 4, 1);
IPAddress netMsk(255, 255, 255, 0);

Preferences prefs;
WebServer server(80);
DNSServer dnsServer;

const byte DNS_PORT = 53;

String hubId;
String wifiSSID;
String wifiPassword;

unsigned long lastPacketMs = 0;

// =====================================================
// SIMPLE ESP-NOW PROTOCOL
// =====================================================

#define MSG_HELLO        1
#define MSG_ACK          2
#define MSG_BUTTON_PRESS 3
#define MSG_RFID_SCAN    4

typedef struct __attribute__((packed)) {
  uint8_t type;
  char from[16];
  char to[16];
  uint32_t counter;
  char text[32];
} SimplePacket;

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

void blinkGreen() {
  ledGreen();
  delay(150);
  setLED(false, false, false);
  delay(100);
  ledGreen();
  delay(150);
  ledBlue();
}

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

// =====================================================
// NVS CONFIG
// =====================================================

void saveBootstrapIdentityIfNeeded() {
  prefs.begin("hub_cfg", false);

  bool initialized = prefs.getBool("initialized", false);

  if (!initialized || BOOTSTRAP_FORCE_OVERWRITE) {
    Serial.println("[NVS] Writing bootstrap Hub identity.");

    prefs.putString("hubid", BOOTSTRAP_HUB_ID);

    if (!initialized) {
      prefs.putString("ssid", "");
      prefs.putString("pass", "");
    }

    prefs.putBool("initialized", true);
  }

  prefs.end();
}

void loadConfig() {
  saveBootstrapIdentityIfNeeded();

  prefs.begin("hub_cfg", true);

  hubId = prefs.getString("hubid", BOOTSTRAP_HUB_ID);
  wifiSSID = prefs.getString("ssid", "");
  wifiPassword = prefs.getString("pass", "");

  prefs.end();

  Serial.println();
  Serial.println("========== LOADED HUB CONFIG ==========");
  Serial.print("Hub ID: "); Serial.println(hubId);
  Serial.print("Wi-Fi SSID: "); Serial.println(wifiSSID);
  Serial.println("=======================================");
}

void saveConfig(String newHubId, String ssid, String pass) {
  prefs.begin("hub_cfg", false);

  prefs.putString("hubid", newHubId);
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.putBool("initialized", true);

  prefs.end();

  Serial.println("[NVS] Hub config saved.");

  loadConfig();
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

  server.send(200, "text/html", "<h2>Hub OTA started</h2><p>Hub is updating from GitHub.</p>");

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
  html += "<title>Hub OTA Setup</title>";
  html += "<style>";
  html += "body{font-family:Arial;background:#f4f4f4;padding:20px;}";
  html += ".card{background:white;padding:20px;border-radius:12px;max-width:450px;margin:auto;box-shadow:0 2px 8px rgba(0,0,0,.15);}";
  html += "input{width:100%;padding:12px;margin:8px 0;box-sizing:border-box;}";
  html += "button{width:100%;padding:14px;margin-top:10px;background:#111;color:white;border:0;border-radius:6px;font-size:16px;}";
  html += ".info{background:#eee;padding:10px;border-radius:6px;margin-bottom:10px;font-size:14px;}";
  html += "</style></head><body><div class='card'>";

  html += "<h2>Smart Hub OTA Setup</h2>";

  html += "<div class='info'>";
  html += "<b>Hub:</b> " + hubId;
  html += "<br><b>Setup:</b> http://192.168.4.1";
  html += "<br><b>OTA URL:</b><br>";
  html += HUB_BIN_URL;
  html += "</div>";

  html += "<form action='/save' method='POST'>";

  html += "<label>Hub ID</label>";
  html += "<input name='hubid' value='" + hubId + "'>";

  html += "<label>Wi-Fi SSID</label>";
  html += "<input name='ssid' value='" + wifiSSID + "'>";

  html += "<label>Wi-Fi Password</label>";
  html += "<input name='pass' type='password' value='" + wifiPassword + "'>";

  html += "<button type='submit'>Save Config</button>";
  html += "</form>";

  html += "<form action='/update' method='POST'>";
  html += "<button type='submit'>Update Hub From GitHub Now</button>";
  html += "</form>";

  html += "</div></body></html>";

  return html;
}

void handleRoot() {
  server.send(200, "text/html", htmlPage());
}

void handleSave() {
  saveConfig(
    server.arg("hubid"),
    server.arg("ssid"),
    server.arg("pass")
  );

  server.send(200, "text/html", "<h2>Saved</h2><p>Config saved.</p><p>If you changed Hub ID, restart Hub.</p><a href='/'>Back</a>");
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
  server.on("/update", HTTP_POST, startOTAFromPortal);

  server.on("/generate_204", HTTP_GET, handleRoot);
  server.on("/hotspot-detect.html", HTTP_GET, handleRoot);
  server.on("/fwlink", HTTP_GET, handleRoot);

  server.onNotFound(handleCaptive);

  server.begin();
}

// =====================================================
// ESP-NOW
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

void sendAck(const uint8_t *buttonMac, const char *buttonName, uint32_t counter) {
  if (!addPeerIfNeeded(buttonMac)) return;

  SimplePacket ack;

  ack.type = MSG_ACK;
  safeCopy(ack.from, hubId.c_str(), sizeof(ack.from));
  safeCopy(ack.to, buttonName, sizeof(ack.to));
  ack.counter = counter;
  safeCopy(ack.text, "ACK", sizeof(ack.text));

  Serial.println();
  Serial.println("========== HUB SEND ACK ==========");
  Serial.print("To Button: "); Serial.println(buttonName);
  Serial.print("Button MAC: "); Serial.println(macToString(buttonMac));
  Serial.print("Counter: "); Serial.println(counter);
  Serial.println("==================================");

  esp_err_t result = esp_now_send(buttonMac, (uint8_t *)&ack, sizeof(ack));

  if (result == ESP_OK) {
    Serial.println("[ESP-NOW] ACK send request OK.");
  } else {
    Serial.print("[ESP-NOW] ACK send failed. Code: ");
    Serial.println(result);
  }
}

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

void handleReceivedData(const uint8_t *senderMac, const uint8_t *incomingData, int len) {
  if (len != sizeof(SimplePacket)) {
    Serial.println("[ESP-NOW] Invalid packet size.");
    return;
  }

  SimplePacket packet;
  memcpy(&packet, incomingData, sizeof(packet));

  Serial.println();
  Serial.println("========== HUB RECEIVED ESP-NOW ==========");
  Serial.print("From MAC: "); Serial.println(macToString(senderMac));
  Serial.print("Type: "); Serial.println(packet.type);
  Serial.print("From: "); Serial.println(packet.from);
  Serial.print("To: "); Serial.println(packet.to);
  Serial.print("Counter: "); Serial.println(packet.counter);
  Serial.print("Text: "); Serial.println(packet.text);
  Serial.println("==========================================");

  if (strcmp(packet.to, hubId.c_str()) != 0) {
    Serial.println("[ESP-NOW] Packet not for this Hub. Ignored.");
    return;
  }

  lastPacketMs = millis();
  blinkGreen();

  if (packet.type == MSG_HELLO) {
    Serial.println("[HUB] HELLO received.");
  } else if (packet.type == MSG_BUTTON_PRESS) {
    Serial.println("[HUB] Button press event received.");
  } else if (packet.type == MSG_RFID_SCAN) {
    Serial.println("[HUB] RFID scan event received.");
  }

  sendAck(senderMac, packet.from, packet.counter);
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
  Serial.println("ESS SMART HUB - OTA SAFE IDENTITY");
  Serial.println("====================================");
  Serial.print("BOOTSTRAP_FORCE_OVERWRITE: ");
  Serial.println(BOOTSTRAP_FORCE_OVERWRITE ? "true" : "false");
  Serial.print("OTA URL: ");
  Serial.println(HUB_BIN_URL);
  Serial.println("====================================");

  loadConfig();

  setupPortal();

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

  if (millis() - lastPacketMs > 1500) {
    ledBlue();
  }
}