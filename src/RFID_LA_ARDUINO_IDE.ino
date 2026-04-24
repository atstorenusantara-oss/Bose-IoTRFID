// ================================================
// ESP32 + ID-12LA RFID Reader + Modbus RTU Only
// ================================================

#include <Preferences.h>

#include "modbus_handler.h"

#define RX_PIN          16  // GPIO16 -> RFID D0
#define RANGE_PIN       17  // GPIO17 -> RFID Tag in Range
#define LED_STATUS      2   // Status LED
#define BUTTON_PIN      23  // Tombol input (active low)
#define LED_BUTTON      15  // LED tombol (active low)
#define LED_ACTION      4   // Output action
#define LED_ACTION_INV  22  // Mirror/output indicator

const int SLOT_NUMBER_DEFAULT = 1;
const unsigned long ACTION_DURATION_MS_DEFAULT = 10000;
const unsigned long BUTTON_WAIT_TIMEOUT_MS_DEFAULT = 30000;

const uint8_t MODBUS_SLAVE_ID_DEFAULT = 1;
const uint16_t MODBUS_EVENT_BOOT = 1;
const uint16_t MODBUS_EVENT_TAG_ENTER = 2;
const uint16_t MODBUS_EVENT_TAG_LEAVE = 3;
const uint16_t MODBUS_EVENT_RFID_VALID = 4;
const uint16_t MODBUS_EVENT_BUTTON_PRESS = 6;
const uint16_t MODBUS_EVENT_SOLENOID_ON = 7;
const uint16_t MODBUS_EVENT_SOLENOID_OFF = 8;

const uint16_t MODBUS_FAULT_CFG_INVALID_RANGE = 1;
const uint16_t MODBUS_FAULT_CFG_LOCKED = 2;
const uint16_t MODBUS_BAUD_ENUM_DEFAULT = 0;
const uint16_t MODBUS_PARITY_DEFAULT = 0;
const uint16_t MODBUS_STOP_BITS_DEFAULT = 1;
const uint16_t MODBUS_UNLOCK_TIMEOUT_DEFAULT = 30;

Preferences prefs;
HardwareSerial RFIDSerial(2);

int slotNumberConfig = SLOT_NUMBER_DEFAULT;
unsigned long actionDurationMs = ACTION_DURATION_MS_DEFAULT;
unsigned long buttonWaitTimeoutMs = BUTTON_WAIT_TIMEOUT_MS_DEFAULT;
bool relayPin4ActiveHigh = true;
bool relayPin2ActiveHigh = true;
bool relayPin22ActiveHigh = false;
uint16_t modbusSlaveIdConfig = MODBUS_SLAVE_ID_DEFAULT;
uint16_t modbusBaudEnumConfig = MODBUS_BAUD_ENUM_DEFAULT;
uint16_t modbusParityConfig = MODBUS_PARITY_DEFAULT;
uint16_t modbusStopBitsConfig = MODBUS_STOP_BITS_DEFAULT;
uint16_t modbusUnlockTimeoutSecConfig = MODBUS_UNLOCK_TIMEOUT_DEFAULT;
bool localButtonEnableConfig = true;
uint16_t localButtonPriorityConfig = 0;

String tagID = "";
String lastCardRfidTag = "";
bool tagDetected = false;
bool confirmOpenSentInCurrentRange = false;

bool buttonWaitActive = false;
bool actionActive = false;
bool lastButtonState = false;
unsigned long buttonWaitStartMs = 0;
unsigned long actionStartMs = 0;
String currentBikeId = "";
String lastStatusRfidTag = "";
bool solenoidState = false;

uint16_t modbusLastEventCode = MODBUS_EVENT_BOOT;
bool modbusFaultActive = false;
uint16_t modbusFaultCode = 0;

void setModbusFault(uint16_t code, const char* message) {
  modbusFaultActive = true;
  modbusFaultCode = code;
  if (message) Serial.println(message);
}

void clearModbusFault(const char* message) {
  modbusFaultActive = false;
  modbusFaultCode = 0;
  if (message) Serial.println(message);
}

