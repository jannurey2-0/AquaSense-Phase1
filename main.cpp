#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WiFiManager.h>
#include <esp_task_wdt.h>
#include <math.h>
#include <time.h>

#include "buffer.h"
#include "config.h"
#include "sensors.h"
#include "settings.h"

namespace {

WiFiManager wm;

WiFiManagerParameter *paramDeviceId  = nullptr;
WiFiManagerParameter *paramIngestUrl = nullptr;
WiFiManagerParameter *paramApiKey    = nullptr;
WiFiManagerParameter *paramInterval  = nullptr;

uint32_t lastSampleMs  = 0;
uint32_t lastNtpTryMs  = 0;
uint32_t nextFlushMs   = 0;
bool     sampledOnce   = false;
bool     ntpSynced     = false;

// --------------------------------------------------------------------------
// helpers
// --------------------------------------------------------------------------

float round2(float v) { return roundf(v * 100.0f) / 100.0f; }

String fmt(float v, bool ok, unsigned int decimals) {
  if (!ok || isnan(v)) return "--";
  return String(v, decimals);
}

String buildPayload(const SensorReading &r, bool buffered) {
  JsonDocument doc;
  doc["deviceId"] = settings::device().deviceId;

  const time_t now    = time(nullptr);
  const bool   timeOk = ntpSynced && now > 1700000000;
  doc["ts"]           = timeOk ? (uint32_t)now : 0u;
  doc["ntp"]          = timeOk;

  if (r.phOk) doc["ph"] = round2(r.ph); else doc["ph"] = nullptr;
  if (r.tempOk) doc["temp"] = round2(r.tempC); else doc["temp"] = nullptr;
  if (r.tdsOk) doc["tds"] = round2(r.tdsPpm); else doc["tds"] = nullptr;
  if (r.turbidityOk) doc["turbidity"] = round2(r.turbidityNtu); else doc["turbidity"] = nullptr;

  doc["valid"]["ph"]        = r.phOk;
  doc["valid"]["temp"]      = r.tempOk;
  doc["valid"]["tds"]       = r.tdsOk;
  doc["valid"]["turbidity"] = r.turbidityOk;
  doc["tdsSaturated"]       = r.tdsSaturated;

  doc["buffered"] = buffered;
  doc["rssi"]     = (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : 0;
  doc["fw"]       = FW_VERSION;

  String out;
  serializeJson(doc, out);
  return out;
}

bool postWith(WiFiClient &client, const String &body) {
  const DeviceConfig &cfg = settings::device();

  HTTPClient http;
  http.setConnectTimeout(HTTP_TIMEOUT_MS);
  http.setTimeout(HTTP_TIMEOUT_MS);
  if (!http.begin(client, cfg.ingestUrl)) {
    Serial.println("[net] invalid endpoint URL");
    return false;
  }
  http.addHeader("Content-Type", "application/json");
  if (cfg.apiKey.length() > 0) http.addHeader("x-api-key", cfg.apiKey);

  const int code = http.POST(body);
  const bool ok  = code >= 200 && code < 300;
  if (!ok) {
    Serial.printf("[net] POST failed (%d)\n", code);
    const String resp = http.getString();
    if (resp.length() > 0) Serial.println(resp.substring(0, 200));
  }
  http.end();
  return ok;
}

bool uploadJson(const String &body) {
  const DeviceConfig &cfg = settings::device();
  if (WiFi.status() != WL_CONNECTED) return false;
  if (!cfg.ingestUrl.startsWith("http")) {
    Serial.println("[net] ingest URL not configured");
    return false;
  }

  if (cfg.ingestUrl.startsWith("https://")) {
    WiFiClientSecure client;
#if AQUASENSE_TLS_INSECURE
    client.setInsecure();
#endif
    return postWith(client, body);
  }

  WiFiClient client;
  return postWith(client, body);
}

void trySyncTime() {
  if (ntpSynced || WiFi.status() != WL_CONNECTED) return;

  const uint32_t now = millis();
  if (lastNtpTryMs != 0 && now - lastNtpTryMs < NTP_RETRY_MS) return;
  lastNtpTryMs = now == 0 ? 1 : now;

  configTzTime("PST-8", "pool.ntp.org", "time.google.com");
  for (int i = 0; i < 20 && !ntpSynced; i++) {
    if (time(nullptr) > 1700000000) ntpSynced = true;
    else delay(500);
  }
  Serial.println(ntpSynced ? "[ntp] time synced" : "[ntp] sync pending - will retry");
}

void takeSample() {
  const SensorReading r = sensors::readAll();

  Serial.printf("[sample] pH %s  temp %s C  TDS %s ppm  turbidity %s NTU\n",
                fmt(r.ph, r.phOk, 2).c_str(),
                fmt(r.tempC, r.tempOk, 2).c_str(),
                fmt(r.tdsPpm, r.tdsOk, 1).c_str(),
                fmt(r.turbidityNtu, r.turbidityOk, 1).c_str());

  const String payload = buildPayload(r, false);
  bool queued          = false;

  // Preserve ordering: if a backlog exists, queue instead of jumping ahead.
  if (buffer::count() == 0) {
    if (uploadJson(payload)) {
      Serial.println("[net] reading uploaded");
    } else {
      queued = buffer::push(payload);
    }
  } else {
    queued = buffer::push(payload);
  }

  if (queued) {
    Serial.printf("[buf] queued (%u pending)\n", (unsigned)buffer::count());
  } else if (buffer::count() > 0) {
    Serial.println("[buf] WARNING: could not queue reading");
  }

  lastSampleMs = millis();
  sampledOnce  = true;
}

void flushOne() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (buffer::count() == 0) return;

  String record;
  if (!buffer::peekOldest(record)) return;

  JsonDocument doc;
  if (deserializeJson(doc, record) != DeserializationError::Ok) {
    Serial.println("[buf] dropping corrupt record");
    buffer::removeOldest();
    return;
  }

  doc["buffered"] = true;
  String body;
  serializeJson(doc, body);

  if (uploadJson(body)) {
    buffer::removeOldest();
    nextFlushMs = millis() + FLUSH_GAP_MS;
    Serial.printf("[buf] flushed one (%u left)\n", (unsigned)buffer::count());
  } else {
    nextFlushMs = millis() + FLUSH_RETRY_MS;
  }
}

// --------------------------------------------------------------------------
// status / UI
// --------------------------------------------------------------------------

void updateLed() {
  const bool     portal = wm.getConfigPortalActive();
  const bool     online = WiFi.status() == WL_CONNECTED;
  const uint32_t t      = millis();

  bool on;
  if (portal)        on = (t / 120) % 2;
  else if (!online)  on = (t / 600) % 2;
  else               on = (t % 3000) < 40;

  digitalWrite(PIN_LED, on ? HIGH : LOW);
}

void handleButton() {
  static bool     wasPressed = false;
  static uint32_t pressedAt  = 0;

  const bool pressed = digitalRead(PIN_BUTTON) == LOW;

  if (pressed && !wasPressed) {
    wasPressed = true;
    pressedAt  = millis();
    return;
  }
  if (!pressed) {
    wasPressed = false;
    return;
  }

  if (millis() > 8000 && millis() - pressedAt > 5000) {
    Serial.println("[cfg] BOOT held 5 s - erasing WiFi settings and restarting");
    wm.resetSettings();
    delay(300);
    ESP.restart();
  }
}

void printStatus() {
  const DeviceConfig &cfg = settings::device();
  const Calibration  &c   = settings::cal();

  Serial.printf("Device ID : %s\n", cfg.deviceId.c_str());
  Serial.printf("Endpoint  : %s\n", cfg.ingestUrl.length() ? cfg.ingestUrl.c_str() : "(not set)");
  Serial.printf("API key   : %s\n", cfg.apiKey.length() ? "(set)" : "(not set)");
  Serial.printf("Interval  : %lu s\n", (unsigned long)cfg.intervalS);
  Serial.printf("WiFi      : %s  RSSI %d dBm  IP %s\n",
                WiFi.status() == WL_CONNECTED ? "connected" : "offline",
                WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0,
                WiFi.localIP().toString().c_str());
  Serial.printf("Portal    : %s\n", wm.getConfigPortalActive() ? "active" : "inactive");
  Serial.printf("NTP       : %s\n", ntpSynced ? "synced" : "not synced");
  Serial.printf("Buffered  : %u / %u\n", (unsigned)buffer::count(),
                (unsigned)BUFFER_MAX_RECORDS);
  Serial.printf("Free heap : %u bytes\n", (unsigned)ESP.getFreeHeap());

  const SensorReading r = sensors::readAll();
  Serial.printf("pH        : %s  (%s, %.0f mV)  cal: %s\n",
                fmt(r.ph, r.phOk, 2).c_str(), r.phOk ? "ok" : "invalid", r.phMv,
                settings::phIsCalibrated() ? "2-point" : "defaults");
  Serial.printf("Temp      : %s C  (%s)\n", fmt(r.tempC, r.tempOk, 2).c_str(),
                r.tempOk ? "ok" : "invalid");
  Serial.printf("TDS       : %s ppm  (%s, %.0f mV%s)\n",
                fmt(r.tdsPpm, r.tdsOk, 1).c_str(), r.tdsOk ? "ok" : "invalid", r.tdsMv,
                r.tdsSaturated ? ", saturated" : "");
  Serial.printf("Turbidity : %s NTU  (%s, %.0f mV)\n",
                fmt(r.turbidityNtu, r.turbidityOk, 1).c_str(),
                r.turbidityOk ? "ok" : "invalid", r.turbMv);
  Serial.printf("Cal       : tdsK %.3f  turbOffset %.1f NTU  tempOffset %.2f C\n",
                c.tdsK, c.turbOffsetNtu, c.tempOffsetC);
}

// --------------------------------------------------------------------------
// serial calibration CLI
// --------------------------------------------------------------------------

void printHelp() {
  Serial.println();
  Serial.println("Commands:");
  Serial.println("  status                  print configuration and live readings");
  Serial.println("  cal ph <bufferValue>    capture current reading as pH 4.00 or 7.00 buffer");
  Serial.println("  cal tds <knownPpm>      calibrate TDS against a known solution");
  Serial.println("  cal turb <knownNtu>     calibrate turbidity against a known sample");
  Serial.println("  cal temp <offsetC>      apply a temperature offset (e.g. -0.3)");
  Serial.println("  cal show / cal reset    show or reset calibration");
  Serial.println("  cfg show / cfg portal   show config or reopen the setup portal");
  Serial.println("  sample                  take and upload a reading now");
  Serial.println("  buffer / buffer clear   show or erase the offline buffer");
  Serial.println("  reboot");
  Serial.println();
}

void handleCal(const String &what, const String &arg) {
  Calibration &c = settings::cal();

  if (what == "show") {
    printStatus();
    return;
  }
  if (what == "reset") {
    settings::resetCal();
    Serial.println("[cal] calibration reset to defaults");
    return;
  }
  if (arg.length() == 0) {
    Serial.println("[cal] missing value - type 'help'");
    return;
  }

  if (what == "ph") {
    const float bufferPh = arg.toFloat();
    if (bufferPh < 0.0f || bufferPh > 14.0f) {
      Serial.println("[cal] pH buffer value must be 0-14");
      return;
    }
    const SensorReading r = sensors::readAll();
    if (r.phMv <= 50.0f || r.phMv >= 3100.0f) {
      Serial.println("[cal] pH voltage out of range - check wiring");
      return;
    }
    if (bufferPh >= 6.0f) {
      c.phValue1    = bufferPh;
      c.phMv1       = r.phMv;
      c.phPoint1Set = true;
    } else {
      c.phValue2    = bufferPh;
      c.phMv2       = r.phMv;
      c.phPoint2Set = true;
    }
    settings::saveCal();
    Serial.printf("[cal] pH point %.2f saved at %.0f mV\n", bufferPh, r.phMv);
    Serial.printf("[cal] 2-point calibration %s\n",
                  settings::phIsCalibrated() ? "complete" : "incomplete - do the other buffer");
    return;
  }

  if (what == "tds") {
    const float known = arg.toFloat();
    if (known <= 10.0f) {
      Serial.println("[cal] known TDS must be > 10 ppm");
      return;
    }
    const SensorReading r = sensors::readAll();
    const float volts    = (r.tdsMv / 1000.0f) * TDS_DIVIDER_RATIO;
    const float rawPpm   = settings::tdsFromSensorVolts(volts, r.tempOk ? r.tempC : 25.0f, false);
    if (isnan(rawPpm) || rawPpm < 5.0f) {
      Serial.println("[cal] TDS signal too low - check the probe");
      return;
    }
    c.tdsK = constrain(known / rawPpm, 0.1f, 10.0f);
    settings::saveCal();
    Serial.printf("[cal] tdsK = %.3f\n", c.tdsK);
    return;
  }

  if (what == "turb") {
    const float known = arg.toFloat();
    if (known < 0.0f) {
      Serial.println("[cal] NTU cannot be negative");
      return;
    }
    const SensorReading r = sensors::readAll();
    const float volts    = (r.turbMv / 1000.0f) * TURBIDITY_DIVIDER_RATIO;
    const float current  = settings::turbidityFromSensorVolts(volts);
    if (isnan(current)) {
      Serial.println("[cal] turbidity signal too low - check wiring");
      return;
    }
    const float raw      = current - c.turbOffsetNtu;
    c.turbOffsetNtu      = constrain(known - raw, -500.0f, 500.0f);
    settings::saveCal();
    Serial.printf("[cal] turbidity offset = %.1f NTU\n", c.turbOffsetNtu);
    return;
  }

  if (what == "temp") {
    c.tempOffsetC = constrain(arg.toFloat(), -10.0f, 10.0f);
    settings::saveCal();
    Serial.printf("[cal] temperature offset = %.2f C\n", c.tempOffsetC);
    return;
  }

  Serial.println("[cal] unknown - use ph | tds | turb | temp | show | reset");
}

void handleCfg(const String &what) {
  if (what == "show") {
    printStatus();
  } else if (what == "portal") {
    Serial.println("[cfg] opening setup portal");
    wm.startConfigPortal(AP_SSID, AP_PASSWORD);
  } else {
    Serial.println("[cfg] use 'cfg show' or 'cfg portal'");
  }
}

void runCommand(String line) {
  line.trim();
  if (line.length() == 0) return;

  String       tok[4];
  int          n     = 0;
  int          start = 0;
  const int    len   = (int)line.length();

  for (int i = 0; i <= len && n < 4; i++) {
    if (i == len || line[i] == ' ') {
      if (i > start) tok[n++] = line.substring(start, i);
      start = i + 1;
    }
  }
  if (n == 0) return;

  String cmd = tok[0];
  cmd.toLowerCase();

  if (cmd == "help") {
    printHelp();
  } else if (cmd == "status") {
    printStatus();
  } else if (cmd == "cal" && n >= 2) {
    String sub = tok[1];
    sub.toLowerCase();
    handleCal(sub, n >= 3 ? tok[2] : "");
  } else if (cmd == "cfg" && n >= 2) {
    String sub = tok[1];
    sub.toLowerCase();
    handleCfg(sub);
  } else if (cmd == "sample") {
    takeSample();
  } else if (cmd == "buffer") {
    if (n >= 2) {
      String sub = tok[1];
      sub.toLowerCase();
      if (sub == "clear") {
        buffer::clear();
        Serial.println("[buf] cleared");
      } else {
        Serial.println("[buf] use 'buffer' or 'buffer clear'");
      }
    } else {
      Serial.printf("[buf] %u / %u records\n", (unsigned)buffer::count(),
                    (unsigned)BUFFER_MAX_RECORDS);
    }
  } else if (cmd == "reboot") {
    ESP.restart();
  } else {
    Serial.println("unknown command - type 'help'");
  }
}

void handleSerial() {
  static String line;
  while (Serial.available()) {
    const char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (line.length() > 0) {
        runCommand(line);
        line = "";
      }
    } else if (line.length() < 120) {
      line += c;
    }
  }
}

