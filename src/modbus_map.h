#pragma once

#include <Arduino.h>

namespace ModbusMap {

constexpr uint16_t COIL_CMD_SOLENOID = 0;
constexpr uint16_t COIL_CMD_RESET_FAULT = 1;
constexpr uint16_t COIL_CMD_SAVE_CONFIG = 2;
constexpr uint16_t COIL_CMD_REBOOT = 3;
constexpr uint16_t COIL_CMD_CLEAR_RFID_BUFFER = 4;

constexpr uint16_t ISTS_STS_TAG_IN_RANGE = 0;
constexpr uint16_t ISTS_STS_BUTTON_PRESSED = 1;
constexpr uint16_t ISTS_STS_SOLENOID_ACTIVE = 2;
constexpr uint16_t ISTS_STS_ACTION_ACTIVE = 3;
constexpr uint16_t ISTS_STS_CONFIG_DIRTY = 4;
constexpr uint16_t ISTS_STS_FAULT_ACTIVE = 5;
constexpr uint16_t ISTS_STS_UNLOCK_VALID = 6;
constexpr uint16_t ISTS_STS_RFID_VALID = 7;

constexpr uint16_t IREG_STS_SLOT_NUMBER = 0;
constexpr uint16_t IREG_STS_LAST_EVENT_CODE = 1;
constexpr uint16_t IREG_STS_FAULT_CODE = 2;
constexpr uint16_t IREG_STS_UPTIME_MS_LO = 3;
constexpr uint16_t IREG_STS_UPTIME_MS_HI = 4;
constexpr uint16_t IREG_STS_ACTION_REMAIN_MS_LO = 5;
constexpr uint16_t IREG_STS_ACTION_REMAIN_MS_HI = 6;
constexpr uint16_t IREG_STS_LAST_RFID_ASCII_0 = 7;
constexpr uint16_t IREG_STS_LAST_RFID_ASCII_1 = 8;
constexpr uint16_t IREG_STS_LAST_RFID_ASCII_2 = 9;
constexpr uint16_t IREG_STS_LAST_RFID_ASCII_3 = 10;
constexpr uint16_t IREG_STS_LAST_RFID_ASCII_4 = 11;
constexpr uint16_t IREG_STS_LAST_RFID_ASCII_5 = 12;
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
void encodeAscii12ToRegs(const String& text, uint16_t outRegs[6]);

} // namespace ModbusMap
