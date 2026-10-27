#include "settings.h"

#include <Preferences.h>
#include <math.h>

#include "config.h"

namespace {

Preferences prefs;

DeviceConfig deviceConfig;
Calibration  calibration;

constexpr const char *NS_DEVICE = "aqua-dev";
constexpr const char *NS_CAL    = "aqua-cal";

void setFactoryDefaults(Calibration &c) {
  // Placeholders derived from a 5 V board through a /1.5 divider. Replace with
  // a 2-point calibration before trusting any pH value.
  c.phValue1      = 7.00f;
  c.phMv1         = 1667.0f;
  c.phValue2      = 4.00f;
  c.phMv2         = 2030.0f;
  c.phPoint1Set   = false;
  c.phPoint2Set   = false;
  c.tdsK          = 1.0f;
  c.turbOffsetNtu = 0.0f;
  c.tempOffsetC   = 0.0f;
}

}  // namespace

namespace settings {

DeviceConfig &device() { return deviceConfig; }
Calibration  &cal()    { return calibration; }

void load() {
  deviceConfig.deviceId   = "AS-001";
  deviceConfig.ingestUrl  = "";
  deviceConfig.apiKey     = "";
  deviceConfig.intervalS  = DEFAULT_INTERVAL_S;
  setFactoryDefaults(calibration);

  prefs.begin(NS_DEVICE, true);
  deviceConfig.deviceId  = prefs.getString("deviceId", deviceConfig.deviceId);
  deviceConfig.ingestUrl = prefs.getString("ingestUrl", deviceConfig.ingestUrl);
  deviceConfig.apiKey    = prefs.getString("apiKey", deviceConfig.apiKey);
  deviceConfig.intervalS = prefs.getUInt("intervalS", deviceConfig.intervalS);
  prefs.end();

  prefs.begin(NS_CAL, true);
  calibration.phValue1      = prefs.getFloat("phVal1", calibration.phValue1);
  calibration.phMv1        = prefs.getFloat("phMv1", calibration.phMv1);
  calibration.phValue2      = prefs.getFloat("phVal2", calibration.phValue2);
  calibration.phMv2        = prefs.getFloat("phMv2", calibration.phMv2);
  calibration.phPoint1Set   = prefs.getBool("phP1", calibration.phPoint1Set);
  calibration.phPoint2Set   = prefs.getBool("phP2", calibration.phPoint2Set);
  calibration.tdsK         = prefs.getFloat("tdsK", calibration.tdsK);
  calibration.turbOffsetNtu = prefs.getFloat("turbOff", calibration.turbOffsetNtu);
  calibration.tempOffsetC  = prefs.getFloat("tempOff", calibration.tempOffsetC);
  prefs.end();

  deviceConfig.intervalS =
      constrain(deviceConfig.intervalS, MIN_INTERVAL_S, MAX_INTERVAL_S);
}

void saveDevice() {
  prefs.begin(NS_DEVICE, false);
  prefs.putString("deviceId", deviceConfig.deviceId);
  prefs.putString("ingestUrl", deviceConfig.ingestUrl);
  prefs.putString("apiKey", deviceConfig.apiKey);
  prefs.putUInt("intervalS", deviceConfig.intervalS);
  prefs.end();
}

void saveCal() {
  prefs.begin(NS_CAL, false);
  prefs.putFloat("phVal1", calibration.phValue1);
  prefs.putFloat("phMv1", calibration.phMv1);
  prefs.putFloat("phVal2", calibration.phValue2);
  prefs.putFloat("phMv2", calibration.phMv2);
  prefs.putBool("phP1", calibration.phPoint1Set);
  prefs.putBool("phP2", calibration.phPoint2Set);
  prefs.putFloat("tdsK", calibration.tdsK);
  prefs.putFloat("turbOff", calibration.turbOffsetNtu);
  prefs.putFloat("tempOff", calibration.tempOffsetC);
  prefs.end();
}

void resetCal() {
  setFactoryDefaults(calibration);
  saveCal();
}

bool phIsCalibrated() {
  return calibration.phPoint1Set && calibration.phPoint2Set;
}

float phFromMv(float adcMv) {
  const float deltaPh = calibration.phValue1 - calibration.phValue2;
  const float deltaMv = calibration.phMv1 - calibration.phMv2;
  if (fabsf(deltaPh) < 0.1f || fabsf(deltaMv) < 20.0f) return NAN;

  const float slope = deltaMv / deltaPh;  // mV per pH unit (negative)
  const float ph    = calibration.phValue1 + (adcMv - calibration.phMv1) / slope;
  if (ph < 0.0f || ph > 14.0f) return NAN;
  return ph;
}

float turbidityFromSensorVolts(float v) {
  if (v <= 0.02f) return NAN;

  float ntu;
  if (v >= 4.2f) {
    ntu = 0.0f;
  } else if (v < 2.5f) {
    ntu = 3000.0f;
  } else {
    ntu = -1120.4f * v * v + 5742.3f * v - 4352.9f;
  }

  ntu += calibration.turbOffsetNtu;
  return constrain(ntu, 0.0f, 3000.0f);
}

float tdsFromSensorVolts(float v, float tempC, bool applyK) {
  if (v <= 0.02f) return NAN;

  float comp = 1.0f + 0.02f * (tempC - 25.0f);
  if (comp < 0.5f) comp = 0.5f;

  const float vc = v / comp;
  float tds = (133.42f * vc * vc * vc - 255.86f * vc * vc + 857.39f * vc) * 0.5f;
  if (applyK) tds *= calibration.tdsK;
  if (tds < 0.0f) tds = 0.0f;
  return tds;
}

}  // namespace settings
