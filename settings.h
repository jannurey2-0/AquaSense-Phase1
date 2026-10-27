#pragma once

#include <Arduino.h>

struct DeviceConfig {
  String   deviceId;
  String   ingestUrl;
  String   apiKey;
  uint32_t intervalS;
};

struct Calibration {
  // pH: two buffer points, voltages stored as ESP32 ADC millivolts
  float phValue1;
  float phMv1;
  float phValue2;
  float phMv2;
  bool  phPoint1Set;
  bool  phPoint2Set;

  float tdsK;
  float turbOffsetNtu;
  float tempOffsetC;
};

namespace settings {

DeviceConfig &device();
Calibration  &cal();

void load();
void saveDevice();
void saveCal();
void resetCal();

bool  phIsCalibrated();
float phFromMv(float adcMv);
float turbidityFromSensorVolts(float sensorVolts);
float tdsFromSensorVolts(float sensorVolts, float tempC, bool applyK = true);

}  // namespace settings