void setModbusEventCode(uint16_t code) {
  if (code == 0) return;
  modbusLastEventCode = code;
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

  for (int i = 0; i <= (int)rawHex.length() - 12; i++) {
    String tagCandidate = rawHex.substring(i, i + 10);
    String checksumCandidate = rawHex.substring(i + 10, i + 12);
    if (validateTagWithChecksum(tagCandidate, checksumCandidate)) {
      *outTag = tagCandidate + checksumCandidate;
      return true;
    }
  }

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

void writeOutputActiveLevel(uint8_t pin, bool active, bool activeHigh) {
  digitalWrite(pin, active ? (activeHigh ? HIGH : LOW) : (activeHigh ? LOW : HIGH));
}

void setActionLed(bool active) {
  writeOutputActiveLevel(LED_ACTION, active, relayPin4ActiveHigh);
}

void setStatusLed(bool active) {
  writeOutputActiveLevel(LED_STATUS, active, relayPin2ActiveHigh);
}

void setMirrorLed(bool active) {
  writeOutputActiveLevel(LED_ACTION_INV, active, relayPin22ActiveHigh);
}

void loadConfig() {
  prefs.begin("cfg", true);
  slotNumberConfig = prefs.getInt("slot_no", SLOT_NUMBER_DEFAULT);
  actionDurationMs = prefs.getULong("act_ms", ACTION_DURATION_MS_DEFAULT);
  buttonWaitTimeoutMs = prefs.getULong("wait_ms", BUTTON_WAIT_TIMEOUT_MS_DEFAULT);
  relayPin4ActiveHigh = prefs.getBool("r4_act_hi", true);
  relayPin2ActiveHigh = prefs.getBool("r2_act_hi", true);
  relayPin22ActiveHigh = prefs.getBool("r22_act_hi", false);
  modbusSlaveIdConfig = (uint16_t)prefs.getUShort("mb_sid", MODBUS_SLAVE_ID_DEFAULT);
  modbusBaudEnumConfig = (uint16_t)prefs.getUShort("mb_baud", MODBUS_BAUD_ENUM_DEFAULT);
  modbusParityConfig = (uint16_t)prefs.getUShort("mb_par", MODBUS_PARITY_DEFAULT);
  modbusStopBitsConfig = (uint16_t)prefs.getUShort("mb_stop", MODBUS_STOP_BITS_DEFAULT);
  modbusUnlockTimeoutSecConfig = (uint16_t)prefs.getUShort("mb_ulto", MODBUS_UNLOCK_TIMEOUT_DEFAULT);
  localButtonEnableConfig = prefs.getBool("mb_lbtn_en", true);
  localButtonPriorityConfig = (uint16_t)prefs.getUShort("mb_lbtn_pr", 0);
  prefs.end();

  if (slotNumberConfig <= 0) slotNumberConfig = SLOT_NUMBER_DEFAULT;
  if (modbusSlaveIdConfig == 0 || modbusSlaveIdConfig > 247) modbusSlaveIdConfig = MODBUS_SLAVE_ID_DEFAULT;
  if (modbusBaudEnumConfig > 4) modbusBaudEnumConfig = MODBUS_BAUD_ENUM_DEFAULT;
  if (modbusParityConfig > 2) modbusParityConfig = MODBUS_PARITY_DEFAULT;
  if (modbusStopBitsConfig != 1 && modbusStopBitsConfig != 2) modbusStopBitsConfig = MODBUS_STOP_BITS_DEFAULT;
  if (modbusUnlockTimeoutSecConfig < 5 || modbusUnlockTimeoutSecConfig > 300) modbusUnlockTimeoutSecConfig = MODBUS_UNLOCK_TIMEOUT_DEFAULT;
  if (localButtonPriorityConfig > 1) localButtonPriorityConfig = 0;
}

void saveRuntimeConfig(
  int slotNo,
  unsigned long actionMs,
  unsigned long waitMs,
  bool pin4ActiveHigh,
  bool pin2ActiveHigh,
  bool pin22ActiveHigh
) {
  prefs.begin("cfg", false);
  prefs.putInt("slot_no", slotNo);
  prefs.putULong("act_ms", actionMs);
  prefs.putULong("wait_ms", waitMs);
  prefs.putBool("r4_act_hi", pin4ActiveHigh);
  prefs.putBool("r2_act_hi", pin2ActiveHigh);
  prefs.putBool("r22_act_hi", pin22ActiveHigh);
  prefs.end();
}

void saveModbusPrefsOnly(
  uint16_t slaveId,
  uint16_t baudEnum,
  uint16_t parity,
  uint16_t stopBits,
  uint16_t unlockTimeoutSec,
  bool localButtonEnable,
  uint16_t localButtonPriority
) {
  prefs.begin("cfg", false);
  prefs.putUShort("mb_sid", slaveId);
  prefs.putUShort("mb_baud", baudEnum);
  prefs.putUShort("mb_par", parity);
  prefs.putUShort("mb_stop", stopBits);
  prefs.putUShort("mb_ulto", unlockTimeoutSec);
  prefs.putBool("mb_lbtn_en", localButtonEnable);
  prefs.putUShort("mb_lbtn_pr", localButtonPriority);
  prefs.end();
}

void persistAllConfigToPreferences() {
  saveRuntimeConfig(slotNumberConfig, actionDurationMs, buttonWaitTimeoutMs, relayPin4ActiveHigh, relayPin2ActiveHigh, relayPin22ActiveHigh);
  saveModbusPrefsOnly(
    modbusSlaveIdConfig,
    modbusBaudEnumConfig,
    modbusParityConfig,
    modbusStopBitsConfig,
    modbusUnlockTimeoutSecConfig,
    localButtonEnableConfig,
    localButtonPriorityConfig
  );
  ModbusHandler::markConfigSaved();
}

void publishModbusSnapshot() {
  ModbusHandler::ConfigState cfg = {};
  ModbusHandler::getConfigState(&cfg);
  slotNumberConfig = (int)cfg.slotNumber;
  actionDurationMs = cfg.actionMs;
  buttonWaitTimeoutMs = cfg.waitMs;
  relayPin4ActiveHigh = cfg.relay4ActiveHigh;
  relayPin2ActiveHigh = cfg.relay2ActiveHigh;
  relayPin22ActiveHigh = cfg.relay22ActiveHigh;
  modbusSlaveIdConfig = cfg.slaveId;
  modbusBaudEnumConfig = cfg.baudEnum;
  modbusParityConfig = cfg.parity;
  modbusStopBitsConfig = cfg.stopBits;
  modbusUnlockTimeoutSecConfig = cfg.unlockTimeoutSec;
  localButtonEnableConfig = cfg.localButtonEnable;
  localButtonPriorityConfig = cfg.localButtonPriority;

  ModbusHandler::updateBasicStatus((uint16_t)slotNumberConfig, millis());

  bool tagInRange = digitalRead(RANGE_PIN) == HIGH;
  bool buttonPressed = localButtonEnableConfig && (digitalRead(BUTTON_PIN) == LOW);
  uint32_t actionRemainMs = 0;
  if (actionActive) {
    unsigned long elapsed = millis() - actionStartMs;
    if (elapsed < actionDurationMs) actionRemainMs = actionDurationMs - elapsed;
  }

  bool rfidValid = lastCardRfidTag.length() > 0;
  ModbusHandler::updateRuntimeStatus(
    tagInRange,
    buttonPressed,
    solenoidState,
    actionActive,
    ModbusHandler::isConfigDirty(),
    modbusFaultActive,
    ModbusHandler::isUnlockValid(),
    rfidValid,
    lastCardRfidTag,
    modbusLastEventCode,
    modbusFaultCode,
    actionRemainMs
  );
  ModbusHandler::task();
}

void processModbusCommands() {
  ModbusHandler::CommandState cmd = {};
  if (!ModbusHandler::consumeCommands(&cmd)) return;

  if (cmd.configWriteLockedFault) {
    setModbusFault(MODBUS_FAULT_CFG_LOCKED, "Modbus config write ditolak: unlock belum valid");
  }

  if (cmd.configInvalidRangeFault) {
    setModbusFault(MODBUS_FAULT_CFG_INVALID_RANGE, "Modbus config write ditolak: nilai di luar range");
  }

  ModbusHandler::ConfigState cfg = {};
  ModbusHandler::getConfigState(&cfg);
  slotNumberConfig = (int)cfg.slotNumber;
  actionDurationMs = cfg.actionMs;
  buttonWaitTimeoutMs = cfg.waitMs;
  relayPin4ActiveHigh = cfg.relay4ActiveHigh;
  relayPin2ActiveHigh = cfg.relay2ActiveHigh;
  relayPin22ActiveHigh = cfg.relay22ActiveHigh;
  modbusSlaveIdConfig = cfg.slaveId;
  modbusBaudEnumConfig = cfg.baudEnum;
  modbusParityConfig = cfg.parity;
  modbusStopBitsConfig = cfg.stopBits;
  modbusUnlockTimeoutSecConfig = cfg.unlockTimeoutSec;
  localButtonEnableConfig = cfg.localButtonEnable;
  localButtonPriorityConfig = cfg.localButtonPriority;

  if (cmd.hasSolenoidCommand) {
    solenoidState = cmd.solenoidValue;
    setActionLed(solenoidState);
    setMirrorLed(solenoidState);
    if (solenoidState) {
      actionActive = true;
      actionStartMs = millis();
      setModbusEventCode(MODBUS_EVENT_SOLENOID_ON);
    } else {
      actionActive = false;
      setModbusEventCode(MODBUS_EVENT_SOLENOID_OFF);
    }
  }

  if (cmd.resetFaultPulse) {
    clearModbusFault("Modbus CMD: reset fault");
  }

  if (cmd.saveConfigPulse) {
    persistAllConfigToPreferences();
    Serial.println("Modbus CMD: save config ke Preferences");
  }

  if (cmd.clearRfidPulse) {
    tagID = "";
    lastCardRfidTag = "";
    lastStatusRfidTag = "";
    currentBikeId = "";
    if (!tagDetected) setStatusLed(false);
    Serial.println("Modbus CMD: clear RFID buffer");
  }

  if (cmd.rebootPulse) {
    if (ModbusHandler::isConfigDirty()) {
      persistAllConfigToPreferences();
      Serial.println("Modbus reboot: config dirty disimpan otomatis");
    }
    Serial.println("Modbus CMD: reboot");
    delay(100);
    ESP.restart();
  }
}

void runRuntimeLoop() {
  bool inRange = digitalRead(RANGE_PIN) == HIGH;
  if (inRange && !tagDetected) {
    tagDetected = true;
    confirmOpenSentInCurrentRange = false;
    setModbusEventCode(MODBUS_EVENT_TAG_ENTER);
    Serial.println("TAG MASUK RANGE");
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
      setModbusEventCode(MODBUS_EVENT_RFID_VALID);
    }
  }

  if (!inRange && tagDetected) {
    tagDetected = false;
    confirmOpenSentInCurrentRange = false;
    setModbusEventCode(MODBUS_EVENT_TAG_LEAVE);
    setStatusLed(false);
  }

  bool buttonPressed = localButtonEnableConfig && (digitalRead(BUTTON_PIN) == LOW);
  digitalWrite(LED_BUTTON, buttonPressed ? LOW : HIGH);

  if (actionActive && (millis() - actionStartMs >= actionDurationMs)) {
    actionActive = false;
    setActionLed(false);
    setMirrorLed(false);
    solenoidState = false;
    setModbusEventCode(MODBUS_EVENT_SOLENOID_OFF);
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
    setModbusEventCode(MODBUS_EVENT_BUTTON_PRESS);
    buttonWaitActive = false;
    buttonWaitStartMs = 0;
    actionActive = true;
    actionStartMs = millis();
    setActionLed(true);
    setMirrorLed(true);
    solenoidState = true;
    setModbusEventCode(MODBUS_EVENT_SOLENOID_ON);
    currentBikeId = "";
  }

  lastButtonState = buttonPressed;
  delay(50);
}

