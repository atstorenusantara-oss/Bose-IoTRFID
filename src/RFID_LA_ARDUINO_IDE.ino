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
#define LED_status  4      // GPIO4 -> LED indikator (opsional) // untuk kalau ada RFID
#define BUTTON_PIN  23     // GPIO23 -> Tombol input
#define LED_BUTTON  15     // GPIO15 -> LED tombol (aktif LOW)
#define LED_ACTION  2      // GPIO2  -> LED aksi 10 detik (ini untuk output relay)
#define LED_ACTION_INV 22  // GPIO22 -> LED indikator status (aktif LOW)

const char* WIFI_SSID_DEFAULT = "TUBIS43LT2";
const char* WIFI_PASSWORD_DEFAULT = "12345678";
const char* MQTT_SERVER_DEFAULT = "192.168.0.113";
const int   MQTT_PORT = 1883;
const int   SLOT_NUMBER_DEFAULT = 1;

const char* MQTT_STATUS_TOPIC = "boseh/status";
const char* MQTT_READY_TOPIC = "boseh/ready";
const char* MQTT_CONFIRM_TOPIC = "boseh/stasiun/confirm_open";
const char* MQTT_MAINT_TOPIC = "boseh/maintenance";

String wifiSsidConfig = WIFI_SSID_DEFAULT;
String wifiPasswordConfig = WIFI_PASSWORD_DEFAULT;
String mqttServerConfig = MQTT_SERVER_DEFAULT;
int slotNumberConfig = SLOT_NUMBER_DEFAULT;
bool wifiUseStaticConfig = false;
String wifiStaticIpConfig = "";
String wifiGatewayConfig = "";
String wifiSubnetConfig = "";
String wifiDnsConfig = "";
String mqttControlTopic = "";
String mqttStatusRequestTopic = "";

const char* AP_FALLBACK_SSID_BASE = "BOSEH-Config-";
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
String lastCardRfidTag = "";
bool solenoidState = false;

bool apModeActive = false;
bool relayPin2ActiveHigh = true;
bool relayPin22ActiveHigh = false;

bool parseIPv4(const String& text, IPAddress* outIp) {
  String trimmed = text;
  trimmed.trim();
  if (trimmed.length() == 0) return false;

  IPAddress parsed;
  if (!parsed.fromString(trimmed)) return false;
  if (outIp) *outIp = parsed;
  return true;
}

bool isZeroIp(const IPAddress& ip) {
  return ip[0] == 0 && ip[1] == 0 && ip[2] == 0 && ip[3] == 0;
}

void writeOutputActiveLevel(uint8_t pin, bool active, bool activeHigh) {
  digitalWrite(pin, active ? (activeHigh ? HIGH : LOW) : (activeHigh ? LOW : HIGH));
}

void setActionLed(bool active) {
  writeOutputActiveLevel(LED_ACTION, active, relayPin2ActiveHigh);
}

void setMirrorLed(bool active) {
  writeOutputActiveLevel(LED_ACTION_INV, active, relayPin22ActiveHigh);
}

int clampApOctet(int slotNo) {
  if (slotNo < 2) return 2;
  if (slotNo > 254) return 254;
  return slotNo;
}

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

bool parseJsonBool(const String& msg, const char* key, bool* outValue) {
  if (!outValue) return false;

  String pattern = "\"";
  pattern += key;
  pattern += "\"";

  int idx = msg.indexOf(pattern);
  if (idx < 0) return false;

  idx = msg.indexOf(':', idx);
  if (idx < 0) return false;

  idx++;
  while (idx < (int)msg.length() && msg[idx] == ' ') idx++;

  if (msg.startsWith("true", idx)) {
    *outValue = true;
    return true;
  }
  if (msg.startsWith("false", idx)) {
    *outValue = false;
    return true;
  }
  return false;
}



bool isHexChar(char c) {
  return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F');
}

