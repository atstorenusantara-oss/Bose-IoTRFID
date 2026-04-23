// ================================================
// ESP32 + ID-12LA RFID Reader + Web Config + OTA
// ================================================

#include <WiFi.h>
#include <PubSubClient.h>
#include <WebServer.h>
#include <Update.h>
#include <Preferences.h>
#include <esp_now.h>
#include "modbus_handler.h"

#define RX_PIN      16     // GPIO16 -> RFID Pin 9 (D0)
#define RANGE_PIN   17     // GPIO17 -> RFID Pin 6 (Tag in Range)
#define LED_status  2      // GPIO2 -> LED indikator (opsional) // untuk kalau ada RFID
#define BUTTON_PIN  23     // GPIO23 -> Tombol input
#define LED_BUTTON  15     // GPIO15 -> LED tombol (aktif LOW)
#define LED_ACTION  4      // GPIO4  -> LED aksi 10 detik (ini untuk output relay)
#define LED_ACTION_INV 22  // GPIO22 -> LED indikator status (aktif LOW)

const char* WIFI_SSID_DEFAULT = "TUBIS43LT2";
const char* WIFI_PASSWORD_DEFAULT = "12345678";
const char* MQTT_SERVER_DEFAULT = "192.168.0.113";
const int   MQTT_PORT = 1883;
const int   SLOT_NUMBER_DEFAULT = 1;
const unsigned long ACTION_DURATION_MS_DEFAULT = 10000;
const unsigned long BUTTON_WAIT_TIMEOUT_MS_DEFAULT = 30000;
const char* DEVICE_ROLE_DEFAULT = "node";
const char* GATEWAY_UPLINK_DEFAULT = "mqtt"; // mqtt | serial | both

const char* MQTT_CONFIRM_TOPIC = "boseh/stasiun/confirm_open";
const char* MQTT_MAINT_TOPIC = "boseh/maintenance";
const uint8_t MODBUS_SLAVE_ID_DEFAULT = 1;

String wifiSsidConfig = WIFI_SSID_DEFAULT;
String wifiPasswordConfig = WIFI_PASSWORD_DEFAULT;
String mqttServerConfig = MQTT_SERVER_DEFAULT;
int slotNumberConfig = SLOT_NUMBER_DEFAULT;
bool wifiUseStaticConfig = false;
String wifiStaticIpConfig = "";
String wifiGatewayConfig = "";
String wifiSubnetConfig = "";
String wifiDnsConfig = "";
String deviceRoleConfig = DEVICE_ROLE_DEFAULT;     // node | gateway
String gatewayUplinkConfig = GATEWAY_UPLINK_DEFAULT;

const char* AP_FALLBACK_SSID_BASE = "BOSEH-Config-";
const char* AP_FALLBACK_PASSWORD = "12345678";
const char* DASHBOARD_PIN = "1221";

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
bool confirmOpenSentInCurrentRange = false;

bool apModeActive = false;
bool relayPin4ActiveHigh = true;
bool relayPin2ActiveHigh = true;
bool relayPin22ActiveHigh = false;
const uint8_t DEBUG_PINS[] = {LED_status, BUTTON_PIN, LED_BUTTON, LED_ACTION, LED_ACTION_INV};
String debugMessage = "";
bool dashboardUnlocked = false;
unsigned long actionDurationMs = ACTION_DURATION_MS_DEFAULT;
unsigned long buttonWaitTimeoutMs = BUTTON_WAIT_TIMEOUT_MS_DEFAULT;
String gatewayMacConfig = "";

const uint8_t ESPNOW_PROTO_VER = 1;
const uint8_t ESPNOW_MSG_STATUS_UPDATE = 1; // node -> gateway (confirm/status)
const uint8_t ESPNOW_MSG_STATUS_ARM = 2;    // gateway -> node (from mqtt status/{slot})
const uint8_t ESPNOW_MSG_CONTROL = 3;       // gateway -> node
const uint8_t ESPNOW_MSG_MAINT_REQ = 4;     // gateway -> node
const uint8_t ESPNOW_MSG_MAINT_RESP = 5;    // node -> gateway
const uint8_t ESPNOW_MSG_ACK = 6;           // both directions
const uint8_t ESPNOW_MSG_HEARTBEAT = 7;     // node -> gateway

struct EspNowPacket {
  uint8_t version;
  uint8_t msgType;
  uint8_t slotNumber;
  uint8_t status;
  uint32_t msgId;
  uint32_t timestampMs;
  uint8_t flags;
  char rfidTag[13];
  char command[12];
  uint8_t value;
  uint16_t reserved;
};

struct EspNowRxItem {
  uint8_t mac[6];
  EspNowPacket pkt;
};

struct SlotPeerState {
  bool valid;
  uint8_t mac[6];
  unsigned long lastSeenMs;
  uint32_t lastInboundMsgId;
  bool deferredActive;
  bool deferredTrackAck;
  unsigned long deferredQueuedMs;
  EspNowPacket deferredPacket;
  bool pendingActive;
  uint32_t pendingMsgId;
  uint8_t pendingRetries;
  unsigned long pendingSentMs;
  EspNowPacket pendingPacket;
};

const int ESPNOW_RX_QUEUE_SIZE = 12;
EspNowRxItem espNowRxQueue[ESPNOW_RX_QUEUE_SIZE];
volatile uint8_t espNowRxHead = 0;
volatile uint8_t espNowRxTail = 0;

struct NodePendingState {
  bool active;
  uint32_t msgId;
  uint8_t retries;
  unsigned long sentMs;
  EspNowPacket packet;
};

const int NODE_TX_QUEUE_SIZE = 8;
EspNowPacket nodeTxQueue[NODE_TX_QUEUE_SIZE];
uint8_t nodeTxHead = 0;
uint8_t nodeTxTail = 0;
NodePendingState nodePending = {};
unsigned long nodeAckTimeoutMs = 700;
uint8_t nodeMaxRetries = 3;

const int MAX_SLOT_INDEX = 64;
SlotPeerState slotPeers[MAX_SLOT_INDEX + 1];
uint32_t gatewayPeerRejectCount = 0;

bool espNowReady = false;
bool gatewayMacValid = false;
uint8_t gatewayMacBytes[6] = {0};
uint32_t nodeTxMsgId = 1;
uint32_t gatewayTxMsgId = 1;
uint32_t nodeLastStatusArmMsgId = 0;
uint32_t nodeLastControlMsgId = 0;
unsigned long lastNodeHeartbeatMs = 0;
unsigned long lastEspNowInitAttemptMs = 0;
String gatewaySerialRxLine = "";

String toLowerTrim(String input) {
  input.trim();
  input.toLowerCase();
  return input;
}

bool isGatewayRole() {
  return deviceRoleConfig == "gateway";
}

bool gatewayUsesMqttUplink() {
  return gatewayUplinkConfig == "mqtt" || gatewayUplinkConfig == "both";
}

bool gatewayUsesSerialUplink() {
  return gatewayUplinkConfig == "serial" || gatewayUplinkConfig == "both";
}

const char* espNowMsgTypeLabel(uint8_t msgType) {
  switch (msgType) {
    case ESPNOW_MSG_STATUS_UPDATE: return "STATUS_UPDATE";
    case ESPNOW_MSG_STATUS_ARM: return "STATUS_ARM";
    case ESPNOW_MSG_CONTROL: return "CONTROL";
    case ESPNOW_MSG_MAINT_REQ: return "MAINT_REQ";
    case ESPNOW_MSG_MAINT_RESP: return "MAINT_RESP";
    case ESPNOW_MSG_ACK: return "ACK";
    case ESPNOW_MSG_HEARTBEAT: return "HEARTBEAT";
    default: return "UNKNOWN";
  }
}

