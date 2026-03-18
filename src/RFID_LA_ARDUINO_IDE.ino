// ================================================
// ESP32 + ID-12LA RFID Reader + Web Config + OTA
// ================================================

#include <WiFi.h>
#include <PubSubClient.h>
#include <WebServer.h>
#include <Update.h>
#include <Preferences.h>

#define RX_PIN      16     // GPIO16 -> RFID Pin 9 (D0)
#define RANGE_PIN   17     // GPIO17 -> RFID Pin 6 (Tag in Range)
#define LED_status  4      // GPIO4 -> LED indikator (opsional)
#define BUTTON_PIN  23     // GPIO23 -> Tombol input
#define LED_BUTTON  15     // GPIO15 -> LED tombol (aktif LOW)
#define LED_ACTION  2      // GPIO2  -> LED aksi 10 detik

const char* WIFI_SSID_DEFAULT = "TUBIS43LT2";
const char* WIFI_PASSWORD_DEFAULT = "12345678";
const char* MQTT_SERVER_DEFAULT = "192.168.0.105";
const int   MQTT_PORT = 1883;
const int   SLOT_NUMBER_DEFAULT = 1;

const char* MQTT_STATUS_TOPIC = "boseh/status";
const char* MQTT_READY_TOPIC = "boseh/ready";
const char* MQTT_CONFIRM_TOPIC = "boseh/stasiun/confirm_open";

String wifiSsidConfig = WIFI_SSID_DEFAULT;
String wifiPasswordConfig = WIFI_PASSWORD_DEFAULT;
String mqttServerConfig = MQTT_SERVER_DEFAULT;
int slotNumberConfig = SLOT_NUMBER_DEFAULT;

const char* AP_FALLBACK_SSID = "ESP32-Config";
const char* AP_FALLBACK_PASSWORD = "12345678";

WiFiClient espClient;
PubSubClient mqttClient(espClient);
WebServer server(80);
Preferences prefs;

HardwareSerial RFIDSerial(2);  // UART2 pada ESP32

String tagID = "";
bool tagDetected = false;

bool buttonWaitActive = false;
bool actionActive = false;
bool lastButtonState = false;
unsigned long buttonWaitStartMs = 0;
unsigned long actionStartMs = 0;
String currentBikeId = "";
String lastStatusRfidTag = "";

bool apModeActive = false;

int parseSlotNumber(const String& msg) {
  int idx = msg.indexOf("\"slot_number\"");
  if (idx < 0) return -1;

  idx = msg.indexOf(":", idx);
  if (idx < 0) return -1;

  idx++;
  while (idx < (int)msg.length() && msg[idx] == ' ') idx++;

  int end = idx;
  while (end < (int)msg.length() && (msg[end] >= '0' && msg[end] <= '9')) end++;
  if (end == idx) return -1;

  return msg.substring(idx, end).toInt();
}

String parseJsonString(const String& msg, const char* key) {
  String pattern = "\"";
  pattern += key;
  pattern += "\"";

  int idx = msg.indexOf(pattern);
  if (idx < 0) return "";

  idx = msg.indexOf(':', idx);
  if (idx < 0) return "";

  idx = msg.indexOf('"', idx);
  if (idx < 0) return "";
  idx++;

  int end = msg.indexOf('"', idx);
  if (end < 0) return "";

  return msg.substring(idx, end);
}

void loadConfig() {
  prefs.begin("cfg", true);
  wifiSsidConfig = prefs.getString("wifi_ssid", WIFI_SSID_DEFAULT);
  wifiPasswordConfig = prefs.getString("wifi_pass", WIFI_PASSWORD_DEFAULT);
  mqttServerConfig = prefs.getString("mqtt_ip", MQTT_SERVER_DEFAULT);
  slotNumberConfig = prefs.getInt("slot_no", SLOT_NUMBER_DEFAULT);
  prefs.end();

  if (slotNumberConfig <= 0) slotNumberConfig = SLOT_NUMBER_DEFAULT;
}

