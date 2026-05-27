// ================================================
// ESP32 + ID-12LA RFID Reader + ESP-NOW + Serial Gateway
// ================================================

#include <WiFi.h>
#include <Preferences.h>
#include <esp_now.h>
#include <esp_wifi.h>

#define RX_PIN      16     // GPIO16 -> RFID Pin 9 (D0)
#define RANGE_PIN   17     // GPIO17 -> RFID Pin 6 (Tag in Range)
#define LED_status  2      // GPIO2 -> LED indikator (opsional) // untuk kalau ada RFID
#define BUTTON_PIN  23     // GPIO23 -> Tombol input
#define LED_BUTTON  15     // GPIO15 -> LED tombol (aktif LOW)
#define LED_ACTION  4      // GPIO4  -> LED aksi 10 detik (ini untuk output relay)
#define LED_ACTION_INV 22  // GPIO22 -> LED indikator status (aktif LOW)

const int   SLOT_NUMBER_DEFAULT = 1;
const unsigned long ACTION_DURATION_MS_DEFAULT = 10000;
const unsigned long BUTTON_WAIT_TIMEOUT_MS_DEFAULT = 30000;
const char* DEVICE_ROLE_DEFAULT = "node";

const char* MQTT_CONFIRM_TOPIC = "boseh/stasiun/confirm_open";
const char* MQTT_MAINT_TOPIC = "boseh/maintenance";

int slotNumberConfig = SLOT_NUMBER_DEFAULT;
String deviceRoleConfig = DEVICE_ROLE_DEFAULT;     // node | gateway
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

bool relayPin4ActiveHigh = true;
bool relayPin2ActiveHigh = true;
bool relayPin22ActiveHigh = false;
const uint8_t DEBUG_PINS[] = {LED_status, BUTTON_PIN, LED_BUTTON, LED_ACTION, LED_ACTION_INV};
String debugMessage = "";
unsigned long actionDurationMs = ACTION_DURATION_MS_DEFAULT;
unsigned long buttonWaitTimeoutMs = BUTTON_WAIT_TIMEOUT_MS_DEFAULT;
String gatewayMacConfig = "";

const uint8_t ESPNOW_PROTO_VER = 1;
const uint8_t ESPNOW_MSG_STATUS_UPDATE = 1; // node -> gateway (confirm/status)
const uint8_t ESPNOW_MSG_STATUS_ARM = 2;    // gateway -> node (from serial status/{slot})
const uint8_t ESPNOW_MSG_CONTROL = 3;       // gateway -> node
const uint8_t ESPNOW_MSG_MAINT_REQ = 4;     // gateway -> node
const uint8_t ESPNOW_MSG_MAINT_RESP = 5;    // node -> gateway
const uint8_t ESPNOW_MSG_ACK = 6;           // both directions
const uint8_t ESPNOW_MSG_HEARTBEAT = 7;     // node -> gateway
const uint8_t ESPNOW_FIXED_CHANNEL = 1;     // node dan gateway wajib channel sama

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
String nodeSerialRxLine = "";

String toLowerTrim(String input) {
  input.trim();
  input.toLowerCase();
  return input;
}

bool isGatewayRole() {
  return deviceRoleConfig == "gateway";
}

bool gatewayUsesMqttUplink() {
  return false;
}
bool gatewayUsesSerialUplink() {
  return true;
}

bool isLocalSerialCommand(const String& rawLine) {
  String line = rawLine;
  line.trim();
  if (line.length() == 0) return false;
  String upper = line;
  upper.toUpperCase();
  if (upper == "HELP" || upper == "GET CONFIG" || upper == "SAVE" || upper == "REBOOT") return true;
  if (upper.startsWith("SET ")) return true;
  return false;
}

void printLocalCommandHelp() {
  Serial.println("OK COMMANDS: HELP | GET CONFIG | SAVE | REBOOT | SET ROLE <NODE|GATEWAY> | SET SLOT <1..64> | SET GATEWAY_MAC <AA:BB:CC:DD:EE:FF>");
}

void printConfigJson() {
  String payload;
  payload.reserve(180);
  payload += "{\"slot_number\":";
  payload += String(slotNumberConfig);
  payload += ",\"device_role\":\"";
  payload += deviceRoleConfig;
  payload += "\",\"gateway_mac\":\"";
  payload += gatewayMacConfig;
  payload += "\",\"action_ms\":";
  payload += String(actionDurationMs);
  payload += ",\"button_timeout_ms\":";
  payload += String(buttonWaitTimeoutMs);
  payload += "}";
  Serial.println("CONFIG " + payload);
}

void persistCurrentConfig() {
  saveConfig(
    slotNumberConfig,
    deviceRoleConfig,
    gatewayMacConfig,
    actionDurationMs,
    buttonWaitTimeoutMs,
    relayPin4ActiveHigh,
    relayPin2ActiveHigh,
    relayPin22ActiveHigh
  );
  gatewayMacValid = parseMacAddress(gatewayMacConfig, gatewayMacBytes);
  if (gatewayMacValid && espNowReady) espNowEnsurePeer(gatewayMacBytes);
}

