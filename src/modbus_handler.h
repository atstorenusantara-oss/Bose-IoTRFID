#pragma once

#include <Arduino.h>

namespace ModbusHandler {

struct ConfigState {
  uint16_t slotNumber;
  uint32_t actionMs;
  uint32_t waitMs;
  bool relay4ActiveHigh;
  bool relay2ActiveHigh;
  bool relay22ActiveHigh;
  uint16_t slaveId;
  uint16_t baudEnum;
  uint16_t parity;
  uint16_t stopBits;
  uint16_t applySerialNow;
  bool localButtonEnable;
  uint16_t localButtonPriority;
  uint16_t unlockTimeoutSec;
};

struct CommandState {
  bool hasSolenoidCommand;
  bool solenoidValue;
  bool resetFaultPulse;
  bool saveConfigPulse;
  bool rebootPulse;
  bool clearRfidPulse;
  bool configWriteLockedFault;
  bool configInvalidRangeFault;
};

bool begin(uint8_t slaveId, const ConfigState& initialConfig);
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
bool consumeCommands(CommandState* out);
void getConfigState(ConfigState* out);
bool isConfigDirty();
bool isUnlockValid();
void markConfigSaved();
void task();
bool isReady();

} // namespace ModbusHandler