// --------------------------------------------------------------------------
// provisioning callbacks
// --------------------------------------------------------------------------

void apCallback(WiFiManager *) {
  Serial.printf("[wifi] setup AP '%s' password '%s' at %s\n", AP_SSID, AP_PASSWORD,
                WiFi.softAPIP().toString().c_str());
  Serial.println("[wifi] join it and the captive portal opens automatically");
}

void saveParamsCallback() {
  DeviceConfig &cfg = settings::device();

  cfg.deviceId = String(paramDeviceId->getValue());
  cfg.deviceId.trim();
  if (cfg.deviceId.length() == 0) cfg.deviceId = "AS-001";

  cfg.ingestUrl = String(paramIngestUrl->getValue());
  cfg.ingestUrl.trim();

  cfg.apiKey = String(paramApiKey->getValue());
  cfg.apiKey.trim();

  const uint32_t interval = String(paramInterval->getValue()).toInt();
  cfg.intervalS = constrain(interval, MIN_INTERVAL_S, MAX_INTERVAL_S);

  settings::saveDevice();
  Serial.printf("[cfg] saved: id=%s interval=%lu s\n", cfg.deviceId.c_str(),
                (unsigned long)cfg.intervalS);
}

void startWatchdog() {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  esp_task_wdt_config_t config = {};
  config.timeout_ms     = 60000;
  config.idle_core_mask = 0;
  config.trigger_panic  = true;
  esp_task_wdt_reconfigure(&config);
#else
  esp_task_wdt_deinit();
  esp_task_wdt_init(60, false);
  esp_task_wdt_add(NULL);
#endif
}

}  // namespace