void handleLocalSerialCommand(const String& rawLine) {
  String line = rawLine;
  line.trim();
  if (line.length() == 0) return;
  String upper = line;
  upper.toUpperCase();

  if (upper == "HELP") {
    printLocalCommandHelp();
    return;
  }
  if (upper == "GET CONFIG") {
    printConfigJson();
    return;
  }
  if (upper == "SAVE") {
    persistCurrentConfig();
    Serial.println("OK SAVED");
    return;
  }
  if (upper == "REBOOT") {
    Serial.println("OK REBOOT");
    delay(100);
    ESP.restart();
    return;
  }
  if (!upper.startsWith("SET ")) {
    Serial.println("ERR unknown_command");
    return;
  }

  int firstSpace = line.indexOf(' ');
  int secondSpace = line.indexOf(' ', firstSpace + 1);
  if (secondSpace < 0) {
    Serial.println("ERR set_format");
    return;
  }
  String key = toLowerTrim(line.substring(firstSpace + 1, secondSpace));
  String value = line.substring(secondSpace + 1);
  value.trim();
  if (value.length() == 0) {
    Serial.println("ERR empty_value");
    return;
  }

  if (key == "role") {
    String role = toLowerTrim(value);
    if (role != "node" && role != "gateway") {
      Serial.println("ERR invalid_role");
      return;
    }
    deviceRoleConfig = role;
    Serial.println("OK ROLE");
    return;
  }
  if (key == "slot") {
    int slot = value.toInt();
    if (slot < 1 || slot > MAX_SLOT_INDEX) {
      Serial.println("ERR invalid_slot");
      return;
    }
    slotNumberConfig = slot;
    Serial.println("OK SLOT");
    return;
  }
  if (key == "gateway_mac") {
    String normalized = toLowerTrim(value);
    uint8_t tmp[6] = {0};
    if (!parseMacAddress(normalized, tmp)) {
      Serial.println("ERR invalid_gateway_mac");
      return;
    }
    gatewayMacConfig = normalized;
    gatewayMacValid = true;
    memcpy(gatewayMacBytes, tmp, 6);
    if (espNowReady) espNowEnsurePeer(gatewayMacBytes);
    Serial.println("OK GATEWAY_MAC");
    return;
  }
  Serial.println("ERR unknown_set_key");
}

