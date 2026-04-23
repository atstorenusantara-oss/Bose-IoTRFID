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

void encodeAscii12ToRegs(const String& text, uint16_t outRegs[6]) {
  if (!outRegs) return;

  char buf[13] = {0};
  text.toCharArray(buf, sizeof(buf));
  for (int i = 0; i < 6; i++) {
    uint8_t hi = (uint8_t)buf[i * 2];
    uint8_t lo = (uint8_t)buf[i * 2 + 1];
    outRegs[i] = (uint16_t)(((uint16_t)hi << 8) | lo);
  }
}

} // namespace ModbusMap
