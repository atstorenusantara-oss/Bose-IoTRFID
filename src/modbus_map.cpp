#include "modbus_map.h"

namespace {
constexpr uint16_t FW_VERSION_MAJOR = 2;
constexpr uint16_t FW_VERSION_MINOR = 0;
constexpr uint16_t FW_VERSION_PATCH = 0;
}

namespace ModbusMap {

void fillBasicSnapshot(BasicSnapshot* out, uint16_t slotNumber, uint32_t uptimeMs) {
  if (!out) return;
  out->slotNumber = slotNumber;
  out->uptimeMs = uptimeMs;
  out->fwMajor = FW_VERSION_MAJOR;
  out->fwMinor = FW_VERSION_MINOR;
  out->fwPatch = FW_VERSION_PATCH;
}

} // namespace ModbusMap