void nodeProcessSerialInput() {
  if (isGatewayRole()) return;
  while (Serial.available() > 0) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      String line = nodeSerialRxLine;
      nodeSerialRxLine = "";
      line.trim();
      if (line.length() > 0) handleLocalSerialCommand(line);
      continue;
    }
    if (nodeSerialRxLine.length() < 220) nodeSerialRxLine += c;
  }
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
  WiFi.mode(WIFI_STA);
  delay(10);
  String staMac = WiFi.macAddress();

  uint64_t chipMac = ESP.getEfuseMac();
  uint8_t efuseMac[6];
  efuseMac[0] = (uint8_t)(chipMac >> 40);
  efuseMac[1] = (uint8_t)(chipMac >> 32);
  efuseMac[2] = (uint8_t)(chipMac >> 24);
  efuseMac[3] = (uint8_t)(chipMac >> 16);
  efuseMac[4] = (uint8_t)(chipMac >> 8);
  efuseMac[5] = (uint8_t)(chipMac);
  char efuseMacBuf[18];
  snprintf(efuseMacBuf, sizeof(efuseMacBuf), "%02X:%02X:%02X:%02X:%02X:%02X",
           efuseMac[0], efuseMac[1], efuseMac[2], efuseMac[3], efuseMac[4], efuseMac[5]);

  Serial.println("=== Device MAC Info ===");
  Serial.print("STA MAC (pakai untuk SET GATEWAY_MAC): ");
  Serial.println(staMac);
  Serial.print("eFuse MAC (referensi chip)          : ");
  Serial.println(efuseMacBuf);
  Serial.print("ESP-NOW fixed channel               : ");
  Serial.println(ESPNOW_FIXED_CHANNEL);
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
  peerInfo.channel = ESPNOW_FIXED_CHANNEL;
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
  esp_err_t ch = esp_wifi_set_channel(ESPNOW_FIXED_CHANNEL, WIFI_SECOND_CHAN_NONE);
  if (ch != ESP_OK) {
    Serial.print("Set channel gagal, rc=");
    Serial.println((int)ch);
  }
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
  slotNumberConfig = prefs.getInt("slot_no", SLOT_NUMBER_DEFAULT);
  deviceRoleConfig = toLowerTrim(prefs.getString("dev_role", DEVICE_ROLE_DEFAULT));
  gatewayMacConfig = toLowerTrim(prefs.getString("gw_mac", ""));
  actionDurationMs = prefs.getULong("act_ms", ACTION_DURATION_MS_DEFAULT);
  buttonWaitTimeoutMs = prefs.getULong("wait_ms", BUTTON_WAIT_TIMEOUT_MS_DEFAULT);
  relayPin4ActiveHigh = prefs.getBool("r4_act_hi", true);
  relayPin2ActiveHigh = prefs.getBool("r2_act_hi", true);
  relayPin22ActiveHigh = prefs.getBool("r22_act_hi", false);
  prefs.end();

  if (deviceRoleConfig != "node" && deviceRoleConfig != "gateway") deviceRoleConfig = DEVICE_ROLE_DEFAULT;
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
  int slotNo,
  const String& role,
  const String& gatewayMac,
  unsigned long actionMs,
  unsigned long waitMs,
  bool pin4ActiveHigh,
  bool pin2ActiveHigh,
  bool pin22ActiveHigh
) {
  prefs.begin("cfg", false);
  prefs.putInt("slot_no", slotNo);
  prefs.putString("dev_role", role);
  prefs.putString("gw_mac", gatewayMac);
  prefs.putULong("act_ms", actionMs);
  prefs.putULong("wait_ms", waitMs);
  prefs.putBool("r4_act_hi", pin4ActiveHigh);
  prefs.putBool("r2_act_hi", pin2ActiveHigh);
  prefs.putBool("r22_act_hi", pin22ActiveHigh);
  prefs.end();
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

  String payload;
  payload.reserve(96);
  payload += "{\"slot_number\":";
  payload += String(slotNumberConfig);
  payload += ",\"rfid_tag\":\"";
  payload += rfidTag;
  payload += "\",\"status\":";
  payload += (status ? "true" : "false");
  payload += "}";
  gatewayPublishSerialLine(MQTT_CONFIRM_TOPIC, payload);
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

  String payload;
  payload.reserve(128);
  payload += "{\"slot_number\":";
  payload += String(slotNumberConfig);
  payload += ",\"ip_address\":\"0.0.0.0\"";
  payload += ",\"status\":true";
  payload += ",\"solenoid\":";
  payload += (solenoidState ? "true" : "false");
  payload += ",\"rfid_tag\":\"";
  payload += lastCardRfidTag;
  payload += "\"}";
  gatewayPublishSerialLine(MQTT_MAINT_TOPIC, payload);
}

void gatewayHandleSerialLine(const String& line) {
  if (!isGatewayRole() || !gatewayUsesSerialUplink()) return;
  if (line.length() == 0) return;

  if (isLocalSerialCommand(line)) {
    handleLocalSerialCommand(line);
    return;
  }

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
  gatewayPublishSerialLine(MQTT_CONFIRM_TOPIC, payload);
}

void gatewayPublishMaintenanceFromPacket(const EspNowPacket& pkt, int slotIdx) {
  String payload;
  payload.reserve(150);
  payload += "{\"slot_number\":";
  payload += String(pkt.slotNumber);
  payload += ",\"ip_address\":\"";
  payload += "0.0.0.0";
  payload += "\",\"status\":";
  payload += (pkt.status ? "true" : "false");
  payload += ",\"solenoid\":";
  payload += ((pkt.flags & 0x01) ? "true" : "false");
  payload += ",\"rfid_tag\":\"";
  payload += String(pkt.rfidTag);
  payload += "\"}";
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

  Serial.print("Topik uplink tidak cocok routing gateway: ");
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

void runNodeEspNowSetup() {
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
  Serial.println("Gateway uplink=serial (ESP-NOW only)");
  if (!initEspNowStack()) {
    Serial.println("ESP-NOW gateway init gagal");
    return;
  }
  memset(slotPeers, 0, sizeof(slotPeers));
  Serial.println("Role GATEWAY aktif (bridge ESP-NOW <-> SERIAL)");
}

void runNodeEspNowLoop() {
  unsigned long now = millis();
  if (!espNowReady && (now - lastEspNowInitAttemptMs >= 2000)) {
    initEspNowStack();
  }
  nodeProcessSerialInput();
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
  unsigned long now = millis();
  if (!espNowReady && (now - lastEspNowInitAttemptMs >= 2000)) {
    initEspNowStack();
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
  Serial.println("Mode ESP-NOW only aktif: WiFi STA/AP, Web dashboard, dan MQTT dinonaktifkan");

  if (isGatewayRole()) {
    runGatewayBridgeSetup();
  } else {
    runNodeEspNowSetup();
  }

  Serial.println("\n=== ESP32 + ID-12LA RFID Reader Siap ===");
  Serial.println("Dashboard web nonaktif pada mode ESP-NOW only");
}

void loop() {
  if (isGatewayRole()) {
    runGatewayBridgeLoop();
    return;
  }

  runNodeEspNowLoop();
}