// --------------------------------------------------------------------------
// setup / loop
// --------------------------------------------------------------------------

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.printf("\nAquaSense firmware %s\n", FW_VERSION);

  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, LOW);
  pinMode(PIN_BUTTON, INPUT_PULLUP);

  settings::load();
  sensors::begin();
  buffer::begin();

  WiFi.mode(WIFI_STA);

  wm.setDebugOutput(false);
  wm.setTitle("AquaSense Setup");
  wm.setConfigPortalBlocking(false);
  wm.setConfigPortalTimeout(PORTAL_TIMEOUT_S);
  wm.setAPCallback(apCallback);
  wm.setSaveParamsCallback(saveParamsCallback);

  paramDeviceId = new WiFiManagerParameter(
      "device_id", "Device ID", settings::device().deviceId.c_str(), 32);
  paramIngestUrl = new WiFiManagerParameter(
      "ingest_url", "Ingest endpoint URL", settings::device().ingestUrl.c_str(), 128);
  paramApiKey = new WiFiManagerParameter(
      "api_key", "Ingest API key", settings::device().apiKey.c_str(), 96);
  const String intervalDefault = String(settings::device().intervalS);
  paramInterval = new WiFiManagerParameter(
      "interval_s", "Sampling interval (s, 60-86400)", intervalDefault.c_str(), 8);

  wm.addParameter(paramDeviceId);
  wm.addParameter(paramIngestUrl);
  wm.addParameter(paramApiKey);
  wm.addParameter(paramInterval);

  Serial.println("[wifi] starting - portal opens if no saved network");
  wm.autoConnect(AP_SSID, AP_PASSWORD);

  startWatchdog();
  printHelp();
}

void loop() {
  wm.process();
  updateLed();
  handleButton();
  handleSerial();

  const bool online = WiFi.status() == WL_CONNECTED;
  if (online && !ntpSynced) trySyncTime();

  const uint32_t intervalMs = settings::device().intervalS * 1000UL;
  if (!sampledOnce) {
    if (millis() >= FIRST_SAMPLE_MS) takeSample();
  } else if (millis() - lastSampleMs >= intervalMs) {
    takeSample();
  }

  if (online && buffer::count() > 0 && millis() >= nextFlushMs) {
    flushOne();
  }

  esp_task_wdt_reset();
  delay(5);
}
