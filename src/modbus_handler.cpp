#include "modbus_handler.h"

#include <ModbusRTU.h>

#include "modbus_map.h"
#include "rs485_hal.h"

namespace {
ModbusRTU gModbus;
bool gReady = false;
ModbusHandler::CommandState gPendingCommands = {};
ModbusHandler::ConfigState gConfig = {};
bool gConfigDirty = false;
bool gUnlockValid = false;
unsigned long gUnlockUntilMs = 0;
bool gLastSolenoidCoil = false;
bool gLastResetFaultCoil = false;
bool gLastSaveConfigCoil = false;
bool gLastRebootCoil = false;
bool gLastClearRfidCoil = false;

constexpr int8_t RS485_RX_PIN = 25;
constexpr int8_t RS485_TX_PIN = 26;
constexpr int8_t RS485_DERE_PIN = 21;
constexpr uint32_t MODBUS_BAUD_DEFAULT = 9600;

constexpr uint16_t MIN_SLOT_NUMBER = 1;
constexpr uint16_t MAX_SLOT_NUMBER = 64;
constexpr uint32_t MIN_ACTION_MS = 1000;
constexpr uint32_t MAX_ACTION_MS = 120000;
constexpr uint32_t MIN_WAIT_MS = 1000;
constexpr uint32_t MAX_WAIT_MS = 300000;
constexpr uint16_t MIN_SLAVE_ID = 1;
constexpr uint16_t MAX_SLAVE_ID = 247;
constexpr uint16_t MAX_BAUD_ENUM = 4;
constexpr uint16_t MAX_PARITY_ENUM = 2;
constexpr uint16_t MIN_UNLOCK_TIMEOUT_SEC = 5;
constexpr uint16_t MAX_UNLOCK_TIMEOUT_SEC = 300;

uint16_t boolToReg(bool v) {
  return v ? 1 : 0;
}

bool regIsBool(uint16_t v) {
  return v == 0 || v == 1;
}

uint32_t joinReg32(uint16_t lo, uint16_t hi) {
  return ((uint32_t)hi << 16) | lo;
}

void splitReg32(uint32_t value, uint16_t* lo, uint16_t* hi) {
  if (lo) *lo = (uint16_t)(value & 0xFFFF);
  if (hi) *hi = (uint16_t)((value >> 16) & 0xFFFF);
}

bool configEquals(const ModbusHandler::ConfigState& a, const ModbusHandler::ConfigState& b) {
  return a.slotNumber == b.slotNumber &&
         a.actionMs == b.actionMs &&
         a.waitMs == b.waitMs &&
         a.relay4ActiveHigh == b.relay4ActiveHigh &&
         a.relay2ActiveHigh == b.relay2ActiveHigh &&
         a.relay22ActiveHigh == b.relay22ActiveHigh &&
         a.slaveId == b.slaveId &&
         a.baudEnum == b.baudEnum &&
         a.parity == b.parity &&
         a.stopBits == b.stopBits &&
         a.applySerialNow == b.applySerialNow &&
         a.localButtonEnable == b.localButtonEnable &&
         a.localButtonPriority == b.localButtonPriority &&
         a.unlockTimeoutSec == b.unlockTimeoutSec;
}

void writeConfigToHregs(const ModbusHandler::ConfigState& cfg) {
  uint16_t lo = 0;
  uint16_t hi = 0;

  gModbus.Hreg(ModbusMap::HREG_CFG_SLOT_NUMBER, cfg.slotNumber);

  splitReg32(cfg.actionMs, &lo, &hi);
  gModbus.Hreg(ModbusMap::HREG_CFG_ACTION_MS_LO, lo);
  gModbus.Hreg(ModbusMap::HREG_CFG_ACTION_MS_HI, hi);

  splitReg32(cfg.waitMs, &lo, &hi);
  gModbus.Hreg(ModbusMap::HREG_CFG_WAIT_MS_LO, lo);
  gModbus.Hreg(ModbusMap::HREG_CFG_WAIT_MS_HI, hi);

  gModbus.Hreg(ModbusMap::HREG_CFG_R4_ACTIVE_HIGH, boolToReg(cfg.relay4ActiveHigh));
  gModbus.Hreg(ModbusMap::HREG_CFG_R2_ACTIVE_HIGH, boolToReg(cfg.relay2ActiveHigh));
  gModbus.Hreg(ModbusMap::HREG_CFG_R22_ACTIVE_HIGH, boolToReg(cfg.relay22ActiveHigh));
  gModbus.Hreg(ModbusMap::HREG_CFG_MODBUS_SLAVE_ID, cfg.slaveId);
  gModbus.Hreg(ModbusMap::HREG_CFG_MODBUS_BAUD_ENUM, cfg.baudEnum);
  gModbus.Hreg(ModbusMap::HREG_CFG_MODBUS_PARITY, cfg.parity);
  gModbus.Hreg(ModbusMap::HREG_CFG_MODBUS_STOP_BITS, cfg.stopBits);
  gModbus.Hreg(ModbusMap::HREG_CFG_APPLY_SERIAL_NOW, cfg.applySerialNow);
  gModbus.Hreg(ModbusMap::HREG_CFG_LOCAL_BUTTON_ENABLE, boolToReg(cfg.localButtonEnable));
  gModbus.Hreg(ModbusMap::HREG_CFG_LOCAL_BUTTON_PRIORITY, cfg.localButtonPriority);
  gModbus.Hreg(ModbusMap::HREG_CFG_UNLOCK_TIMEOUT_SEC, cfg.unlockTimeoutSec);
}

ModbusHandler::ConfigState readConfigFromHregs() {
  ModbusHandler::ConfigState cfg = {};
  cfg.slotNumber = gModbus.Hreg(ModbusMap::HREG_CFG_SLOT_NUMBER);
  cfg.actionMs = joinReg32(gModbus.Hreg(ModbusMap::HREG_CFG_ACTION_MS_LO), gModbus.Hreg(ModbusMap::HREG_CFG_ACTION_MS_HI));
  cfg.waitMs = joinReg32(gModbus.Hreg(ModbusMap::HREG_CFG_WAIT_MS_LO), gModbus.Hreg(ModbusMap::HREG_CFG_WAIT_MS_HI));
  cfg.relay4ActiveHigh = gModbus.Hreg(ModbusMap::HREG_CFG_R4_ACTIVE_HIGH) == 1;
  cfg.relay2ActiveHigh = gModbus.Hreg(ModbusMap::HREG_CFG_R2_ACTIVE_HIGH) == 1;
  cfg.relay22ActiveHigh = gModbus.Hreg(ModbusMap::HREG_CFG_R22_ACTIVE_HIGH) == 1;
  cfg.slaveId = gModbus.Hreg(ModbusMap::HREG_CFG_MODBUS_SLAVE_ID);
  cfg.baudEnum = gModbus.Hreg(ModbusMap::HREG_CFG_MODBUS_BAUD_ENUM);
  cfg.parity = gModbus.Hreg(ModbusMap::HREG_CFG_MODBUS_PARITY);
  cfg.stopBits = gModbus.Hreg(ModbusMap::HREG_CFG_MODBUS_STOP_BITS);
  cfg.applySerialNow = gModbus.Hreg(ModbusMap::HREG_CFG_APPLY_SERIAL_NOW);
  cfg.localButtonEnable = gModbus.Hreg(ModbusMap::HREG_CFG_LOCAL_BUTTON_ENABLE) == 1;
  cfg.localButtonPriority = gModbus.Hreg(ModbusMap::HREG_CFG_LOCAL_BUTTON_PRIORITY);
  cfg.unlockTimeoutSec = gModbus.Hreg(ModbusMap::HREG_CFG_UNLOCK_TIMEOUT_SEC);
  return cfg;
}

bool isConfigRangeValid(const ModbusHandler::ConfigState& cfg) {
  if (cfg.slotNumber < MIN_SLOT_NUMBER || cfg.slotNumber > MAX_SLOT_NUMBER) return false;
  if (cfg.actionMs < MIN_ACTION_MS || cfg.actionMs > MAX_ACTION_MS) return false;
  if (cfg.waitMs < MIN_WAIT_MS || cfg.waitMs > MAX_WAIT_MS) return false;
  if (cfg.slaveId < MIN_SLAVE_ID || cfg.slaveId > MAX_SLAVE_ID) return false;
  if (cfg.baudEnum > MAX_BAUD_ENUM) return false;
  if (cfg.parity > MAX_PARITY_ENUM) return false;
  if (cfg.stopBits != 1 && cfg.stopBits != 2) return false;
  if (cfg.localButtonPriority > 1) return false;
  if (cfg.applySerialNow > 1) return false;
  if (cfg.unlockTimeoutSec < MIN_UNLOCK_TIMEOUT_SEC || cfg.unlockTimeoutSec > MAX_UNLOCK_TIMEOUT_SEC) return false;
  if (!regIsBool(boolToReg(cfg.relay4ActiveHigh))) return false;
  if (!regIsBool(boolToReg(cfg.relay2ActiveHigh))) return false;
  if (!regIsBool(boolToReg(cfg.relay22ActiveHigh))) return false;
  if (!regIsBool(boolToReg(cfg.localButtonEnable))) return false;
  return true;
}

void processUnlockWindow() {
  unsigned long now = millis();
  if (gUnlockValid && (long)(now - gUnlockUntilMs) >= 0) {
    gUnlockValid = false;
  }

  uint16_t unlockKey = gModbus.Hreg(ModbusMap::HREG_CFG_WRITE_UNLOCK_KEY);
  if (unlockKey == ModbusMap::CFG_UNLOCK_MAGIC) {
    uint16_t timeoutSec = gModbus.Hreg(ModbusMap::HREG_CFG_UNLOCK_TIMEOUT_SEC);
    if (timeoutSec < MIN_UNLOCK_TIMEOUT_SEC || timeoutSec > MAX_UNLOCK_TIMEOUT_SEC) {
      timeoutSec = gConfig.unlockTimeoutSec;
      gModbus.Hreg(ModbusMap::HREG_CFG_UNLOCK_TIMEOUT_SEC, timeoutSec);
    }
    gUnlockValid = true;
    gUnlockUntilMs = now + ((unsigned long)timeoutSec * 1000UL);
  }

  if (unlockKey != 0) {
    gModbus.Hreg(ModbusMap::HREG_CFG_WRITE_UNLOCK_KEY, 0);
  }
}

void processConfigWrites() {
  ModbusHandler::ConfigState candidate = readConfigFromHregs();
  if (configEquals(candidate, gConfig)) return;

  if (!gUnlockValid) {
    gPendingCommands.configWriteLockedFault = true;
    writeConfigToHregs(gConfig);
    return;
  }

  if (!isConfigRangeValid(candidate)) {
    gPendingCommands.configInvalidRangeFault = true;
    writeConfigToHregs(gConfig);
    return;
  }

  gConfig = candidate;
  gConfigDirty = true;
}

void handlePulseCoil(uint16_t offset, bool& lastState, bool* pulseFlag) {
  bool current = gModbus.Coil(offset);
  if (current && !lastState && pulseFlag) {
    *pulseFlag = true;
  }
  if (current) {
    gModbus.Coil(offset, false);
    current = false;
  }
  lastState = current;
}
} // namespace

