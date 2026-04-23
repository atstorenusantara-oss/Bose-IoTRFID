#pragma once

#include <Arduino.h>

namespace ModbusHandler {

bool begin(uint8_t slaveId, uint16_t initialSlotNumber);
void updateBasicStatus(uint16_t slotNumber, uint32_t uptimeMs);
void task();
bool isReady();

} // namespace ModbusHandler
