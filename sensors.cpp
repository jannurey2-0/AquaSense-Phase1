#include "sensors.h"

#include <DallasTemperature.h>
#include <OneWire.h>
#include <math.h>

#include "config.h"
#include "settings.h"

#if USE_ADS1115
#include <Adafruit_ADS1X15.h>
#include <Wire.h>
#endif

namespace {

OneWire           oneWire(PIN_DS18B20);
DallasTemperature ds18b20(&oneWire);

#if USE_ADS1115
Adafruit_ADS1115 ads;
#endif

float median(float *values, int n) {
  for (int i = 1; i < n; i++) {
    const float key = values[i];
    int j = i - 1;
    while (j >= 0 && values[j] > key) {
      values[j + 1] = values[j];
      j--;
    }
    values[j + 1] = key;
  }
  return values[n / 2];
}

// channel: 0 = pH, 1 = turbidity, 2 = TDS
float readChannelMv(uint8_t channel) {
  float samples[ADC_SAMPLES];

#if USE_ADS1115
  for (int i = 0; i < ADC_SAMPLES; i++) {
    samples[i] = (float)ads.readADC_SingleEnded(channel) * 0.1875f;  // GAIN_ONE
    delay(2);
  }
#else
  const int pin = (channel == 0) ? PIN_PH : (channel == 1 ? PIN_TURBIDITY : PIN_TDS);
  for (int i = 0; i < ADC_SAMPLES; i++) {
    samples[i] = analogReadMilliVolts(pin);
    delay(ADC_SAMPLE_GAP_MS);
  }
#endif

  return median(samples, ADC_SAMPLES);
}

}  // namespace

namespace sensors {

void begin() {
#if USE_ADS1115
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  if (!ads.begin()) Serial.println("[sensor] ADS1115 not found");
  ads.setGain(GAIN_ONE);
#else
  analogReadResolution(12);
  analogSetPinAttenuation(PIN_PH, ADC_11db);
  analogSetPinAttenuation(PIN_TURBIDITY, ADC_11db);
  analogSetPinAttenuation(PIN_TDS, ADC_11db);
#endif

  ds18b20.begin();
  ds18b20.setResolution(11);
  if (ds18b20.getDeviceCount() == 0) {
    Serial.println("[sensor] DS18B20 not found - check the 4.7k pull-up");
  }
}

SensorReading readAll() {
  SensorReading r = {};

  const float rawC = ds18b20.getTempCByIndex(0);
  r.tempOk         = rawC > -40.0f && rawC < 80.0f;
  r.tempC          = r.tempOk ? rawC + settings::cal().tempOffsetC : NAN;
  const float compTemp = r.tempOk ? r.tempC : 25.0f;

  r.phMv = readChannelMv(0);
  const float ph = settings::phFromMv(r.phMv);
  r.phOk         = r.phMv > 50.0f && r.phMv < 3100.0f && !isnan(ph);
  r.ph           = r.phOk ? ph : NAN;

  r.turbMv = readChannelMv(1);
  const float turbVolts = (r.turbMv / 1000.0f) * TURBIDITY_DIVIDER_RATIO;
  const float ntu       = settings::turbidityFromSensorVolts(turbVolts);
  r.turbidityOk         = r.turbMv > 30.0f && !isnan(ntu);
  r.turbidityNtu        = r.turbidityOk ? ntu : NAN;

  r.tdsMv = readChannelMv(2);
  const float tdsVolts = (r.tdsMv / 1000.0f) * TDS_DIVIDER_RATIO;
  const float ppm      = settings::tdsFromSensorVolts(tdsVolts, compTemp);
  r.tdsSaturated       = tdsVolts > 2.25f;
  r.tdsOk              = r.tdsMv > 30.0f && !isnan(ppm) && ppm <= 1200.0f && !r.tdsSaturated;
  r.tdsPpm             = r.tdsOk ? ppm : NAN;

  return r;
}

}  // namespace sensors