void saveConfig(const String& ssid, const String& pass, const String& mqttIp, int slotNo) {
  prefs.begin("cfg", false);
  prefs.putString("wifi_ssid", ssid);
  prefs.putString("wifi_pass", pass);
  prefs.putString("mqtt_ip", mqttIp);
  prefs.putInt("slot_no", slotNo);
  prefs.end();
}

void startFallbackAP() {
  if (apModeActive) return;

  WiFi.mode(WIFI_AP_STA);
  bool ok = WiFi.softAP(AP_FALLBACK_SSID, AP_FALLBACK_PASSWORD);
  if (ok) {
    apModeActive = true;
    Serial.print("AP fallback aktif. SSID: ");
    Serial.println(AP_FALLBACK_SSID);
    Serial.print("AP IP: ");
    Serial.println(WiFi.softAPIP());
  } else {
    Serial.println("Gagal start AP fallback");
  }
}

void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;

  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiSsidConfig.c_str(), wifiPasswordConfig.c_str());
  Serial.print("Connecting WiFi");

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < 15000) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("WiFi connected, IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("WiFi connect failed, masuk AP fallback");
    startFallbackAP();
  }
}

void connectMQTT() {
  if (mqttClient.connected()) return;

  mqttClient.setServer(mqttServerConfig.c_str(), MQTT_PORT);
  Serial.print("Connecting MQTT");

  String clientId = "esp32-rfid-" + String((uint32_t)ESP.getEfuseMac(), HEX);
  unsigned long start = millis();
  while (!mqttClient.connected() && (millis() - start) < 10000) {
    if (mqttClient.connect(clientId.c_str())) {
      Serial.println(" connected");
      mqttClient.subscribe(MQTT_STATUS_TOPIC);
      Serial.print("Subscribed to: ");
      Serial.println(MQTT_STATUS_TOPIC);
    } else {
      Serial.print(".");
      delay(500);
    }
  }

  if (!mqttClient.connected()) {
    Serial.println("\nMQTT connect failed");
  }
}

void publishReadyMessage(const String& bikeId) {
  if (bikeId.length() == 0) {
    Serial.println("bike_id kosong, publish dibatalkan");
    return;
  }

  if (WiFi.status() != WL_CONNECTED) connectWiFi();
  if (!mqttClient.connected()) connectMQTT();
  mqttClient.loop();

  if (!mqttClient.connected()) {
    Serial.println("MQTT tidak terkoneksi, publish ready gagal");
    return;
  }

  String payload = "{\"bike_id\":\"" + bikeId + "\"}";
  bool ok = mqttClient.publish(MQTT_READY_TOPIC, payload.c_str());

  Serial.print("Publish ");
  Serial.print(MQTT_READY_TOPIC);
  Serial.print(": ");
  Serial.println(payload);
  if (!ok) {
    Serial.println("Publish ready gagal");
  }
}

void publishConfirmOpen(const String& rfidTag, bool status) {
  if (WiFi.status() != WL_CONNECTED) connectWiFi();
  if (!mqttClient.connected()) connectMQTT();
  mqttClient.loop();

  if (!mqttClient.connected()) {
    Serial.println("MQTT tidak terkoneksi, publish confirm_open gagal");
    return;
  }

  String payload;
  payload.reserve(96);
  payload += "{\"slot_number\":";
  payload += String(slotNumberConfig);
  payload += ",\"rfid_tag\":\"";
  payload += rfidTag;
  payload += "\",\"status\":";
  payload += (status ? "true" : "false");
  payload += "}";

  bool ok = mqttClient.publish(MQTT_CONFIRM_TOPIC, payload.c_str());
  Serial.print("Publish ");
  Serial.print(MQTT_CONFIRM_TOPIC);
  Serial.print(": ");
  Serial.println(payload);
  if (!ok) {
    Serial.println("Publish confirm_open gagal");
  }
}