bool hexPairToByte(char hi, char lo, uint8_t* out) {
  if (!out) return false;

  uint8_t high = 0;
  uint8_t low = 0;

  if (hi >= '0' && hi <= '9') high = (uint8_t)(hi - '0');
  else if (hi >= 'A' && hi <= 'F') high = (uint8_t)(10 + hi - 'A');
  else return false;

  if (lo >= '0' && lo <= '9') low = (uint8_t)(lo - '0');
  else if (lo >= 'A' && lo <= 'F') low = (uint8_t)(10 + lo - 'A');
  else return false;

  *out = (uint8_t)((high << 4) | low);
  return true;
}

bool validateTagWithChecksum(const String& tenHexTag, const String& twoHexChecksum) {
  if (tenHexTag.length() != 10 || twoHexChecksum.length() != 2) return false;

  uint8_t xorValue = 0;
  for (int i = 0; i < 10; i += 2) {
    uint8_t tagByte = 0;
    if (!hexPairToByte(tenHexTag[i], tenHexTag[i + 1], &tagByte)) return false;
    xorValue ^= tagByte;
  }

  uint8_t expectedChecksum = 0;
  if (!hexPairToByte(twoHexChecksum[0], twoHexChecksum[1], &expectedChecksum)) return false;
  return xorValue == expectedChecksum;
}

bool extractValidTagFromRawHex(const String& rawHex, String* outTag) {
  if (!outTag) return false;
  if (rawHex.length() < 12) return false;

  // Format umum: 10 hex tag + 2 hex checksum.
  for (int i = 0; i <= (int)rawHex.length() - 12; i++) {
    String tagCandidate = rawHex.substring(i, i + 10);
    String checksumCandidate = rawHex.substring(i + 10, i + 12);
    if (validateTagWithChecksum(tagCandidate, checksumCandidate)) {
      *outTag = tagCandidate + checksumCandidate;
      return true;
    }
  }

  // Fallback jika urutan terbaca bergeser: 2 checksum + 10 tag.
  // Tetap keluarkan dalam format kanonik 10 tag + 2 checksum.
  for (int i = 0; i <= (int)rawHex.length() - 12; i++) {
    String checksumCandidate = rawHex.substring(i, i + 2);
    String tagCandidate = rawHex.substring(i + 2, i + 12);
    if (validateTagWithChecksum(tagCandidate, checksumCandidate)) {
      *outTag = tagCandidate + checksumCandidate;
      return true;
    }
  }

  return false;
}

void updateMqttControlTopic() {
  mqttControlTopic = "boseh/device/";
  mqttControlTopic += String(slotNumberConfig);
  mqttControlTopic += "/control";
  mqttStatusRequestTopic = "boseh/";
  mqttStatusRequestTopic += String(slotNumberConfig);
}

void loadConfig() {
  prefs.begin("cfg", true);
  wifiSsidConfig = prefs.getString("wifi_ssid", WIFI_SSID_DEFAULT);
  wifiPasswordConfig = prefs.getString("wifi_pass", WIFI_PASSWORD_DEFAULT);
  mqttServerConfig = prefs.getString("mqtt_ip", MQTT_SERVER_DEFAULT);
  slotNumberConfig = prefs.getInt("slot_no", SLOT_NUMBER_DEFAULT);
  wifiUseStaticConfig = prefs.getBool("wifi_static", false);
  wifiStaticIpConfig = prefs.getString("sta_ip", "");
  wifiGatewayConfig = prefs.getString("sta_gw", "");
  wifiSubnetConfig = prefs.getString("sta_sn", "");
  wifiDnsConfig = prefs.getString("sta_dns", "");
  relayPin2ActiveHigh = prefs.getBool("r2_act_hi", true);
  relayPin22ActiveHigh = prefs.getBool("r22_act_hi", false);
  prefs.end();

  if (slotNumberConfig <= 0) slotNumberConfig = SLOT_NUMBER_DEFAULT;
  updateMqttControlTopic();
}

