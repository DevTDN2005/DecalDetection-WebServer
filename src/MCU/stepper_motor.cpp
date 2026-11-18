#include "module.h"

void StepperMotor::begin() {
  _running = false;
  _forward = DIR_MAC_DINH;
  _target  = SPEED_MIN;
  _cur     = SPEED_START;

  // ghi muc EN truoc khi dat OUTPUT de driver khong bat thoang qua luc khoi dong
  digitalWrite(PIN_EN, (STOP_NHA_MOTOR ? HIGH : LOW));
  pinMode(PIN_EN, OUTPUT);
  applyEnable();

  pinMode(PIN_STEP, OUTPUT);
  pinMode(PIN_DIR, OUTPUT);
  digitalWrite(PIN_STEP, LOW);
  applyDir();
  updatePeriod();
}

void StepperMotor::update() {
  if (!_running) return;

  // --- Ramp: nhich toc do hien tai ve toc do dat ---
  uint32_t nowMs = millis();
  uint32_t dtMs  = nowMs - _lastRampMs;
  if (dtMs >= RAMP_INTERVAL_MS) {
    _lastRampMs = nowMs;
    float d = RAMP_SPS_PER_S * dtMs / 1000.0f;   // luong thay doi cho lan nay
    if (_cur < _target) {
      _cur += d;
      if (_cur > _target) _cur = _target;
    } else if (_cur > _target) {
      _cur -= d;
      if (_cur < _target) _cur = _target;
    }
    updatePeriod();
  }

  // --- Phat xung STEP ---
  uint32_t now = micros();
  // phat bu toi da MAX_CATCHUP xung neu loop bi tre (vd luc ghi LCD)
  for (uint8_t i = 0; i < MAX_CATCHUP; i++) {
    // so sanh co dau de khong loi khi micros() tran so
    if ((int32_t)(now - _nextUs) < 0) break;
    digitalWrite(PIN_STEP, HIGH);
    delayMicroseconds(3);              // A4988 can xung HIGH toi thieu 1 us
    digitalWrite(PIN_STEP, LOW);
    _nextUs += _periodUs;
  }
  // van tre qua nhieu thi dong bo lai, tranh phat xung don dap
  if ((int32_t)(now - _nextUs) > (int32_t)(_periodUs * MAX_CATCHUP)) {
    _nextUs = now;
  }
}

void StepperMotor::run() {
  if (_running) return;
  _running = true;
  _cur = SPEED_START;                  // bat dau cham roi tang dan
  updatePeriod();
  applyEnable();
  _lastRampMs = millis();
  _nextUs = micros() + 1000;           // doi 1 ms cho driver on dinh roi moi phat xung
}

void StepperMotor::stop() {
  _running = false;                    // dung dot ngot
  digitalWrite(PIN_STEP, LOW);
  applyEnable();
}

void StepperMotor::toggleDir() {
  _forward = !_forward;
  applyDir();
  _cur = SPEED_START;                  // dao chieu thi chay lai tu toc do thap
  updatePeriod();
  _nextUs = micros() + DIR_PAUSE_US;   // nghi mot luc truoc khi phat xung tiep
}

void StepperMotor::setTarget(uint16_t sps) {
  if (sps < SPEED_MIN) sps = SPEED_MIN;
  if (sps > SPEED_MAX) sps = SPEED_MAX;
  _target = sps;
}

float StepperMotor::rpm() const {
  // vong/phut = (buoc/giay * 60) / (so buoc moi vong * he so vi buoc)
  return (speed() * 60.0f) / (STEPS_PER_REV * MICROSTEP);
}

void StepperMotor::applyDir() {
  digitalWrite(PIN_DIR, _forward ? HIGH : LOW);
}

void StepperMotor::applyEnable() {
  // ENABLE tich cuc muc thap: LOW = bat driver, HIGH = tat driver
  bool bat = _running || !STOP_NHA_MOTOR;
  digitalWrite(PIN_EN, bat ? LOW : HIGH);
}

void StepperMotor::updatePeriod() {
  uint32_t s = (uint32_t)_cur;
  if (s < SPEED_MIN) s = SPEED_MIN;    // tranh chia cho 0
  _periodUs = 1000000UL / s;
}