String htmlPage() {
  String html;
  html.reserve(3500);
  html += "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<title>ESP32 Dashboard</title>";
  html += "<style>body{font-family:Arial,sans-serif;background:#f5f7fb;margin:0;padding:20px;}";
  html += ".card{max-width:700px;margin:auto;background:#fff;padding:20px;border-radius:10px;box-shadow:0 4px 20px rgba(0,0,0,.08);}h2{margin-top:0;}";
  html += "label{display:block;margin-top:12px;font-weight:600;}input{width:100%;padding:10px;margin-top:6px;border:1px solid #ccc;border-radius:8px;}";
  html += "button{margin-top:16px;padding:10px 16px;border:0;background:#0066cc;color:#fff;border-radius:8px;cursor:pointer;}";
  html += "small{color:#666;} .row{margin-top:12px;padding:10px;background:#f1f5f9;border-radius:8px;}</style></head><body>";
  html += "<div class='card'><h2>ESP32 Config Dashboard</h2>";
  html += "<div class='row'><b>WiFi STA IP:</b> ";
  html += WiFi.localIP().toString();
  html += "<br><b>AP IP:</b> ";
  html += WiFi.softAPIP().toString();
  html += "<br><b>MQTT Broker:</b> ";
  html += mqttServerConfig;
  html += "</div>";
  html += "<form method='POST' action='/save'>";
  html += "<label>WiFi SSID</label><input name='ssid' value='" + wifiSsidConfig + "' required>";
  html += "<label>WiFi Password</label><input name='password' type='password' value='" + wifiPasswordConfig + "'>";
  html += "<label>MQTT Broker IP</label><input name='mqtt' value='" + mqttServerConfig + "' required>";
  html += "<label>Slot Number</label><input name='slot' type='number' min='1' value='" + String(slotNumberConfig) + "' required>";
  html += "<button type='submit'>Simpan Config</button></form>";
  html += "<hr><h3>OTA Firmware Update</h3>";
  html += "<form method='POST' action='/update' enctype='multipart/form-data'>";
  html += "<input type='file' name='firmware' accept='.bin' required>";
  html += "<button type='submit'>Upload OTA</button></form>";
  html += "<p><small>Setelah simpan config, device akan reconnect WiFi/MQTT otomatis. OTA akan reboot saat sukses.</small></p>";
  html += "</div></body></html>";
  return html;
}

void handleRoot() {
  server.send(200, "text/html", htmlPage());
}

void handleSaveConfig() {
  String ssid = server.arg("ssid");
  String pass = server.arg("password");
  String mqtt = server.arg("mqtt");
  int slot = server.arg("slot").toInt();

  if (ssid.length() == 0 || mqtt.length() == 0 || slot <= 0) {
    server.send(400, "text/plain", "Parameter tidak valid");
    return;
  }

  wifiSsidConfig = ssid;
  wifiPasswordConfig = pass;
  mqttServerConfig = mqtt;
  slotNumberConfig = slot;
  saveConfig(wifiSsidConfig, wifiPasswordConfig, mqttServerConfig, slotNumberConfig);

  mqttClient.disconnect();
  WiFi.disconnect(true);
  delay(300);
  connectWiFi();
  connectMQTT();

  server.send(200, "text/html", "<html><body><h3>Config tersimpan.</h3><a href='/'>Kembali</a></body></html>");
}

void handleUpdateUpload() {
  HTTPUpload& upload = server.upload();

  if (upload.status == UPLOAD_FILE_START) {
    Serial.printf("OTA Start: %s\n", upload.filename.c_str());
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
      Update.printError(Serial);
    }
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      Update.printError(Serial);
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (Update.end(true)) {
      Serial.printf("OTA Success: %u bytes\n", upload.totalSize);
    } else {
      Update.printError(Serial);
    }
  }
}

void handleUpdateDone() {
  if (Update.hasError()) {
    server.send(500, "text/plain", "OTA gagal");
    return;
  }

  server.send(200, "text/plain", "OTA sukses, reboot...");
  delay(1000);
  ESP.restart();
}

