#pragma once

#include <Arduino.h>

namespace ModbusHandler {

bool begin(uint8_t slaveId, uint16_t initialSlotNumber);
void updateBasicStatus(uint16_t slotNumber, uint32_t uptimeMs);
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
);
void task();
bool isReady();

} // namespace ModbusHandler