void setup() {
  Serial.begin(115200);
  RFIDSerial.begin(9600, SERIAL_8N1, RX_PIN, -1);
  loadConfig();

  pinMode(RANGE_PIN, INPUT);
  pinMode(LED_STATUS, OUTPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(LED_BUTTON, OUTPUT);
  pinMode(LED_ACTION, OUTPUT);
  pinMode(LED_ACTION_INV, OUTPUT);

  setStatusLed(false);
  digitalWrite(LED_BUTTON, HIGH);
  setActionLed(false);
  setMirrorLed(false);

  ModbusHandler::ConfigState modbusCfg = {};
  modbusCfg.slotNumber = (uint16_t)slotNumberConfig;
  modbusCfg.actionMs = actionDurationMs;
  modbusCfg.waitMs = buttonWaitTimeoutMs;
  modbusCfg.relay4ActiveHigh = relayPin4ActiveHigh;
  modbusCfg.relay2ActiveHigh = relayPin2ActiveHigh;
  modbusCfg.relay22ActiveHigh = relayPin22ActiveHigh;
  modbusCfg.slaveId = modbusSlaveIdConfig;
  modbusCfg.baudEnum = modbusBaudEnumConfig;
  modbusCfg.parity = modbusParityConfig;
  modbusCfg.stopBits = modbusStopBitsConfig;
  modbusCfg.applySerialNow = 0;
  modbusCfg.localButtonEnable = localButtonEnableConfig;
  modbusCfg.localButtonPriority = localButtonPriorityConfig;
  modbusCfg.unlockTimeoutSec = modbusUnlockTimeoutSecConfig;

  bool modbusReady = ModbusHandler::begin((uint8_t)modbusSlaveIdConfig, modbusCfg);
  modbusLastEventCode = MODBUS_EVENT_BOOT;
  Serial.println(modbusReady ? "Modbus RTU siap" : "Modbus RTU gagal init");
  Serial.println("Firmware mode: Modbus-only");
}

void loop() {
  publishModbusSnapshot();
  processModbusCommands();
  runRuntimeLoop();
}