void setupWebServer() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/save", HTTP_POST, handleSaveConfig);
  server.on("/update", HTTP_POST, handleUpdateDone, handleUpdateUpload);
  server.begin();
  Serial.println("Web dashboard aktif di port 80");
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  if (strcmp(topic, MQTT_STATUS_TOPIC) != 0) return;

  String message;
  message.reserve(length);
  for (unsigned int i = 0; i < length; i++) {
    message += (char)payload[i];
  }

  Serial.print("Status update [");
  Serial.print(topic);
  Serial.print("]: ");
  Serial.println(message);

  int slot = parseSlotNumber(message);
  String rfidTag = parseJsonString(message, "rfid_tag");
  if (slot < 0) {
    Serial.println("slot_number tidak ditemukan");
    buttonWaitActive = false;
    currentBikeId = "";
    lastStatusRfidTag = "";
    return;
  }

  if (slot == slotNumberConfig) {
    currentBikeId = rfidTag;
    lastStatusRfidTag = rfidTag;
    buttonWaitActive = true;
    buttonWaitStartMs = millis();
    Serial.print("slot_number cocok: ");
    Serial.println(slot);
    Serial.print("rfid_tag: ");
    Serial.println(currentBikeId);
    Serial.println("Menunggu tombol selama 60 detik");
  } else {
    buttonWaitActive = false;
    currentBikeId = "";
    lastStatusRfidTag = "";
    Serial.print("slot number tidak sama: ");
    Serial.println(slot);
  }
}

void setup() {
  Serial.begin(115200);
  RFIDSerial.begin(9600, SERIAL_8N1, RX_PIN, -1);

  pinMode(RANGE_PIN, INPUT);
  pinMode(LED_status, OUTPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(LED_BUTTON, OUTPUT);
  pinMode(LED_ACTION, OUTPUT);

  digitalWrite(LED_status, HIGH);
  digitalWrite(LED_BUTTON, HIGH);
  digitalWrite(LED_ACTION, LOW);

  loadConfig();
  connectWiFi();
  setupWebServer();

  mqttClient.setCallback(mqttCallback);
  connectMQTT();

  Serial.println("\n=== ESP32 + ID-12LA RFID Reader Siap ===");
  Serial.println("Dashboard: buka IP ESP32 di browser");
}

void loop() {
  server.handleClient();

  if (WiFi.status() != WL_CONNECTED && !apModeActive) connectWiFi();
  if (!mqttClient.connected()) connectMQTT();
  mqttClient.loop();

  bool inRange = digitalRead(RANGE_PIN) == HIGH;

  if (inRange && !tagDetected) {
    tagDetected = true;
    Serial.println("TAG MASUK RANGE!");
    publishConfirmOpen(lastStatusRfidTag, true);
  }

  if (RFIDSerial.available() > 0) {
    tagID = "";

    while (RFIDSerial.available()) {
      char c = RFIDSerial.read();
      if (c == '\r' || c == '\n') break;
      if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F')) {
        tagID += c;
      }
    }

    if (tagID.length() >= 10) {
      Serial.print("TAG TERBACA     : ");
      Serial.println(tagID);
      Serial.print("Scan Time       : ");
      Serial.print(millis());
      Serial.println(" ms");
      Serial.println("-----------------------------------");
      digitalWrite(LED_status, LOW);
    }
  }

  if (!inRange && tagDetected) {
    tagDetected = false;
    Serial.println("Tag keluar range\n");
    digitalWrite(LED_status, HIGH);
    publishConfirmOpen(lastStatusRfidTag, false);
  }

  bool buttonPressed = (digitalRead(BUTTON_PIN) == LOW);
  digitalWrite(LED_BUTTON, buttonPressed ? LOW : HIGH);

  if (buttonWaitActive && (millis() - buttonWaitStartMs >= 60000)) {
    buttonWaitActive = false;
    currentBikeId = "";
    Serial.println("Timeout tombol 60 detik");
  }

  if (actionActive && (millis() - actionStartMs >= 10000)) {
    actionActive = false;
    digitalWrite(LED_ACTION, LOW);
    Serial.println("LED aksi OFF");
  }

  if (buttonPressed && !lastButtonState) {
    Serial.println("Tombol ditekan");
  }

  if (buttonWaitActive && !actionActive && buttonPressed && !lastButtonState) {
    buttonWaitActive = false;
    actionActive = true;
    actionStartMs = millis();
    digitalWrite(LED_ACTION, HIGH);
    publishReadyMessage(currentBikeId);
    Serial.println("Tombol ditekan: LED aksi ON 10 detik");
    currentBikeId = "";
  }

  lastButtonState = buttonPressed;
  delay(50);
}
