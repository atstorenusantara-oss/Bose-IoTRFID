#include "rs485_hal.h"

namespace {
HardwareSerial gRs485Serial(1);
int8_t gDeRePin = -1;
}

namespace Rs485Hal {

void begin(uint32_t baud, uint32_t serialConfig, int8_t rxPin, int8_t txPin, int8_t deRePin) {
  gDeRePin = deRePin;
  if (gDeRePin >= 0) {
    pinMode((uint8_t)gDeRePin, OUTPUT);
    digitalWrite((uint8_t)gDeRePin, LOW);
  }
  gRs485Serial.begin(baud, serialConfig, rxPin, txPin);
}

HardwareSerial& stream() {
  return gRs485Serial;
}

int8_t deRePin() {
  return gDeRePin;
}

} // namespace Rs485Hal