void logNodeInboundPacket(const uint8_t* srcMac, const EspNowPacket& pkt) {
  char macBuf[18] = "??:??:??:??:??:??";
  if (srcMac) {
    snprintf(macBuf, sizeof(macBuf), "%02X:%02X:%02X:%02X:%02X:%02X",
             srcMac[0], srcMac[1], srcMac[2], srcMac[3], srcMac[4], srcMac[5]);
  }
  Serial.print("Node RX from ");
  Serial.print(macBuf);
  Serial.print(" type=");
  Serial.print(espNowMsgTypeLabel(pkt.msgType));
  Serial.print(" msgId=");
  Serial.print(pkt.msgId);
  Serial.print(" slot=");
  Serial.print(pkt.slotNumber);

  if (pkt.msgType == ESPNOW_MSG_STATUS_ARM || pkt.msgType == ESPNOW_MSG_MAINT_REQ) {
    Serial.print(" status=");
    Serial.print(pkt.status);
  }
  if (pkt.msgType == ESPNOW_MSG_STATUS_ARM || pkt.msgType == ESPNOW_MSG_STATUS_UPDATE || pkt.msgType == ESPNOW_MSG_MAINT_RESP) {
    Serial.print(" rfid=");
    Serial.print(String(pkt.rfidTag));
  }
  if (pkt.msgType == ESPNOW_MSG_CONTROL) {
    Serial.print(" command=");
    Serial.print(String(pkt.command));
    Serial.print(" value=");
    Serial.print(pkt.value);
  }
  Serial.println();
}

String macToString(const uint8_t* mac) {
  if (!mac) return "";
  char buf[18];
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(buf);
}

bool macEquals(const uint8_t* a, const uint8_t* b) {
  if (!a || !b) return false;
  return memcmp(a, b, 6) == 0;
}

bool parseMacAddress(const String& text, uint8_t outMac[6]) {
  if (!outMac) return false;
  String s = toLowerTrim(text);
  if (s.length() != 17) return false;
  for (int i = 0; i < 17; i++) {
    if ((i % 3) == 2) {
      if (s[i] != ':') return false;
      continue;
    }
    char c = s[i];
    bool isHex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    if (!isHex) return false;
  }

  for (int i = 0; i < 6; i++) {
    String part = s.substring(i * 3, i * 3 + 2);
    outMac[i] = (uint8_t)strtoul(part.c_str(), nullptr, 16);
  }
  return true;
}

void printDeviceMacInfo() {
  String staMac = WiFi.macAddress();
  String apMac = WiFi.softAPmacAddress();

  Serial.println("=== Device MAC Info ===");
  Serial.println("MAC STA (Node/Gateway): " + staMac);
  Serial.println("MAC AP  (SoftAP)      : " + apMac);
  Serial.println("=======================");
}

uint32_t nextNodeMsgId() {
  return nodeTxMsgId++;
}

uint32_t nextGatewayMsgId() {
  return gatewayTxMsgId++;
}

int safeSlotIndex(uint8_t slotNo) {
  if (slotNo == 0 || slotNo > MAX_SLOT_INDEX) return -1;
  return (int)slotNo;
}

bool espNowEnsurePeer(const uint8_t* mac) {
  if (!espNowReady || !mac) return false;
  if (esp_now_is_peer_exist(mac)) return true;

  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, mac, 6);
  peerInfo.channel = 0;
  peerInfo.encrypt = false;
  return esp_now_add_peer(&peerInfo) == ESP_OK;
}

void espNowQueuePush(const uint8_t* mac, const EspNowPacket* pkt) {
  if (!mac || !pkt) return;
  uint8_t next = (uint8_t)((espNowRxHead + 1) % ESPNOW_RX_QUEUE_SIZE);
  if (next == espNowRxTail) return; // queue full, drop paket paling baru
  memcpy(espNowRxQueue[espNowRxHead].mac, mac, 6);
  espNowRxQueue[espNowRxHead].pkt = *pkt;
  espNowRxHead = next;
}

bool espNowQueuePop(EspNowRxItem* out) {
  if (!out) return false;
  if (espNowRxTail == espNowRxHead) return false;
  *out = espNowRxQueue[espNowRxTail];
  espNowRxTail = (uint8_t)((espNowRxTail + 1) % ESPNOW_RX_QUEUE_SIZE);
  return true;
}

bool nodeTxQueuePush(const EspNowPacket* pkt) {
  if (!pkt) return false;
  uint8_t next = (uint8_t)((nodeTxHead + 1) % NODE_TX_QUEUE_SIZE);
  if (next == nodeTxTail) return false;
  nodeTxQueue[nodeTxHead] = *pkt;
  nodeTxHead = next;
  return true;
}

bool nodeTxQueuePop(EspNowPacket* out) {
  if (!out) return false;
  if (nodeTxTail == nodeTxHead) return false;
  *out = nodeTxQueue[nodeTxTail];
  nodeTxTail = (uint8_t)((nodeTxTail + 1) % NODE_TX_QUEUE_SIZE);
  return true;
}

void nodeQueueReliablePacket(const EspNowPacket* pkt) {
  if (!pkt) return;
  if (!nodeTxQueuePush(pkt)) {
    Serial.println("Node TX queue penuh, paket dibuang");
  }
}

void nodeStartPending(const EspNowPacket* pkt) {
  if (!pkt) return;
  nodePending.active = true;
  nodePending.msgId = pkt->msgId;
  nodePending.retries = 0;
  nodePending.sentMs = millis();
  nodePending.packet = *pkt;
}

void nodeKickReliableTx() {
  if (isGatewayRole()) return;
  if (!gatewayMacValid || !espNowReady) return;
  if (nodePending.active) return;

  EspNowPacket pkt = {};
  if (!nodeTxQueuePop(&pkt)) return;
  nodeStartPending(&pkt);
  espNowSendPacket(gatewayMacBytes, pkt);
}

void nodeHandlePendingRetries() {
  if (isGatewayRole()) return;
  if (!nodePending.active) return;
  unsigned long now = millis();
  if ((now - nodePending.sentMs) < nodeAckTimeoutMs) return;

  if (nodePending.retries >= nodeMaxRetries) {
    nodePending.active = false;
    Serial.print("Node ACK timeout msgId=");
    Serial.println(nodePending.msgId);
    nodeKickReliableTx();
    return;
  }

  nodePending.retries++;
  nodePending.sentMs = now;
  espNowSendPacket(gatewayMacBytes, nodePending.packet);
}

void onEspNowRecvRaw(const uint8_t* mac, const uint8_t* incomingData, int len) {
  if (!mac || !incomingData) return;
  if (len != (int)sizeof(EspNowPacket)) return;

  EspNowPacket pkt;
  memcpy(&pkt, incomingData, sizeof(EspNowPacket));
  if (pkt.version != ESPNOW_PROTO_VER) return;
  espNowQueuePush(mac, &pkt);
}

void onEspNowRecv(const uint8_t* mac, const uint8_t* incomingData, int len) {
  onEspNowRecvRaw(mac, incomingData, len);
}

void onEspNowSent(const uint8_t* mac, esp_now_send_status_t status) {
  (void)mac;
  (void)status;
}

bool initEspNowStack() {
  if (espNowReady) return true;
  lastEspNowInitAttemptMs = millis();
  WiFi.mode(WIFI_STA);
  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init gagal");
    return false;
  }
  esp_now_register_recv_cb(onEspNowRecv);
  esp_now_register_send_cb(onEspNowSent);
  espNowReady = true;
  Serial.println("ESP-NOW init sukses");
  return true;
}

bool espNowSendPacket(const uint8_t* mac, const EspNowPacket& pkt) {
  if (!espNowReady) {
    if (!initEspNowStack()) return false;
  }
  if (!espNowEnsurePeer(mac)) return false;
  esp_err_t r = esp_now_send(mac, (const uint8_t*)&pkt, sizeof(EspNowPacket));
  if (r == ESP_ERR_ESPNOW_NOT_INIT) {
    espNowReady = false;
    Serial.println("ESP-NOW belum init saat send, akan re-init");
  }
  return r == ESP_OK;
}

void buildAckPacket(EspNowPacket* ack, uint8_t slotNo, uint32_t msgId) {
  if (!ack) return;
  memset(ack, 0, sizeof(EspNowPacket));
  ack->version = ESPNOW_PROTO_VER;
  ack->msgType = ESPNOW_MSG_ACK;
  ack->slotNumber = slotNo;
  ack->msgId = msgId;
  ack->timestampMs = millis();
}

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

bool isInputOnlyPin(uint8_t pin) {
  return pin == 34 || pin == 35 || pin == 36 || pin == 39;
}

