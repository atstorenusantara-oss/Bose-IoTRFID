#include "modbus_handler.h"

#include <ModbusRTU.h>

#include "modbus_map.h"
#include "rs485_hal.h"

namespace {
ModbusRTU gModbus;
bool gReady = false;

constexpr int8_t RS485_RX_PIN = 25;
constexpr int8_t RS485_TX_PIN = 26;
constexpr int8_t RS485_DERE_PIN = 21;
constexpr uint32_t MODBUS_BAUD_DEFAULT = 9600;
}

namespace ModbusHandler {

bool begin(uint8_t slaveId, uint16_t initialSlotNumber) {
  Rs485Hal::begin(MODBUS_BAUD_DEFAULT, SERIAL_8N1, RS485_RX_PIN, RS485_TX_PIN, RS485_DERE_PIN);

  if (!gModbus.begin(&Rs485Hal::stream(), Rs485Hal::deRePin())) {
    gReady = false;
    return false;
  }

  gModbus.slave(slaveId);
  gModbus.addIsts(ModbusMap::ISTS_STS_TAG_IN_RANGE, false);
  gModbus.addIsts(ModbusMap::ISTS_STS_BUTTON_PRESSED, false);
  gModbus.addIsts(ModbusMap::ISTS_STS_SOLENOID_ACTIVE, false);
  gModbus.addIsts(ModbusMap::ISTS_STS_ACTION_ACTIVE, false);
  gModbus.addIsts(ModbusMap::ISTS_STS_CONFIG_DIRTY, false);
  gModbus.addIsts(ModbusMap::ISTS_STS_FAULT_ACTIVE, false);
  gModbus.addIsts(ModbusMap::ISTS_STS_UNLOCK_VALID, false);
  gModbus.addIsts(ModbusMap::ISTS_STS_RFID_VALID, false);
  gModbus.addIreg(ModbusMap::IREG_STS_SLOT_NUMBER, initialSlotNumber);
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

  updateBasicStatus(initialSlotNumber, 0);
  updateRuntimeStatus(false, false, false, false, false, false, false, false, "", 1, 0, 0);
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

void task() {
  if (!gReady) return;
  gModbus.task();
}

bool isReady() {
  return gReady;
}

} // namespace ModbusHandler
