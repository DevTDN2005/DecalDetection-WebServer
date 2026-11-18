#include "module.h"

// Doi gia tri ADC sang phan tram 0..100 (cat hai dau vi ADC ESP32 khong chinh xac sat bien)
float Pot::toPercent(float raw) const {
  float pct = (raw - POT_RAW_MIN) * 100.0f / (POT_RAW_MAX - POT_RAW_MIN);
  if (pct < 0.0f)   pct = 0.0f;
  if (pct > 100.0f) pct = 100.0f;
  return pct;
}

void Pot::begin() {
  analogSetPinAttenuation(PIN_POT, ADC_11db);   // do duoc dai 0..3.3V
  _filtered   = analogRead(PIN_POT);            // khoi tao bang gia tri that, tranh nhay tu 0
  _reported   = toPercent(_filtered);
  _lastSample = millis();
}

void Pot::update() {
  uint32_t now = millis();
  if (now - _lastSample < POT_SAMPLE_MS) return;
  _lastSample = now;

  int raw = analogRead(PIN_POT);                // 0..4095 (12 bit)
  _filtered += POT_ALPHA * (raw - _filtered);   // loc: trung binh truot ham mu

  float pct = toPercent(_filtered);

  // Chong nhap nhay: chi cap nhat khi thay doi du lon (hai dau thi cap nhat luon)
  if (fabsf(pct - _reported) >= POT_HYSTERESIS || pct == 0.0f || pct == 100.0f) {
    _reported = pct;
  }
}
