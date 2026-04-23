#pragma once

#include <Arduino.h>

namespace ModbusMap {

constexpr uint16_t IREG_STS_SLOT_NUMBER = 0;
constexpr uint16_t IREG_STS_UPTIME_MS_LO = 3;
constexpr uint16_t IREG_STS_UPTIME_MS_HI = 4;
constexpr uint16_t IREG_STS_FW_VERSION_MAJOR = 13;
constexpr uint16_t IREG_STS_FW_VERSION_MINOR = 14;
constexpr uint16_t IREG_STS_FW_VERSION_PATCH = 15;

struct BasicSnapshot {
  uint16_t slotNumber;
  uint32_t uptimeMs;
  uint16_t fwMajor;
  uint16_t fwMinor;
  uint16_t fwPatch;
};

void fillBasicSnapshot(BasicSnapshot* out, uint16_t slotNumber, uint32_t uptimeMs);

} // namespace ModbusMap
