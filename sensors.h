#pragma once

#include <Arduino.h>

struct SensorReading {
  float ph;
  float tempC;
  float tdsPpm;
  float turbidityNtu;

  // Raw ADC-side millivolts, kept for calibration and diagnostics
  float phMv;
  float turbMv;
  float tdsMv;

  bool phOk;
  bool tempOk;
  bool tdsOk;
  bool turbidityOk;
  bool tdsSaturated;
};

namespace sensors {

void begin();
SensorReading readAll();

}  // namespace sensors
