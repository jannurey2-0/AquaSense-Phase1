#pragma once

// ---------- Build options ----------
#ifndef USE_ADS1115
#define USE_ADS1115 0
#endif
#ifndef AQUASENSE_TLS_INSECURE
#define AQUASENSE_TLS_INSECURE 1
#endif

#define FW_VERSION "2.0.0"

// ---------- Pins ----------
constexpr int PIN_PH        = 34;  // ADC1_CH6, input-only
constexpr int PIN_TURBIDITY = 35;  // ADC1_CH7, input-only
constexpr int PIN_TDS       = 32;  // ADC1_CH4
constexpr int PIN_DS18B20   = 4;
constexpr int PIN_LED       = 2;
constexpr int PIN_BUTTON    = 0;   // on-board BOOT
constexpr int PIN_I2C_SDA   = 21;  // ADS1115 only
constexpr int PIN_I2C_SCL   = 22;  // ADS1115 only

// ---------- Analog front end ----------
constexpr float PH_DIVIDER_RATIO        = 1.5f;  // 10k / 20k
constexpr float TURBIDITY_DIVIDER_RATIO = 1.5f;  // 10k / 20k
constexpr float TDS_DIVIDER_RATIO       = 1.0f;  // direct

// ---------- Sampling ----------
constexpr int      ADC_SAMPLES        = 15;
constexpr uint32_t ADC_SAMPLE_GAP_MS  = 20;
constexpr uint32_t DEFAULT_INTERVAL_S = 900;
constexpr uint32_t MIN_INTERVAL_S     = 60;
constexpr uint32_t MAX_INTERVAL_S     = 86400;

// ---------- Networking ----------
constexpr uint32_t PORTAL_TIMEOUT_S = 600;
constexpr uint32_t HTTP_TIMEOUT_MS  = 12000;
constexpr uint32_t NTP_RETRY_MS     = 60000;
constexpr uint32_t FLUSH_GAP_MS     = 1000;
constexpr uint32_t FLUSH_RETRY_MS   = 30000;
constexpr uint32_t FIRST_SAMPLE_MS  = 5000;

// ---------- Offline buffer ----------
constexpr size_t BUFFER_MAX_RECORDS = 500;

// ---------- Provisioning access point ----------
constexpr char AP_SSID[]     = "AquaSense-Setup";
constexpr char AP_PASSWORD[] = "aquasense";