void saveConfig(
  const String& ssid,
  const String& pass,
  const String& mqttIp,
  int slotNo,
  bool useStatic,
  const String& staIp,
  const String& staGw,
  const String& staSn,
  const String& staDns,
  bool pin2ActiveHigh,
  bool pin22ActiveHigh
) {
  prefs.begin("cfg", false);
  prefs.putString("wifi_ssid", ssid);
  prefs.putString("wifi_pass", pass);
  prefs.putString("mqtt_ip", mqttIp);
  prefs.putInt("slot_no", slotNo);
  prefs.putBool("wifi_static", useStatic);
  prefs.putString("sta_ip", staIp);
  prefs.putString("sta_gw", staGw);
  prefs.putString("sta_sn", staSn);
  prefs.putString("sta_dns", staDns);
  prefs.putBool("r2_act_hi", pin2ActiveHigh);
  prefs.putBool("r22_act_hi", pin22ActiveHigh);
  prefs.end();
}

void startFallbackAP() {
  if (apModeActive) return;

  WiFi.mode(WIFI_AP_STA);
  int apOctet = clampApOctet(slotNumberConfig);
  IPAddress apIP(192, 168, 4, apOctet);
  IPAddress apGW(192, 168, 4, apOctet);
  IPAddress apSN(255, 255, 255, 0);
  WiFi.softAPConfig(apIP, apGW, apSN);

  String apSsid = String(AP_FALLBACK_SSID_BASE) + String(slotNumberConfig);
  bool ok = WiFi.softAP(apSsid.c_str(), AP_FALLBACK_PASSWORD);
  if (ok) {
    apModeActive = true;
    Serial.print("AP fallback aktif. SSID: ");
    Serial.println(apSsid);
    Serial.print("AP IP: ");
    Serial.println(WiFi.softAPIP());
  } else {
    Serial.println("Gagal start AP fallback");
  }
}

void stopFallbackAPIfConnected() {
  if (apModeActive && WiFi.status() == WL_CONNECTED) {
    WiFi.softAPdisconnect(true);
    apModeActive = false;
    Serial.println("AP fallback dimatikan (STA sudah terkoneksi)");
  }
}

