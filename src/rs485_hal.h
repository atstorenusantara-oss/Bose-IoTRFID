#pragma once

#include <Arduino.h>

namespace Rs485Hal {

void begin(uint32_t baud, uint32_t serialConfig, int8_t rxPin, int8_t txPin, int8_t deRePin);
HardwareSerial& stream();
int8_t deRePin();

} // namespace Rs485Hal