bool isDebugPinAllowed(int pin) {
  if (pin < 0 || pin > 39) return false;
  for (size_t i = 0; i < (sizeof(DEBUG_PINS) / sizeof(DEBUG_PINS[0])); i++) {
    if ((int)DEBUG_PINS[i] == pin) return true;
  }
  return false;
}

String debugPinLabel(uint8_t pin) {
  if (pin == LED_status) return "LED_status";
  if (pin == BUTTON_PIN) return "BUTTON_PIN";
  if (pin == LED_BUTTON) return "LED_BUTTON";
  if (pin == LED_ACTION) return "LED_ACTION";
  if (pin == LED_ACTION_INV) return "LED_ACTION_INV";
  return "-";
}

String debugPinModeLabel(uint8_t pin) {
  if (isInputOnlyPin(pin)) return "INPUT_ONLY";
  if (pin == BUTTON_PIN) return "INPUT_PULLUP";
  if (pin == LED_status || pin == LED_BUTTON || pin == LED_ACTION || pin == LED_ACTION_INV) return "OUTPUT";
  return "UNKNOWN";
}

String buildDebugPinsSection() {
  String html;
  html.reserve(2200);

  html += "<hr><h3 id='debug'>Debug GPIO Pin Yang Digunakan</h3>";
  if (debugMessage.length() > 0) {
    html += "<div class='row'><b>Debug:</b> ";
    html += debugMessage;
    html += "</div>";
  }

  html += "<p><small>Catatan: aksi debug pada pin yang dipakai aplikasi bisa tertimpa logika loop normal.</small></p>";
  html += "<div style='overflow-x:auto;'><table class='pins'>";
  html += "<tr><th>GPIO</th><th>Level</th><th>Mode</th><th>Fungsi</th><th>Aksi</th></tr>";

  for (size_t i = 0; i < (sizeof(DEBUG_PINS) / sizeof(DEBUG_PINS[0])); i++) {
    uint8_t pin = DEBUG_PINS[i];
    int level = digitalRead(pin);

    html += (pin == BUTTON_PIN) ? "<tr class='button-pin'><td>" : "<tr><td>";
    html += String(pin);
    html += "</td><td>";
    html += (level == HIGH ? "HIGH" : "LOW");
    html += "</td><td>";
    html += debugPinModeLabel(pin);
    html += "</td><td>";
    html += debugPinLabel(pin);
    html += "</td><td>";

    html += "<form class='pin-actions' method='POST' action='/debug/pin'>";
    html += "<input type='hidden' name='pin' value='";
    html += String(pin);
    html += "'>";
    html += "<button type='submit' name='action' value='read'>READ</button>";

    if (!isInputOnlyPin(pin)) {
      html += "<button type='submit' name='action' value='high'>HIGH</button>";
      html += "<button type='submit' name='action' value='low'>LOW</button>";
    }

    html += "</form></td></tr>";
  }

  html += "</table></div>";
  return html;
}

String buildRfidCardSection() {
  String html;
  html.reserve(220);
  html += "<div class='row'><b>RFID Card:</b> ";
  if (tagDetected && lastCardRfidTag.length() > 0) {
    html += lastCardRfidTag;
  }
  html += "</div>";
  return html;
}

void writeOutputActiveLevel(uint8_t pin, bool active, bool activeHigh) {
  digitalWrite(pin, active ? (activeHigh ? HIGH : LOW) : (activeHigh ? LOW : HIGH));
}

void setActionLed(bool active) {
  writeOutputActiveLevel(LED_ACTION, active, relayPin4ActiveHigh);
}

void setStatusLed(bool active) {
  writeOutputActiveLevel(LED_status, active, relayPin2ActiveHigh);
}

void setMirrorLed(bool active) {
  writeOutputActiveLevel(LED_ACTION_INV, active, relayPin22ActiveHigh);
}