namespace ModbusHandler {

bool begin(uint8_t slaveId, const ConfigState& initialConfig) {
  Rs485Hal::begin(MODBUS_BAUD_DEFAULT, SERIAL_8N1, RS485_RX_PIN, RS485_TX_PIN, RS485_DERE_PIN);

  if (!gModbus.begin(&Rs485Hal::stream(), Rs485Hal::deRePin())) {
    gReady = false;
    return false;
  }

  gConfig = initialConfig;
  if (!isConfigRangeValid(gConfig)) {
    gConfig.slotNumber = 1;
    gConfig.actionMs = 10000;
    gConfig.waitMs = 30000;
    gConfig.relay4ActiveHigh = true;
    gConfig.relay2ActiveHigh = true;
    gConfig.relay22ActiveHigh = false;
    gConfig.slaveId = 1;
    gConfig.baudEnum = 0;
    gConfig.parity = 0;
    gConfig.stopBits = 1;
    gConfig.applySerialNow = 0;
    gConfig.localButtonEnable = true;
    gConfig.localButtonPriority = 0;
    gConfig.unlockTimeoutSec = 30;
  }

  gModbus.slave(slaveId);
  gModbus.addCoil(ModbusMap::COIL_CMD_SOLENOID, false);
  gModbus.addCoil(ModbusMap::COIL_CMD_RESET_FAULT, false);
  gModbus.addCoil(ModbusMap::COIL_CMD_SAVE_CONFIG, false);
  gModbus.addCoil(ModbusMap::COIL_CMD_REBOOT, false);
  gModbus.addCoil(ModbusMap::COIL_CMD_CLEAR_RFID_BUFFER, false);

  gModbus.addIsts(ModbusMap::ISTS_STS_TAG_IN_RANGE, false);
  gModbus.addIsts(ModbusMap::ISTS_STS_BUTTON_PRESSED, false);
  gModbus.addIsts(ModbusMap::ISTS_STS_SOLENOID_ACTIVE, false);
  gModbus.addIsts(ModbusMap::ISTS_STS_ACTION_ACTIVE, false);
  gModbus.addIsts(ModbusMap::ISTS_STS_CONFIG_DIRTY, false);
  gModbus.addIsts(ModbusMap::ISTS_STS_FAULT_ACTIVE, false);
  gModbus.addIsts(ModbusMap::ISTS_STS_UNLOCK_VALID, false);
  gModbus.addIsts(ModbusMap::ISTS_STS_RFID_VALID, false);

  gModbus.addIreg(ModbusMap::IREG_STS_SLOT_NUMBER, gConfig.slotNumber);
  gModbus.addIreg(ModbusMap::IREG_STS_LAST_EVENT_CODE, 0);
  gModbus.addIreg(ModbusMap::IREG_STS_FAULT_CODE, 0);
  gModbus.addIreg(ModbusMap::IREG_STS_UPTIME_MS_LO, 0);
  gModbus.addIreg(ModbusMap::IREG_STS_UPTIME_MS_HI, 0);
  gModbus.addIreg(ModbusMap::IREG_STS_ACTION_REMAIN_MS_LO, 0);
  gModbus.addIreg(ModbusMap::IREG_STS_ACTION_REMAIN_MS_HI, 0);
  gModbus.addIreg(ModbusMap::IREG_STS_LAST_RFID_ASCII_0, 0, 6);
  gModbus.addIreg(ModbusMap::IREG_STS_FW_VERSION_MAJOR, 0);
  gModbus.addIreg(ModbusMap::IREG_STS_FW_VERSION_MINOR, 0);
  gModbus.addIreg(ModbusMap::IREG_STS_FW_VERSION_PATCH, 0);

  gModbus.addHreg(ModbusMap::HREG_CFG_SLOT_NUMBER, gConfig.slotNumber);
  gModbus.addHreg(ModbusMap::HREG_CFG_ACTION_MS_LO, 0);
  gModbus.addHreg(ModbusMap::HREG_CFG_ACTION_MS_HI, 0);
  gModbus.addHreg(ModbusMap::HREG_CFG_WAIT_MS_LO, 0);
  gModbus.addHreg(ModbusMap::HREG_CFG_WAIT_MS_HI, 0);
  gModbus.addHreg(ModbusMap::HREG_CFG_R4_ACTIVE_HIGH, boolToReg(gConfig.relay4ActiveHigh));
  gModbus.addHreg(ModbusMap::HREG_CFG_R2_ACTIVE_HIGH, boolToReg(gConfig.relay2ActiveHigh));
  gModbus.addHreg(ModbusMap::HREG_CFG_R22_ACTIVE_HIGH, boolToReg(gConfig.relay22ActiveHigh));
  gModbus.addHreg(ModbusMap::HREG_CFG_MODBUS_SLAVE_ID, gConfig.slaveId);
  gModbus.addHreg(ModbusMap::HREG_CFG_MODBUS_BAUD_ENUM, gConfig.baudEnum);
  gModbus.addHreg(ModbusMap::HREG_CFG_MODBUS_PARITY, gConfig.parity);
  gModbus.addHreg(ModbusMap::HREG_CFG_MODBUS_STOP_BITS, gConfig.stopBits);
  gModbus.addHreg(ModbusMap::HREG_CFG_APPLY_SERIAL_NOW, gConfig.applySerialNow);
  gModbus.addHreg(ModbusMap::HREG_CFG_LOCAL_BUTTON_ENABLE, boolToReg(gConfig.localButtonEnable));
  gModbus.addHreg(ModbusMap::HREG_CFG_LOCAL_BUTTON_PRIORITY, gConfig.localButtonPriority);
  gModbus.addHreg(ModbusMap::HREG_CFG_WRITE_UNLOCK_KEY, 0);
  gModbus.addHreg(ModbusMap::HREG_CFG_UNLOCK_TIMEOUT_SEC, gConfig.unlockTimeoutSec);
  writeConfigToHregs(gConfig);

  updateBasicStatus(gConfig.slotNumber, 0);
  updateRuntimeStatus(false, false, false, false, false, false, false, false, "", 1, 0, 0);

  gPendingCommands = {};
  gConfigDirty = false;
  gUnlockValid = false;
  gUnlockUntilMs = 0;
  gLastSolenoidCoil = false;
  gLastResetFaultCoil = false;
  gLastSaveConfigCoil = false;
  gLastRebootCoil = false;
  gLastClearRfidCoil = false;

  gReady = true;
  return true;
}

void updateBasicStatus(uint16_t slotNumber, uint32_t uptimeMs) {
  if (!gReady) return;

  ModbusMap::BasicSnapshot snap = {};
  ModbusMap::fillBasicSnapshot(&snap, slotNumber, uptimeMs);

  gModbus.Ireg(ModbusMap::IREG_STS_SLOT_NUMBER, snap.slotNumber);
  gModbus.Ireg(ModbusMap::IREG_STS_UPTIME_MS_LO, (uint16_t)(snap.uptimeMs & 0xFFFF));
  gModbus.Ireg(ModbusMap::IREG_STS_UPTIME_MS_HI, (uint16_t)((snap.uptimeMs >> 16) & 0xFFFF));
  gModbus.Ireg(ModbusMap::IREG_STS_FW_VERSION_MAJOR, snap.fwMajor);
  gModbus.Ireg(ModbusMap::IREG_STS_FW_VERSION_MINOR, snap.fwMinor);
  gModbus.Ireg(ModbusMap::IREG_STS_FW_VERSION_PATCH, snap.fwPatch);
}

void updateRuntimeStatus(
  bool tagInRange,
  bool buttonPressed,
  bool solenoidActive,
  bool actionActive,
  bool configDirty,
  bool faultActive,
  bool unlockValid,
  bool rfidValid,
  const String& lastRfid,
  uint16_t eventCode,
  uint16_t faultCode,
  uint32_t actionRemainMs
) {
  if (!gReady) return;

  gModbus.Ists(ModbusMap::ISTS_STS_TAG_IN_RANGE, tagInRange);
  gModbus.Ists(ModbusMap::ISTS_STS_BUTTON_PRESSED, buttonPressed);
  gModbus.Ists(ModbusMap::ISTS_STS_SOLENOID_ACTIVE, solenoidActive);
  gModbus.Ists(ModbusMap::ISTS_STS_ACTION_ACTIVE, actionActive);
  gModbus.Ists(ModbusMap::ISTS_STS_CONFIG_DIRTY, configDirty);
  gModbus.Ists(ModbusMap::ISTS_STS_FAULT_ACTIVE, faultActive);
  gModbus.Ists(ModbusMap::ISTS_STS_UNLOCK_VALID, unlockValid);
  gModbus.Ists(ModbusMap::ISTS_STS_RFID_VALID, rfidValid);

  gModbus.Ireg(ModbusMap::IREG_STS_LAST_EVENT_CODE, eventCode);
  gModbus.Ireg(ModbusMap::IREG_STS_FAULT_CODE, faultCode);
  gModbus.Ireg(ModbusMap::IREG_STS_ACTION_REMAIN_MS_LO, (uint16_t)(actionRemainMs & 0xFFFF));
  gModbus.Ireg(ModbusMap::IREG_STS_ACTION_REMAIN_MS_HI, (uint16_t)((actionRemainMs >> 16) & 0xFFFF));

  uint16_t rfidRegs[6] = {0};
  ModbusMap::encodeAscii12ToRegs(lastRfid, rfidRegs);
  for (int i = 0; i < 6; i++) {
    gModbus.Ireg((uint16_t)(ModbusMap::IREG_STS_LAST_RFID_ASCII_0 + i), rfidRegs[i]);
  }
}

bool consumeCommands(CommandState* out) {
  if (!out || !gReady) return false;
  *out = gPendingCommands;
  bool hasAny = out->hasSolenoidCommand || out->resetFaultPulse || out->saveConfigPulse ||
                out->rebootPulse || out->clearRfidPulse ||
                out->configWriteLockedFault || out->configInvalidRangeFault;
  gPendingCommands = {};
  return hasAny;
}

void getConfigState(ConfigState* out) {
  if (!out) return;
  *out = gConfig;
}

bool isConfigDirty() {
  return gConfigDirty;
}

bool isUnlockValid() {
  return gUnlockValid;
}

void markConfigSaved() {
  gConfigDirty = false;
}

void task() {
  if (!gReady) return;
  gModbus.task();

  processUnlockWindow();
  processConfigWrites();

  bool solenoidCoil = gModbus.Coil(ModbusMap::COIL_CMD_SOLENOID);
  if (solenoidCoil != gLastSolenoidCoil) {
    gPendingCommands.hasSolenoidCommand = true;
    gPendingCommands.solenoidValue = solenoidCoil;
    gLastSolenoidCoil = solenoidCoil;
  }

  handlePulseCoil(ModbusMap::COIL_CMD_RESET_FAULT, gLastResetFaultCoil, &gPendingCommands.resetFaultPulse);
  handlePulseCoil(ModbusMap::COIL_CMD_SAVE_CONFIG, gLastSaveConfigCoil, &gPendingCommands.saveConfigPulse);
  handlePulseCoil(ModbusMap::COIL_CMD_REBOOT, gLastRebootCoil, &gPendingCommands.rebootPulse);
  handlePulseCoil(ModbusMap::COIL_CMD_CLEAR_RFID_BUFFER, gLastClearRfidCoil, &gPendingCommands.clearRfidPulse);
}

bool isReady() {
  return gReady;
}

} // namespace ModbusHandler