void connectWiFi(bool allowApFallback) {
  if (WiFi.status() == WL_CONNECTED) return;

  WiFi.mode(WIFI_STA);
  if (wifiUseStaticConfig) {
    IPAddress localIp;
    IPAddress gateway;
    IPAddress subnet;
    IPAddress dns;

    bool validLocal = parseIPv4(wifiStaticIpConfig, &localIp);
    bool validGateway = parseIPv4(wifiGatewayConfig, &gateway);
    bool validSubnet = parseIPv4(wifiSubnetConfig, &subnet);
    bool validDns = true;
    if (wifiDnsConfig.length() > 0) {
      validDns = parseIPv4(wifiDnsConfig, &dns);
    }

    if (!validLocal || !validGateway || !validSubnet || !validDns || isZeroIp(localIp) || isZeroIp(gateway) || isZeroIp(subnet)) {
      Serial.println("Konfigurasi static IP tidak valid, WiFi tidak dijalankan");
      if (allowApFallback) {
        Serial.println("Masuk AP fallback");
        startFallbackAP();
      }
      return;
    }

    bool configOk = false;
    if (wifiDnsConfig.length() > 0) {
      configOk = WiFi.config(localIp, gateway, subnet, dns);
    } else {
      configOk = WiFi.config(localIp, gateway, subnet);
    }

    if (!configOk) {
      Serial.println("Gagal menerapkan static IP");
      if (allowApFallback) {
        Serial.println("Masuk AP fallback");
        startFallbackAP();
      }
      return;
    }

    Serial.print("Mode WiFi: STATIC (");
    Serial.print(localIp);
    Serial.println(")");
  } else {
    Serial.println("Mode WiFi: DHCP");
  }

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
    stopFallbackAPIfConnected();
  } else {
    Serial.println("WiFi connect failed");
    if (allowApFallback) {
      Serial.println("Masuk AP fallback");
      startFallbackAP();
    }
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
      if (mqttControlTopic.length() > 0) {
        mqttClient.subscribe(mqttControlTopic.c_str());
        Serial.print("Subscribed to: ");
        Serial.println(mqttControlTopic);
      }
      if (mqttStatusRequestTopic.length() > 0) {
        mqttClient.subscribe(mqttStatusRequestTopic.c_str());
        Serial.print("Subscribed to: ");
        Serial.println(mqttStatusRequestTopic);
      }
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

  if (WiFi.status() != WL_CONNECTED) connectWiFi(false);
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
  if (WiFi.status() != WL_CONNECTED) connectWiFi(false);
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

void publishMaintenanceStatus() {
  if (WiFi.status() != WL_CONNECTED) connectWiFi(false);
  if (!mqttClient.connected()) connectMQTT();
  mqttClient.loop();

  if (!mqttClient.connected()) {
    Serial.println("MQTT tidak terkoneksi, publish maintenance gagal");
    return;
  }

  String payload;
  payload.reserve(128);
  payload += "{\"slot_number\":";
  payload += String(slotNumberConfig);
  payload += ",\"ip_address\":\"";
  payload += WiFi.localIP().toString();
  payload += "\",\"status\":";
  payload += (mqttClient.connected() ? "true" : "false");
  payload += ",\"solenoid\":";
  payload += (solenoidState ? "true" : "false");
  payload += ",\"rfid_tag\":\"";
  payload += lastCardRfidTag;
  payload += "\"";
  payload += "}";

  bool ok = mqttClient.publish(MQTT_MAINT_TOPIC, payload.c_str());
  Serial.print("Publish ");
  Serial.print(MQTT_MAINT_TOPIC);
  Serial.print(": ");
  Serial.println(payload);
  if (!ok) {
    Serial.println("Publish maintenance gagal");
  }
}

String htmlPage() {
  String html;
  html.reserve(5500);
  html += "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<title>IoT Boseh</title>";
  html += "<style>body{font-family:Arial,sans-serif;background:#f5f7fb;margin:0;padding:20px;}";
  html += ".card{max-width:700px;margin:auto;background:#fff;padding:20px;border-radius:10px;box-shadow:0 4px 20px rgba(0,0,0,.08);}h2{margin-top:0;}";
  html += "label{display:block;margin-top:12px;font-weight:600;}input,select{width:100%;padding:10px;margin-top:6px;border:1px solid #ccc;border-radius:8px;background:#fff;}";
  html += "button{margin-top:16px;padding:10px 16px;border:0;background:#0066cc;color:#fff;border-radius:8px;cursor:pointer;}";
  html += "small{color:#666;} .row{margin-top:12px;padding:10px;background:#f1f5f9;border-radius:8px;}</style></head><body>";
  html += "<div class='card'><h2>Boseh Device Config</h2>";
  html += "<div class='row'><b>WiFi STA IP:</b> ";
  html += WiFi.localIP().toString();
  html += "<br><b>AP IP:</b> ";
  html += WiFi.softAPIP().toString();
  html += "<br><b>MQTT Broker:</b> ";
  html += mqttServerConfig;
  html += "<br><b>WiFi Mode:</b> ";
  html += (wifiUseStaticConfig ? "STATIC IP" : "DHCP");
  html += "</div>";
  html += "<form method='POST' action='/save'>";
  html += "<label>WiFi SSID</label><input name='ssid' value='" + wifiSsidConfig + "' required>";
  html += "<label>WiFi Password</label><input name='password' type='password' value='" + wifiPasswordConfig + "'>";
  html += "<label>MQTT Broker IP</label><input name='mqtt' value='" + mqttServerConfig + "' required>";
  html += "<label>Slot Number</label><input name='slot' type='number' min='1' value='" + String(slotNumberConfig) + "' required>";
  html += "<label>IP Mode</label><select name='ip_mode'>";
  html += wifiUseStaticConfig ? "<option value='dhcp'>DHCP</option><option value='static' selected>STATIC</option>" : "<option value='dhcp' selected>DHCP</option><option value='static'>STATIC</option>";
  html += "</select>";
  html += "<label>Static IP</label><input name='sta_ip' placeholder='contoh: 192.168.1.50' value='" + wifiStaticIpConfig + "'>";
  html += "<label>Gateway</label><input name='sta_gw' placeholder='contoh: 192.168.1.1' value='" + wifiGatewayConfig + "'>";
  html += "<label>Subnet Mask</label><input name='sta_sn' placeholder='contoh: 255.255.255.0' value='" + wifiSubnetConfig + "'>";
  html += "<label>DNS (opsional)</label><input name='sta_dns' placeholder='contoh: 8.8.8.8' value='" + wifiDnsConfig + "'>";
  html += "<small>Jika IP Mode = STATIC, field Static IP/Gateway/Subnet wajib valid.</small>";
  html += "<label>Relay Pin2 Active Level</label><select name='r2_active'>";
  html += relayPin2ActiveHigh ? "<option value='high' selected>HIGH</option><option value='low'>LOW</option>" : "<option value='high'>HIGH</option><option value='low' selected>LOW</option>";
  html += "</select>";
  html += "<label>Relay Pin22 Active Level</label><select name='r22_active'>";
  html += relayPin22ActiveHigh ? "<option value='high' selected>HIGH</option><option value='low'>LOW</option>" : "<option value='high'>HIGH</option><option value='low' selected>LOW</option>";
  html += "</select>";
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
  String ipMode = server.arg("ip_mode");
  String staIp = server.arg("sta_ip");
  String staGw = server.arg("sta_gw");
  String staSn = server.arg("sta_sn");
  String staDns = server.arg("sta_dns");
  String relayPin2Arg = server.arg("r2_active");
  String relayPin22Arg = server.arg("r22_active");

  staIp.trim();
  staGw.trim();
  staSn.trim();
  staDns.trim();

  bool pin2ActiveHigh = !(relayPin2Arg == "low" || relayPin2Arg == "LOW" || relayPin2Arg == "0");
  bool pin22ActiveHigh = !(relayPin22Arg == "low" || relayPin22Arg == "LOW" || relayPin22Arg == "0");
  bool useStaticIp = (ipMode == "static" || ipMode == "STATIC");

  if (ssid.length() == 0 || mqtt.length() == 0 || slot <= 0) {
    server.send(400, "text/plain", "Parameter tidak valid");
    return;
  }

  if (useStaticIp) {
    IPAddress localIp;
    IPAddress gateway;
    IPAddress subnet;
    IPAddress dns;

    bool validLocal = parseIPv4(staIp, &localIp);
    bool validGateway = parseIPv4(staGw, &gateway);
    bool validSubnet = parseIPv4(staSn, &subnet);
    bool validDns = true;
    if (staDns.length() > 0) validDns = parseIPv4(staDns, &dns);

    if (!validLocal || !validGateway || !validSubnet || !validDns || isZeroIp(localIp) || isZeroIp(gateway) || isZeroIp(subnet)) {
      server.send(400, "text/plain", "Static IP/Gateway/Subnet/DNS tidak valid");
      return;
    }
  }

  wifiSsidConfig = ssid;
  wifiPasswordConfig = pass;
  mqttServerConfig = mqtt;
  slotNumberConfig = slot;
  wifiUseStaticConfig = useStaticIp;
  wifiStaticIpConfig = staIp;
  wifiGatewayConfig = staGw;
  wifiSubnetConfig = staSn;
  wifiDnsConfig = staDns;
  relayPin2ActiveHigh = pin2ActiveHigh;
  relayPin22ActiveHigh = pin22ActiveHigh;
  saveConfig(
    wifiSsidConfig,
    wifiPasswordConfig,
    mqttServerConfig,
    slotNumberConfig,
    wifiUseStaticConfig,
    wifiStaticIpConfig,
    wifiGatewayConfig,
    wifiSubnetConfig,
    wifiDnsConfig,
    relayPin2ActiveHigh,
    relayPin22ActiveHigh
  );
  updateMqttControlTopic();
  setActionLed(solenoidState);
  setMirrorLed(solenoidState);

  mqttClient.disconnect();
  WiFi.disconnect(true);
  delay(300);
  connectWiFi(true);
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
  String message;
  message.reserve(length);
  for (unsigned int i = 0; i < length; i++) {
    message += (char)payload[i];
  }

  Serial.print("MQTT masuk [");
  Serial.print(topic);
  Serial.print("]: ");
  Serial.println(message);

  if (strcmp(topic, MQTT_STATUS_TOPIC) == 0) {
    int slot = parseSlotNumber(message);
    String rfidTag = parseJsonString(message, "rfid_tag");
    if (slot < 0) {
      Serial.println("slot_number tidak ditemukan");
      if (buttonWaitActive && !actionActive) {
        setMirrorLed(false);
        solenoidState = false;
      }
      buttonWaitActive = false;
      currentBikeId = "";
      lastStatusRfidTag = "";
      return;
    }

    if (slot == slotNumberConfig) {
      currentBikeId = rfidTag;
      lastStatusRfidTag = rfidTag;
      buttonWaitActive = true;
      // Pin 22 jadi indikator status: nyala saat slot cocok.
      setMirrorLed(true);
      solenoidState = true;
      Serial.print("slot_number cocok: ");
      Serial.println(slot);
      Serial.print("rfid_tag: ");
      Serial.println(currentBikeId);
      Serial.println("Status cocok: LED indikator pin22 ON, menunggu tombol");
    } else {
      if (buttonWaitActive && !actionActive) {
        setMirrorLed(false);
        solenoidState = false;
      }
      buttonWaitActive = false;
      currentBikeId = "";
      lastStatusRfidTag = "";
      Serial.print("slot number tidak sama: ");
      Serial.println(slot);
    }
    return;
  }

  if (mqttControlTopic.length() > 0 && strcmp(topic, mqttControlTopic.c_str()) == 0) {
    int slot = parseSlotNumber(message);
    String command = parseJsonString(message, "command");
    bool value = false;
    bool hasValue = parseJsonBool(message, "value", &value);

    if (slot != slotNumberConfig) {
      Serial.println("slot_number tidak cocok, kontrol diabaikan");
      return;
    }
    if (command != "solenoid" || !hasValue) {
      Serial.println("command/value tidak valid");
      return;
    }

    solenoidState = value;
    setActionLed(solenoidState);
    setMirrorLed(solenoidState);
    if (solenoidState) {
      actionActive = true;
      actionStartMs = millis();
    } else {
      actionActive = false;
    }
    Serial.print("Solenoid set ke: ");
    Serial.println(solenoidState ? "ON" : "OFF");
    return;
  }

  if (mqttStatusRequestTopic.length() > 0 && strcmp(topic, mqttStatusRequestTopic.c_str()) == 0) {
    bool status = false;
    bool hasStatus = parseJsonBool(message, "status", &status);
    if (!hasStatus) {
      Serial.println("status tidak ditemukan, abaikan");
      return;
    }
    if (status) {
      publishMaintenanceStatus();
    } else {
      Serial.println("status=false, maintenance tidak dikirim");
    }
    return;
  }
}

void setup() {
  Serial.begin(115200);
  RFIDSerial.begin(9600, SERIAL_8N1, RX_PIN, -1);
  loadConfig();

  pinMode(RANGE_PIN, INPUT);
  pinMode(LED_status, OUTPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(LED_BUTTON, OUTPUT);
  pinMode(LED_ACTION, OUTPUT);
  pinMode(LED_ACTION_INV, OUTPUT);

  digitalWrite(LED_status, HIGH);
  digitalWrite(LED_BUTTON, HIGH);
  setActionLed(false);
  setMirrorLed(false);
  connectWiFi(true);
  setupWebServer();

  mqttClient.setCallback(mqttCallback);
  connectMQTT();

  Serial.println("\n=== ESP32 + ID-12LA RFID Reader Siap ===");
  Serial.println("Dashboard: buka IP ESP32 di browser");
}

void loop() {
  server.handleClient();

  if (apModeActive) {
    delay(10);
    return;
  }

  if (WiFi.status() != WL_CONNECTED) connectWiFi(false);
  if (!mqttClient.connected()) connectMQTT();
  mqttClient.loop();

  bool inRange = digitalRead(RANGE_PIN) == HIGH;

  if (inRange && !tagDetected) {
    tagDetected = true;
    Serial.println("TAG MASUK RANGE!");
    if (lastCardRfidTag.length() > 0) {
      publishConfirmOpen(lastCardRfidTag, true);
    } else {
      Serial.println("Menunggu RFID terbaca untuk kirim confirm_open status=true");
    }
  }

  if (RFIDSerial.available() > 0) {
    String rawHex = "";
    bool frameEnded = false;
    unsigned long readStartMs = millis();

    // Auto-refresh singkat: tunggu byte lanjutan agar frame tidak terpotong.
    while ((millis() - readStartMs) < 30 && !frameEnded) {
      while (RFIDSerial.available() > 0) {
        char c = RFIDSerial.read();
        if (c >= 'a' && c <= 'f') c = (char)(c - 32);

        if (c == '\r' || c == '\n') {
          frameEnded = true;
          break;
        }

        if (isHexChar(c)) {
          rawHex += c;
        }
      }
      if (!frameEnded) delay(1);
    }

    String validatedTag = "";
    if (extractValidTagFromRawHex(rawHex, &validatedTag)) {
      tagID = validatedTag;
      Serial.print("TAG TERBACA     : ");
      Serial.println(tagID);
      Serial.print("Scan Time       : ");
      Serial.print(millis());
      Serial.println(" ms");
      Serial.println("-----------------------------------");
      digitalWrite(LED_status, LOW);
      lastCardRfidTag = tagID;
      if (inRange) {
        publishConfirmOpen(lastCardRfidTag, true);
      }
    } else if (rawHex.length() > 0) {
      Serial.print("Frame RFID tidak valid/kurang lengkap, retry: ");
      Serial.println(rawHex);
    }
  }

  if (!inRange && tagDetected) {
    tagDetected = false;
    Serial.println("Tag keluar range\n");
    digitalWrite(LED_status, HIGH);
    if (lastCardRfidTag.length() > 0) {
      publishConfirmOpen(lastCardRfidTag, false);
    } else {
      Serial.println("rfid_tag kosong (belum ada kartu terbaca), confirm_open tidak dikirim");
    }
  }

  bool buttonPressed = (digitalRead(BUTTON_PIN) == LOW);
  digitalWrite(LED_BUTTON, buttonPressed ? LOW : HIGH);


  if (actionActive && (millis() - actionStartMs >= 10000)) {
    actionActive = false;
    setActionLed(false);
    setMirrorLed(false);
    solenoidState = false;
    Serial.println("LED aksi OFF");
  }

  if (buttonPressed && !lastButtonState) {
    Serial.println("Tombol ditekan");
  }

  if (buttonWaitActive && !actionActive && buttonPressed && !lastButtonState) {
    buttonWaitActive = false;
    actionActive = true;
    actionStartMs = millis();
    setActionLed(true);
    setMirrorLed(true);
    solenoidState = true;
    String confirmTag = lastCardRfidTag;
    if (confirmTag.length() == 0) {
      confirmTag = currentBikeId;
    }
    if (confirmTag.length() > 0) {
      publishConfirmOpen(confirmTag, false);
    } else {
      Serial.println("rfid_tag kosong, confirm_open tidak dikirim saat tombol ditekan");
    }
    Serial.println("Tombol ditekan: LED aksi ON 10 detik");
    currentBikeId = "";
  }

  lastButtonState = buttonPressed;
  delay(50);
}