int clampApOctet(int slotNo) {
  if (slotNo < 2) return 2;
  if (slotNo > 254) return 254;
  return slotNo;
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

bool parseJsonObjectRaw(const String& msg, const char* key, String* outObject) {
  if (!outObject) return false;

  String pattern = "\"";
  pattern += key;
  pattern += "\"";

  int idx = msg.indexOf(pattern);
  if (idx < 0) return false;
  idx = msg.indexOf(':', idx);
  if (idx < 0) return false;
  idx++;

  while (idx < (int)msg.length() && (msg[idx] == ' ' || msg[idx] == '\t')) idx++;
  if (idx >= (int)msg.length() || msg[idx] != '{') return false;

  int start = idx;
  int depth = 0;
  bool inString = false;
  bool escaped = false;

  for (int i = idx; i < (int)msg.length(); i++) {
    char c = msg[i];
    if (inString) {
      if (escaped) {
        escaped = false;
      } else if (c == '\\') {
        escaped = true;
      } else if (c == '"') {
        inString = false;
      }
      continue;
    }

    if (c == '"') {
      inString = true;
      continue;
    }
    if (c == '{') depth++;
    if (c == '}') {
      depth--;
      if (depth == 0) {
        *outObject = msg.substring(start, i + 1);
        return true;
      }
    }
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
  deviceRoleConfig = toLowerTrim(prefs.getString("dev_role", DEVICE_ROLE_DEFAULT));
  gatewayUplinkConfig = toLowerTrim(prefs.getString("gw_uplink", GATEWAY_UPLINK_DEFAULT));
  gatewayMacConfig = toLowerTrim(prefs.getString("gw_mac", ""));
  actionDurationMs = prefs.getULong("act_ms", ACTION_DURATION_MS_DEFAULT);
  buttonWaitTimeoutMs = prefs.getULong("wait_ms", BUTTON_WAIT_TIMEOUT_MS_DEFAULT);
  relayPin4ActiveHigh = prefs.getBool("r4_act_hi", true);
  relayPin2ActiveHigh = prefs.getBool("r2_act_hi", true);
  relayPin22ActiveHigh = prefs.getBool("r22_act_hi", false);
  prefs.end();

  if (deviceRoleConfig != "node" && deviceRoleConfig != "gateway") deviceRoleConfig = DEVICE_ROLE_DEFAULT;
  if (gatewayUplinkConfig != "mqtt" && gatewayUplinkConfig != "serial" && gatewayUplinkConfig != "both") {
    gatewayUplinkConfig = GATEWAY_UPLINK_DEFAULT;
  }
  if (slotNumberConfig <= 0) slotNumberConfig = SLOT_NUMBER_DEFAULT;
}

int parseTrailingSlotFromTopic(const char* topic, const char* prefix) {
  if (!topic || !prefix) return -1;
  size_t pLen = strlen(prefix);
  if (strncmp(topic, prefix, pLen) != 0) return -1;

  const char* s = topic + pLen;
  if (*s == '\0') return -1;
  int value = 0;
  while (*s >= '0' && *s <= '9') {
    value = value * 10 + (*s - '0');
    s++;
  }
  if (*s != '\0') return -1;
  return value;
}

int parseSlotFromControlTopic(const char* topic) {
  const char* prefix = "boseh/device/";
  size_t pLen = strlen(prefix);
  if (!topic || strncmp(topic, prefix, pLen) != 0) return -1;
  const char* s = topic + pLen;
  int value = 0;
  if (!(*s >= '0' && *s <= '9')) return -1;
  while (*s >= '0' && *s <= '9') {
    value = value * 10 + (*s - '0');
    s++;
  }
  if (strcmp(s, "/control") != 0) return -1;
  return value;
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
  const String& role,
  const String& gatewayUplink,
  const String& gatewayMac,
  unsigned long actionMs,
  unsigned long waitMs,
  bool pin4ActiveHigh,
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
  prefs.putString("dev_role", role);
  prefs.putString("gw_uplink", gatewayUplink);
  prefs.putString("gw_mac", gatewayMac);
  prefs.putULong("act_ms", actionMs);
  prefs.putULong("wait_ms", waitMs);
  prefs.putBool("r4_act_hi", pin4ActiveHigh);
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
      if (isGatewayRole()) {
        mqttClient.subscribe("boseh/status/+");
        mqttClient.subscribe("boseh/device/+/control");
        mqttClient.subscribe("boseh/+");
        Serial.println("Subscribed gateway: boseh/status/+, boseh/device/+/control, boseh/+");
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

void publishConfirmOpen(const String& rfidTag, bool status) {
  if (!isGatewayRole()) {
    if (!gatewayMacValid || !espNowReady) return;
    EspNowPacket pkt = {};
    pkt.version = ESPNOW_PROTO_VER;
    pkt.msgType = ESPNOW_MSG_STATUS_UPDATE;
    pkt.slotNumber = (uint8_t)slotNumberConfig;
    pkt.status = status ? 1 : 0;
    pkt.msgId = nextNodeMsgId();
    pkt.timestampMs = millis();
    rfidTag.toCharArray(pkt.rfidTag, sizeof(pkt.rfidTag));
    nodeQueueReliablePacket(&pkt);
    nodeKickReliableTx();
    return;
  }

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
  if (!isGatewayRole()) {
    if (!gatewayMacValid || !espNowReady) return;
    EspNowPacket pkt = {};
    pkt.version = ESPNOW_PROTO_VER;
    pkt.msgType = ESPNOW_MSG_MAINT_RESP;
    pkt.slotNumber = (uint8_t)slotNumberConfig;
    pkt.status = 1;
    pkt.msgId = nextNodeMsgId();
    pkt.timestampMs = millis();
    pkt.flags = solenoidState ? 0x01 : 0x00;
    lastCardRfidTag.toCharArray(pkt.rfidTag, sizeof(pkt.rfidTag));
    nodeQueueReliablePacket(&pkt);
    nodeKickReliableTx();
    return;
  }

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
  html.reserve(7000);
  html += "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<title>IoT Boseh</title>";
  html += "<style>body{font-family:Arial,sans-serif;background:#f5f7fb;margin:0;padding:20px;}";
  html += ".card{max-width:700px;margin:auto;background:#fff;padding:20px;border-radius:10px;box-shadow:0 4px 20px rgba(0,0,0,.08);}h2{margin-top:0;}";
  html += "label{display:block;margin-top:12px;font-weight:600;}input,select{width:100%;padding:10px;margin-top:6px;border:1px solid #ccc;border-radius:8px;background:#fff;}";
  html += "button{margin-top:16px;padding:10px 16px;border:0;background:#0066cc;color:#fff;border-radius:8px;cursor:pointer;}";
  html += "a.btn-link{display:inline-block;margin-top:12px;padding:10px 16px;background:#0f766e;color:#fff;text-decoration:none;border-radius:8px;font-size:14px;}";
  html += "small{color:#666;} .row{margin-top:12px;padding:10px;background:#f1f5f9;border-radius:8px;}";
  html += "</style></head><body>";
  html += "<div class='card'><h2>Boseh Device Config</h2>";
  html += "<div class='row'><b>WiFi STA IP:</b> ";
  html += WiFi.localIP().toString();
  html += "<br><b>MAC STA (Node/Gateway):</b> ";
  html += WiFi.macAddress();
  html += "<br><b>AP IP:</b> ";
  html += WiFi.softAPIP().toString();
  html += "<br><b>MQTT Broker:</b> ";
  html += mqttServerConfig;
  html += "<br><b>WiFi Mode:</b> ";
  html += (wifiUseStaticConfig ? "STATIC IP" : "DHCP");
  html += "<br><b>Device Role:</b> ";
  html += deviceRoleConfig;
  html += "<br><b>Node Comm Mode:</b> ESP-NOW (fixed)";
  html += "<br><b>Gateway Uplink:</b> ";
  html += gatewayUplinkConfig;
  html += "</div>";
  html += "<form method='POST' action='/save'>";
  html += "<label>Device Role</label><select id='device_role_select' name='device_role'>";
  html += (deviceRoleConfig == "gateway") ? "<option value='node'>NODE</option><option value='gateway' selected>GATEWAY</option>" : "<option value='node' selected>NODE</option><option value='gateway'>GATEWAY</option>";
  html += "</select>";
  html += "<div id='gateway_uplink_group'>";
  html += "<label>Gateway Uplink</label><select id='gw_uplink_select' name='gw_uplink'>";
  if (gatewayUplinkConfig == "serial") {
    html += "<option value='mqtt'>MQTT</option><option value='serial' selected>SERIAL</option><option value='both'>BOTH</option>";
  } else if (gatewayUplinkConfig == "both") {
    html += "<option value='mqtt'>MQTT</option><option value='serial'>SERIAL</option><option value='both' selected>BOTH</option>";
  } else {
    html += "<option value='mqtt' selected>MQTT</option><option value='serial'>SERIAL</option><option value='both'>BOTH</option>";
  }
  html += "</select>";
  html += "<small>Gateway uplink menentukan jalur komunikasi Gateway ke PC/Backend.</small>";
  html += "</div>";
  html += "<label>Gateway MAC (untuk NODE mode ESP-NOW)</label><input name='gw_mac' placeholder='contoh: 24:6F:28:AA:BB:CC' value='" + gatewayMacConfig + "'>";
  html += "<small>Role GATEWAY menjalankan bridge ESP-NOW dengan uplink sesuai pilihan. Role NODE selalu menggunakan ESP-NOW.</small>";
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
  html += "<label>Timeout Tunggu Tombol Setelah Status (detik)</label><input name='wait_sec' type='number' min='1' max='300' value='" + String(buttonWaitTimeoutMs / 1000) + "' required>";
  html += "<label>Durasi Aksi Relay (detik)</label><input name='action_sec' type='number' min='1' max='120' value='" + String(actionDurationMs / 1000) + "' required>";
  html += "<label>Relay Pin4 Active Level (LED_ACTION)</label><select name='r4_active'>";
  html += relayPin4ActiveHigh ? "<option value='high' selected>HIGH</option><option value='low'>LOW</option>" : "<option value='high'>HIGH</option><option value='low' selected>LOW</option>";
  html += "</select>";
  html += "<label>Relay Pin2 Active Level (LED_status)</label><select name='r2_active'>";
  html += relayPin2ActiveHigh ? "<option value='high' selected>HIGH</option><option value='low'>LOW</option>" : "<option value='high'>HIGH</option><option value='low' selected>LOW</option>";
  html += "</select>";
  html += "<label>Relay Pin22 Active Level (LED_ACTION_INV)</label><select name='r22_active'>";
  html += relayPin22ActiveHigh ? "<option value='high' selected>HIGH</option><option value='low'>LOW</option>" : "<option value='high'>HIGH</option><option value='low' selected>LOW</option>";
  html += "</select>";
  html += "<button type='submit'>Simpan Config</button></form>";
  html += "<hr><h3>OTA Firmware Update</h3>";
  html += "<form method='POST' action='/update' enctype='multipart/form-data'>";
  html += "<input type='file' name='firmware' accept='.bin' required>";
  html += "<button type='submit'>Upload OTA</button></form>";
  html += "<p><small>Setelah simpan config, device akan reconnect layanan sesuai role/uplink. OTA akan reboot saat sukses.</small></p>";
  html += "<a class='btn-link' href='/debug'>Buka Halaman Debug GPIO</a>";
  html += "<script>";
  html += "function syncRoleSections(){";
  html += "var role=document.getElementById('device_role_select');";
  html += "var gwGroup=document.getElementById('gateway_uplink_group');";
  html += "if(!role||!gwGroup)return;";
  html += "var isGateway=(role.value==='gateway');";
  html += "gwGroup.style.display=isGateway?'block':'none';";
  html += "}";
  html += "document.addEventListener('DOMContentLoaded',function(){";
  html += "var role=document.getElementById('device_role_select');";
  html += "if(role){role.addEventListener('change',syncRoleSections);}syncRoleSections();";
  html += "});";
  html += "</script>";
  html += "</div></body></html>";
  return html;
}

String htmlPinPage(const String& errorMessage) {
  String html;
  html.reserve(1800);
  html += "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<title>IoT Boseh - PIN</title>";
  html += "<style>body{font-family:Arial,sans-serif;background:#f5f7fb;margin:0;padding:20px;}";
  html += ".card{max-width:420px;margin:40px auto;background:#fff;padding:24px;border-radius:10px;box-shadow:0 4px 20px rgba(0,0,0,.08);}";
  html += "h2{margin-top:0;}label{display:block;margin-top:12px;font-weight:600;}input{width:100%;padding:12px;margin-top:6px;border:1px solid #ccc;border-radius:8px;background:#fff;font-size:20px;letter-spacing:6px;text-align:center;}";
  html += "button{margin-top:16px;padding:10px 16px;border:0;background:#0066cc;color:#fff;border-radius:8px;cursor:pointer;}";
  html += ".err{margin-top:12px;background:#fee2e2;color:#991b1b;padding:10px;border-radius:8px;font-size:14px;}</style></head><body>";
  html += "<div class='card'><h2>Boseh Dashboard Lock</h2>";
  html += "<form method='POST' action='/unlock'>";
  html += "<label>Masukkan PIN 4 Digit</label>";
  html += "<input name='pin' type='password' inputmode='numeric' pattern='[0-9]{4}' maxlength='4' minlength='4' required autofocus>";
  html += "<button type='submit'>Buka Dashboard</button></form>";
  if (errorMessage.length() > 0) {
    html += "<div class='err'>";
    html += errorMessage;
    html += "</div>";
  }
  html += "</div></body></html>";
  return html;
}

String htmlDebugPage() {
  String html;
  html.reserve(6500);
  html += "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<title>IoT Boseh - Debug GPIO</title>";
  html += "<style>body{font-family:Arial,sans-serif;background:#f5f7fb;margin:0;padding:20px;}";
  html += ".card{max-width:980px;margin:auto;background:#fff;padding:20px;border-radius:10px;box-shadow:0 4px 20px rgba(0,0,0,.08);}h2{margin-top:0;}";
  html += "a.btn-link{display:inline-block;margin:8px 8px 0 0;padding:9px 14px;background:#0066cc;color:#fff;text-decoration:none;border-radius:8px;font-size:14px;}";
  html += "small{color:#666;} .row{margin-top:12px;padding:10px;background:#f1f5f9;border-radius:8px;}";
  html += "table.pins{width:100%;border-collapse:collapse;font-size:13px;}table.pins th,table.pins td{border:1px solid #d7dce3;padding:8px;text-align:left;vertical-align:top;}";
  html += "table.pins th{background:#eef2f7;}table.pins tr.button-pin td{background:#fff6d6;font-weight:600;}form.pin-actions{display:flex;flex-wrap:wrap;gap:6px;}";
  html += "button{padding:8px 12px;border:0;background:#0066cc;color:#fff;border-radius:8px;cursor:pointer;font-size:12px;}";
  html += "</style></head><body>";
  html += "<div class='card'><h2>Debug GPIO Pin Yang Digunakan</h2>";
  html += "<a class='btn-link' href='/'>Kembali Ke Halaman Utama</a>";
  html += "<a class='btn-link' href='/debug'>Refresh Debug</a>";
  html += buildRfidCardSection();
  html += buildDebugPinsSection();
  html += "</div></body></html>";
  return html;
}

bool requireDashboardUnlocked() {
  if (dashboardUnlocked) return true;
  server.sendHeader("Location", "/");
  server.send(303);
  return false;
}

void handleRoot() {
  if (!dashboardUnlocked) {
    server.send(200, "text/html", htmlPinPage(""));
    return;
  }
  server.send(200, "text/html", htmlPage());
}

void handleUnlock() {
  String pin = server.arg("pin");
  if (pin == DASHBOARD_PIN) {
    dashboardUnlocked = true;
    server.sendHeader("Location", "/");
    server.send(303);
    return;
  }
  server.send(200, "text/html", htmlPinPage("PIN salah, coba lagi."));
}

void handleDebugPage() {
  if (!requireDashboardUnlocked()) return;
  server.send(200, "text/html", htmlDebugPage());
}

void handleSaveConfig() {
  if (!requireDashboardUnlocked()) return;
  String ssid = server.arg("ssid");
  String pass = server.arg("password");
  String mqtt = server.arg("mqtt");
  int slot = server.arg("slot").toInt();
  String deviceRoleArg = toLowerTrim(server.arg("device_role"));
  String gatewayUplinkArg = toLowerTrim(server.arg("gw_uplink"));
  String gatewayMacArg = toLowerTrim(server.arg("gw_mac"));
  String ipMode = server.arg("ip_mode");
  String staIp = server.arg("sta_ip");
  String staGw = server.arg("sta_gw");
  String staSn = server.arg("sta_sn");
  String staDns = server.arg("sta_dns");
  int waitSec = server.arg("wait_sec").toInt();
  int actionSec = server.arg("action_sec").toInt();
  String relayPin4Arg = server.arg("r4_active");
  String relayPin2Arg = server.arg("r2_active");
  String relayPin22Arg = server.arg("r22_active");

  staIp.trim();
  staGw.trim();
  staSn.trim();
  staDns.trim();
  gatewayMacArg.trim();

  bool pin4ActiveHigh = !(relayPin4Arg == "low" || relayPin4Arg == "LOW" || relayPin4Arg == "0");
  bool pin2ActiveHigh = !(relayPin2Arg == "low" || relayPin2Arg == "LOW" || relayPin2Arg == "0");
  bool pin22ActiveHigh = !(relayPin22Arg == "low" || relayPin22Arg == "LOW" || relayPin22Arg == "0");
  bool useStaticIp = (ipMode == "static" || ipMode == "STATIC");

  if (ssid.length() == 0 || mqtt.length() == 0 || slot <= 0 || actionSec < 1 || actionSec > 120 || waitSec < 1 || waitSec > 300) {
    server.send(400, "text/plain", "Parameter tidak valid");
    return;
  }
  if (deviceRoleArg != "node" && deviceRoleArg != "gateway") {
    server.send(400, "text/plain", "device_role tidak valid");
    return;
  }
  if (gatewayUplinkArg != "mqtt" && gatewayUplinkArg != "serial" && gatewayUplinkArg != "both") {
    server.send(400, "text/plain", "gw_uplink tidak valid");
    return;
  }
  if (deviceRoleArg == "node") {
    uint8_t tmpMac[6];
    if (!parseMacAddress(gatewayMacArg, tmpMac)) {
      server.send(400, "text/plain", "Gateway MAC tidak valid");
      return;
    }
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
  deviceRoleConfig = deviceRoleArg;
  gatewayUplinkConfig = gatewayUplinkArg;
  gatewayMacConfig = gatewayMacArg;
  buttonWaitTimeoutMs = (unsigned long)waitSec * 1000UL;
  actionDurationMs = (unsigned long)actionSec * 1000UL;
  relayPin4ActiveHigh = pin4ActiveHigh;
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
    deviceRoleConfig,
    gatewayUplinkConfig,
    gatewayMacConfig,
    actionDurationMs,
    buttonWaitTimeoutMs,
    relayPin4ActiveHigh,
    relayPin2ActiveHigh,
    relayPin22ActiveHigh
  );
  setStatusLed(tagDetected);
  setActionLed(solenoidState);
  setMirrorLed(solenoidState);

  mqttClient.disconnect();
  WiFi.disconnect(true);
  delay(300);
  connectWiFi(true);
  if (isGatewayRole() && gatewayUsesMqttUplink()) {
    connectMQTT();
  }

  server.send(200, "text/html", "<html><body><h3>Config tersimpan.</h3><a href='/'>Kembali</a></body></html>");
}

void handleDebugPin() {
  if (!requireDashboardUnlocked()) return;
  int pin = server.arg("pin").toInt();
  String action = server.arg("action");

  if (!isDebugPinAllowed(pin)) {
    debugMessage = "GPIO tidak diizinkan untuk debug";
    server.sendHeader("Location", "/debug#debug");
    server.send(303);
    return;
  }

  if (action == "read") {
    int level = digitalRead(pin);
    debugMessage = "GPIO" + String(pin) + " = " + (level == HIGH ? "HIGH" : "LOW");
  } else if (action == "high") {
    if (isInputOnlyPin((uint8_t)pin)) {
      debugMessage = "GPIO" + String(pin) + " hanya input";
    } else {
      pinMode(pin, OUTPUT);
      digitalWrite(pin, HIGH);
      debugMessage = "GPIO" + String(pin) + " diset HIGH";
    }
  } else if (action == "low") {
    if (isInputOnlyPin((uint8_t)pin)) {
      debugMessage = "GPIO" + String(pin) + " hanya input";
    } else {
      pinMode(pin, OUTPUT);
      digitalWrite(pin, LOW);
      debugMessage = "GPIO" + String(pin) + " diset LOW";
    }
  } else if (action == "input") {
    pinMode(pin, INPUT);
    debugMessage = "GPIO" + String(pin) + " diset INPUT";
  } else if (action == "pullup") {
    if (isInputOnlyPin((uint8_t)pin)) {
      debugMessage = "GPIO" + String(pin) + " tidak mendukung pullup via pinMode ini";
    } else {
      pinMode(pin, INPUT_PULLUP);
      debugMessage = "GPIO" + String(pin) + " diset INPUT_PULLUP";
    }
  } else {
    debugMessage = "Aksi debug tidak dikenali";
  }

  if (pin == LED_status || pin == BUTTON_PIN || pin == LED_BUTTON || pin == LED_ACTION || pin == LED_ACTION_INV) {
    debugMessage += " (pin ini dipakai oleh aplikasi utama)";
  }

  server.sendHeader("Location", "/debug#debug");
  server.send(303);
}

void handleUpdateUpload() {
  if (!dashboardUnlocked) return;
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
  if (!requireDashboardUnlocked()) return;
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
  server.on("/unlock", HTTP_POST, handleUnlock);
  server.on("/debug", HTTP_GET, handleDebugPage);
  server.on("/save", HTTP_POST, handleSaveConfig);
  server.on("/debug/pin", HTTP_POST, handleDebugPin);
  server.on("/update", HTTP_POST, handleUpdateDone, handleUpdateUpload);
  server.begin();
  Serial.println("Web dashboard aktif di port 80");
}

void gatewayPublishSerialLine(const String& topic, const String& payload) {
  if (!gatewayUsesSerialUplink()) return;
  String line;
  line.reserve(topic.length() + payload.length() + 32);
  line += "{\"topic\":\"";
  line += topic;
  line += "\",\"payload\":";
  line += payload;
  line += "}";
  Serial.println(line);
}

void gatewayHandleSerialLine(const String& line) {
  if (!isGatewayRole() || !gatewayUsesSerialUplink()) return;
  if (line.length() == 0) return;

  String topic = parseJsonString(line, "topic");
  String payloadObj = "";
  if (topic.length() == 0 || !parseJsonObjectRaw(line, "payload", &payloadObj)) {
    Serial.println("Serial command invalid");
    return;
  }

  handleGatewayMqttMessage(topic.c_str(), payloadObj);
}

void gatewayProcessSerialInput() {
  if (!isGatewayRole() || !gatewayUsesSerialUplink()) return;

  while (Serial.available() > 0) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      String line = gatewaySerialRxLine;
      gatewaySerialRxLine = "";
      line.trim();
      gatewayHandleSerialLine(line);
      continue;
    }
    if (gatewaySerialRxLine.length() < 400) {
      gatewaySerialRxLine += c;
    }
  }
}

void gatewayPublishConfirmFromPacket(const EspNowPacket& pkt) {
  String payload;
  payload.reserve(100);
  payload += "{\"slot_number\":";
  payload += String(pkt.slotNumber);
  payload += ",\"rfid_tag\":\"";
  payload += String(pkt.rfidTag);
  payload += "\",\"status\":";
  payload += (pkt.status ? "true" : "false");
  payload += "}";
  if (gatewayUsesMqttUplink() && mqttClient.connected()) {
    mqttClient.publish(MQTT_CONFIRM_TOPIC, payload.c_str());
  }
  gatewayPublishSerialLine(MQTT_CONFIRM_TOPIC, payload);
}

void gatewayPublishMaintenanceFromPacket(const EspNowPacket& pkt, int slotIdx) {
  String payload;
  payload.reserve(150);
  payload += "{\"slot_number\":";
  payload += String(pkt.slotNumber);
  payload += ",\"ip_address\":\"";
  payload += WiFi.localIP().toString();
  payload += "\",\"status\":";
  payload += (pkt.status ? "true" : "false");
  payload += ",\"solenoid\":";
  payload += ((pkt.flags & 0x01) ? "true" : "false");
  payload += ",\"rfid_tag\":\"";
  payload += String(pkt.rfidTag);
  payload += "\"}";
  if (gatewayUsesMqttUplink() && mqttClient.connected()) {
    mqttClient.publish(MQTT_MAINT_TOPIC, payload.c_str());
  }
  gatewayPublishSerialLine(MQTT_MAINT_TOPIC, payload);

  if (slotIdx >= 0 && slotIdx <= MAX_SLOT_INDEX) {
    slotPeers[slotIdx].lastSeenMs = millis();
  }
}

void gatewayClearPendingAck(uint8_t slotNo, uint32_t msgId) {
  int idx = safeSlotIndex(slotNo);
  if (idx < 0) return;
  if (slotPeers[idx].pendingActive && slotPeers[idx].pendingMsgId == msgId) {
    slotPeers[idx].pendingActive = false;
    slotPeers[idx].pendingRetries = 0;
  }
}

void gatewayQueueDeferredPacket(uint8_t slotNo, const EspNowPacket& pkt, bool trackAck) {
  int idx = safeSlotIndex(slotNo);
  if (idx < 0) return;
  slotPeers[idx].deferredActive = true;
  slotPeers[idx].deferredTrackAck = trackAck;
  slotPeers[idx].deferredQueuedMs = millis();
  slotPeers[idx].deferredPacket = pkt;
  Serial.print("Perintah slot ");
  Serial.print(slotNo);
  Serial.println(" di-queue menunggu peer online");
}

bool gatewayTryFlushDeferredPacket(uint8_t slotNo) {
  int idx = safeSlotIndex(slotNo);
  if (idx < 0) return false;
  if (!slotPeers[idx].deferredActive) return false;
  if (!slotPeers[idx].valid) return false;

  EspNowPacket pkt = slotPeers[idx].deferredPacket;
  bool trackAck = slotPeers[idx].deferredTrackAck;
  bool sent = espNowSendPacket(slotPeers[idx].mac, pkt);
  if (!sent) return false;

  if (trackAck) {
    slotPeers[idx].pendingActive = true;
    slotPeers[idx].pendingMsgId = pkt.msgId;
    slotPeers[idx].pendingRetries = 0;
    slotPeers[idx].pendingSentMs = millis();
    slotPeers[idx].pendingPacket = pkt;
  }
  slotPeers[idx].deferredActive = false;
  Serial.print("Perintah tertunda slot ");
  Serial.print(slotNo);
  Serial.println(" berhasil dikirim");
  return true;
}

bool gatewaySendPacketToSlot(uint8_t slotNo, EspNowPacket& pkt, bool trackAck) {
  int idx = safeSlotIndex(slotNo);
  if (idx < 0) {
    Serial.print("Slot tidak valid: ");
    Serial.println(slotNo);
    return false;
  }
  if (!slotPeers[idx].valid) {
    Serial.print("Peer slot tidak tersedia: ");
    Serial.println(slotNo);
    gatewayQueueDeferredPacket(slotNo, pkt, trackAck);
    return false;
  }

  bool sent = espNowSendPacket(slotPeers[idx].mac, pkt);
  if (!sent) {
    Serial.print("Gagal kirim ESP-NOW ke slot ");
    Serial.println(slotNo);
    gatewayQueueDeferredPacket(slotNo, pkt, trackAck);
    return false;
  }
  if (sent && trackAck) {
    slotPeers[idx].pendingActive = true;
    slotPeers[idx].pendingMsgId = pkt.msgId;
    slotPeers[idx].pendingRetries = 0;
    slotPeers[idx].pendingSentMs = millis();
    slotPeers[idx].pendingPacket = pkt;
  }
  return sent;
}

void handleGatewayMqttMessage(const char* topic, const String& message) {
  int slotFromStatus = parseTrailingSlotFromTopic(topic, "boseh/status/");
  if (slotFromStatus > 0 && slotFromStatus <= MAX_SLOT_INDEX) {
    String rfidTag = parseJsonString(message, "rfid_tag");
    if (rfidTag.length() == 0) return;

    EspNowPacket pkt = {};
    pkt.version = ESPNOW_PROTO_VER;
    pkt.msgType = ESPNOW_MSG_STATUS_ARM;
    pkt.slotNumber = (uint8_t)slotFromStatus;
    pkt.status = 1;
    pkt.msgId = nextGatewayMsgId();
    pkt.timestampMs = millis();
    rfidTag.toCharArray(pkt.rfidTag, sizeof(pkt.rfidTag));
    bool ok = gatewaySendPacketToSlot((uint8_t)slotFromStatus, pkt, true);
    if (!ok) {
      Serial.print("Status arm slot ");
      Serial.print(slotFromStatus);
      Serial.println(" belum terkirim (queued atau peer belum siap)");
    }
    return;
  }

  int slotFromControl = parseSlotFromControlTopic(topic);
  if (slotFromControl > 0 && slotFromControl <= MAX_SLOT_INDEX) {
    String command = parseJsonString(message, "command");
    bool value = false;
    bool hasValue = parseJsonBool(message, "value", &value);
    if (command != "solenoid" || !hasValue) return;

    EspNowPacket pkt = {};
    pkt.version = ESPNOW_PROTO_VER;
    pkt.msgType = ESPNOW_MSG_CONTROL;
    pkt.slotNumber = (uint8_t)slotFromControl;
    pkt.msgId = nextGatewayMsgId();
    pkt.timestampMs = millis();
    pkt.value = value ? 1 : 0;
    command.toCharArray(pkt.command, sizeof(pkt.command));
    bool ok = gatewaySendPacketToSlot((uint8_t)slotFromControl, pkt, true);
    if (!ok) {
      Serial.print("Control slot ");
      Serial.print(slotFromControl);
      Serial.println(" belum terkirim (queued atau peer belum siap)");
    }
    return;
  }

  int slotFromMaintReq = parseTrailingSlotFromTopic(topic, "boseh/");
  if (slotFromMaintReq > 0 && slotFromMaintReq <= MAX_SLOT_INDEX) {
    bool statusReq = false;
    if (!parseJsonBool(message, "status", &statusReq) || !statusReq) return;

    EspNowPacket pkt = {};
    pkt.version = ESPNOW_PROTO_VER;
    pkt.msgType = ESPNOW_MSG_MAINT_REQ;
    pkt.slotNumber = (uint8_t)slotFromMaintReq;
    pkt.status = 1;
    pkt.msgId = nextGatewayMsgId();
    pkt.timestampMs = millis();
    bool ok = gatewaySendPacketToSlot((uint8_t)slotFromMaintReq, pkt, true);
    if (!ok) {
      Serial.print("Maintenance req slot ");
      Serial.print(slotFromMaintReq);
      Serial.println(" belum terkirim (queued atau peer belum siap)");
    }
    return;
  }

  Serial.print("Topik MQTT tidak cocok routing gateway: ");
  Serial.println(topic);
}

void handleNodeEspNowPacket(const uint8_t* srcMac, const EspNowPacket& pkt) {
  if (gatewayMacValid && srcMac && memcmp(srcMac, gatewayMacBytes, 6) != 0) return;
  if (pkt.slotNumber != (uint8_t)slotNumberConfig) return;
  logNodeInboundPacket(srcMac, pkt);

  if (pkt.msgType == ESPNOW_MSG_ACK) {
    if (nodePending.active && pkt.msgId == nodePending.msgId) {
      nodePending.active = false;
      nodePending.retries = 0;
      nodeKickReliableTx();
    }
    return;
  }

  if (pkt.msgType == ESPNOW_MSG_STATUS_ARM) {
    EspNowPacket ack = {};
    buildAckPacket(&ack, pkt.slotNumber, pkt.msgId);
    espNowSendPacket(srcMac, ack);

    if (pkt.msgId == nodeLastStatusArmMsgId) return;
    nodeLastStatusArmMsgId = pkt.msgId;

    String rfidTag = String(pkt.rfidTag);
    if (rfidTag.length() == 0) {
      if (buttonWaitActive && !actionActive) {
        setMirrorLed(false);
        solenoidState = false;
      }
      buttonWaitActive = false;
      buttonWaitStartMs = 0;
      currentBikeId = "";
      lastStatusRfidTag = "";
      return;
    }

    currentBikeId = rfidTag;
    lastStatusRfidTag = rfidTag;
    buttonWaitActive = true;
    buttonWaitStartMs = millis();
    setMirrorLed(true);
    solenoidState = true;
    return;
  }

  if (pkt.msgType == ESPNOW_MSG_CONTROL) {
    EspNowPacket ack = {};
    buildAckPacket(&ack, pkt.slotNumber, pkt.msgId);
    espNowSendPacket(srcMac, ack);

    if (pkt.msgId == nodeLastControlMsgId) return;
    nodeLastControlMsgId = pkt.msgId;

    String command = String(pkt.command);
    if (command != "solenoid") return;
    bool value = pkt.value == 1;
    solenoidState = value;
    setActionLed(solenoidState);
    setMirrorLed(solenoidState);
    if (solenoidState) {
      actionActive = true;
      actionStartMs = millis();
    } else {
      actionActive = false;
    }
    return;
  }

  if (pkt.msgType == ESPNOW_MSG_MAINT_REQ && pkt.status == 1) {
    EspNowPacket ack = {};
    buildAckPacket(&ack, pkt.slotNumber, pkt.msgId);
    espNowSendPacket(srcMac, ack);
    publishMaintenanceStatus();
    return;
  }
}

void handleGatewayEspNowPacket(const uint8_t* srcMac, const EspNowPacket& pkt) {
  if (!srcMac) return;
  int idx = safeSlotIndex(pkt.slotNumber);
  if (idx < 0) return;

  if (!slotPeers[idx].valid) {
    slotPeers[idx].valid = true;
    memcpy(slotPeers[idx].mac, srcMac, 6);
    Serial.print("Peer slot ");
    Serial.print(idx);
    Serial.print(" locked ke MAC ");
    Serial.println(macToString(srcMac));
  } else if (!macEquals(slotPeers[idx].mac, srcMac)) {
    gatewayPeerRejectCount++;
    static unsigned long lastRejectLogMs = 0;
    unsigned long nowLog = millis();
    if ((nowLog - lastRejectLogMs) > 1000) {
      Serial.print("Reject packet slot ");
      Serial.print(idx);
      Serial.print(" dari MAC tidak dikenal: ");
      Serial.print(macToString(srcMac));
      Serial.print(" (expected ");
      Serial.print(macToString(slotPeers[idx].mac));
      Serial.print(") totalReject=");
      Serial.println(gatewayPeerRejectCount);
      lastRejectLogMs = nowLog;
    }
    return;
  }

  slotPeers[idx].lastSeenMs = millis();
  gatewayTryFlushDeferredPacket((uint8_t)idx);

  if (pkt.msgType == ESPNOW_MSG_ACK) {
    gatewayClearPendingAck(pkt.slotNumber, pkt.msgId);
    return;
  }

  if (pkt.msgId == slotPeers[idx].lastInboundMsgId) {
    return; // dedup
  }
  slotPeers[idx].lastInboundMsgId = pkt.msgId;

  if (pkt.msgType == ESPNOW_MSG_STATUS_UPDATE) {
    gatewayPublishConfirmFromPacket(pkt);
    EspNowPacket ack = {};
    buildAckPacket(&ack, pkt.slotNumber, pkt.msgId);
    espNowSendPacket(srcMac, ack);
    return;
  }

  if (pkt.msgType == ESPNOW_MSG_MAINT_RESP) {
    gatewayPublishMaintenanceFromPacket(pkt, idx);
    EspNowPacket ack = {};
    buildAckPacket(&ack, pkt.slotNumber, pkt.msgId);
    espNowSendPacket(srcMac, ack);
    return;
  }

  if (pkt.msgType == ESPNOW_MSG_HEARTBEAT) {
    EspNowPacket ack = {};
    buildAckPacket(&ack, pkt.slotNumber, pkt.msgId);
    espNowSendPacket(srcMac, ack);
    return;
  }
}

void processEspNowQueue() {
  EspNowRxItem item;
  while (espNowQueuePop(&item)) {
    if (isGatewayRole()) {
      handleGatewayEspNowPacket(item.mac, item.pkt);
    } else {
      handleNodeEspNowPacket(item.mac, item.pkt);
    }
  }
}

void gatewayHandlePendingRetries() {
  unsigned long now = millis();
  for (int i = 1; i <= MAX_SLOT_INDEX; i++) {
    if (slotPeers[i].deferredActive && slotPeers[i].valid) {
      gatewayTryFlushDeferredPacket((uint8_t)i);
    }

    if (!slotPeers[i].pendingActive) continue;
    if ((now - slotPeers[i].pendingSentMs) < 700) continue;

    if (slotPeers[i].pendingRetries >= 3) {
      slotPeers[i].pendingActive = false;
      Serial.print("ACK timeout slot ");
      Serial.println(i);
      continue;
    }

    if (espNowSendPacket(slotPeers[i].mac, slotPeers[i].pendingPacket)) {
      slotPeers[i].pendingRetries++;
      slotPeers[i].pendingSentMs = now;
    }
  }
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  if (!isGatewayRole()) return;

  String message;
  message.reserve(length);
  for (unsigned int i = 0; i < length; i++) {
    message += (char)payload[i];
  }

  Serial.print("MQTT masuk [");
  Serial.print(topic);
  Serial.print("]: ");
  Serial.println(message);

  handleGatewayMqttMessage(topic, message);
}

void runNodeEspNowSetup() {
  mqttClient.disconnect();
  nodeTxHead = 0;
  nodeTxTail = 0;
  nodePending.active = false;
  nodePending.retries = 0;
  gatewayMacValid = parseMacAddress(gatewayMacConfig, gatewayMacBytes);
  if (!gatewayMacValid) {
    Serial.println("Gateway MAC invalid, ESP-NOW node tidak aktif");
    return;
  }
  if (!initEspNowStack()) {
    Serial.println("ESP-NOW node init gagal");
    return;
  }
  espNowEnsurePeer(gatewayMacBytes);
  lastNodeHeartbeatMs = 0;
  Serial.print("Role NODE aktif, mode ESP-NOW. Gateway MAC: ");
  Serial.println(macToString(gatewayMacBytes));
}

void runGatewayBridgeSetup() {
  mqttClient.setCallback(mqttCallback);
  if (gatewayUsesMqttUplink()) {
    connectMQTT();
  } else {
    mqttClient.disconnect();
    Serial.println("Gateway uplink=serial, command dari MQTT tidak diproses");
  }
  if (!initEspNowStack()) {
    Serial.println("ESP-NOW gateway init gagal");
    return;
  }
  memset(slotPeers, 0, sizeof(slotPeers));
  Serial.println("Role GATEWAY aktif (bridge ESP-NOW <-> MQTT)");
}

void runNodeEspNowLoop() {
  if (WiFi.status() != WL_CONNECTED) connectWiFi(false);
  unsigned long now = millis();
  if (!espNowReady && (now - lastEspNowInitAttemptMs >= 2000)) {
    initEspNowStack();
  }
  processEspNowQueue();
  nodeKickReliableTx();
  nodeHandlePendingRetries();

  bool inRange = digitalRead(RANGE_PIN) == HIGH;
  if (inRange && !tagDetected) {
    tagDetected = true;
    confirmOpenSentInCurrentRange = false;
    Serial.println("TAG MASUK RANGE!");
  }

  if (RFIDSerial.available() > 0) {
    String rawHex = "";
    bool frameEnded = false;
    unsigned long readStartMs = millis();
    while ((millis() - readStartMs) < 30 && !frameEnded) {
      while (RFIDSerial.available() > 0) {
        char c = RFIDSerial.read();
        if (c >= 'a' && c <= 'f') c = (char)(c - 32);
        if (c == '\r' || c == '\n') {
          frameEnded = true;
          break;
        }
        if (isHexChar(c)) rawHex += c;
      }
      if (!frameEnded) delay(1);
    }

    String validatedTag = "";
    if (extractValidTagFromRawHex(rawHex, &validatedTag)) {
      tagID = validatedTag;
      setStatusLed(true);
      lastCardRfidTag = tagID;
      if (inRange && !confirmOpenSentInCurrentRange) {
        publishConfirmOpen(lastCardRfidTag, true);
        confirmOpenSentInCurrentRange = true;
      }
    }
  }

  if (!inRange && tagDetected) {
    tagDetected = false;
    confirmOpenSentInCurrentRange = false;
    setStatusLed(false);
    if (lastCardRfidTag.length() > 0) {
      publishConfirmOpen(lastCardRfidTag, false);
    }
  }

  bool buttonPressed = (digitalRead(BUTTON_PIN) == LOW);
  digitalWrite(LED_BUTTON, buttonPressed ? LOW : HIGH);

  if (actionActive && (millis() - actionStartMs >= actionDurationMs)) {
    actionActive = false;
    setActionLed(false);
    setMirrorLed(false);
    solenoidState = false;
  }

  if (buttonWaitActive && !actionActive && (millis() - buttonWaitStartMs >= buttonWaitTimeoutMs)) {
    buttonWaitActive = false;
    buttonWaitStartMs = 0;
    setMirrorLed(false);
    solenoidState = false;
    currentBikeId = "";
    lastStatusRfidTag = "";
  }

  if (buttonWaitActive && !actionActive && buttonPressed && !lastButtonState) {
    buttonWaitActive = false;
    buttonWaitStartMs = 0;
    actionActive = true;
    actionStartMs = millis();
    setActionLed(true);
    setMirrorLed(true);
    solenoidState = true;
    String confirmTag = lastCardRfidTag.length() > 0 ? lastCardRfidTag : currentBikeId;
    if (confirmTag.length() > 0) publishConfirmOpen(confirmTag, false);
    currentBikeId = "";
  }

  if (gatewayMacValid && espNowReady && (now - lastNodeHeartbeatMs >= 10000)) {
    EspNowPacket hb = {};
    hb.version = ESPNOW_PROTO_VER;
    hb.msgType = ESPNOW_MSG_HEARTBEAT;
    hb.slotNumber = (uint8_t)slotNumberConfig;
    hb.status = 1;
    hb.msgId = nextNodeMsgId();
    hb.timestampMs = now;
    hb.flags = solenoidState ? 0x01 : 0x00;
    lastCardRfidTag.toCharArray(hb.rfidTag, sizeof(hb.rfidTag));
    nodeQueueReliablePacket(&hb);
    nodeKickReliableTx();
    lastNodeHeartbeatMs = now;
  }

  lastButtonState = buttonPressed;
  delay(50);
}

void runGatewayBridgeLoop() {
  if (WiFi.status() != WL_CONNECTED) connectWiFi(false);
  unsigned long now = millis();
  if (!espNowReady && (now - lastEspNowInitAttemptMs >= 2000)) {
    initEspNowStack();
  }
  if (gatewayUsesMqttUplink()) {
    if (!mqttClient.connected()) connectMQTT();
    mqttClient.loop();
  }
  gatewayProcessSerialInput();
  processEspNowQueue();
  gatewayHandlePendingRetries();
  delay(20);
}

void setup() {
  Serial.begin(115200);
  RFIDSerial.begin(9600, SERIAL_8N1, RX_PIN, -1);
  loadConfig();
  printDeviceMacInfo();

  pinMode(RANGE_PIN, INPUT);
  pinMode(LED_status, OUTPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(LED_BUTTON, OUTPUT);
  pinMode(LED_ACTION, OUTPUT);
  pinMode(LED_ACTION_INV, OUTPUT);

  setStatusLed(false);
  digitalWrite(LED_BUTTON, HIGH);
  setActionLed(false);
  setMirrorLed(false);
  connectWiFi(true);
  setupWebServer();

  if (isGatewayRole()) {
    runGatewayBridgeSetup();
  } else {
    runNodeEspNowSetup();
  }

  bool modbusReady = ModbusHandler::begin(MODBUS_SLAVE_ID_DEFAULT, (uint16_t)slotNumberConfig);
  Serial.println(modbusReady ? "Modbus RTU siap (Commit A bootstrap)" : "Modbus RTU gagal init");

  Serial.println("\n=== ESP32 + ID-12LA RFID Reader Siap ===");
  Serial.println("Dashboard: buka IP ESP32 di browser");
}

void loop() {
  ModbusHandler::updateBasicStatus((uint16_t)slotNumberConfig, millis());
  ModbusHandler::task();
  server.handleClient();

  if (apModeActive) {
    delay(10);
    return;
  }

  if (isGatewayRole()) {
    runGatewayBridgeLoop();
    return;
  }

  runNodeEspNowLoop();
}
