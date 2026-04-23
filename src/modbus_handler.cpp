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
  gModbus.addIreg(ModbusMap::IREG_STS_SLOT_NUMBER, initialSlotNumber);
  gModbus.addIreg(ModbusMap::IREG_STS_UPTIME_MS_LO, 0);
  gModbus.addIreg(ModbusMap::IREG_STS_UPTIME_MS_HI, 0);
  gModbus.addIreg(ModbusMap::IREG_STS_FW_VERSION_MAJOR, 0);
  gModbus.addIreg(ModbusMap::IREG_STS_FW_VERSION_MINOR, 0);
  gModbus.addIreg(ModbusMap::IREG_STS_FW_VERSION_PATCH, 0);

  updateBasicStatus(initialSlotNumber, 0);
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

void task() {
  if (!gReady) return;
  gModbus.task();
}

bool isReady() {
  return gReady;
}

} // namespace ModbusHandler
