#include "module.h"

void Buttons::initBtn(Btn &b, uint8_t pin) {
  b.pin = pin;
  pinMode(pin, INPUT_PULLUP);
  delayMicroseconds(100);                      // cho dien tro keo len on dinh
  bool reading = (digitalRead(pin) == LOW);    // doc trang thai that luc khoi dong
  b.lastReading  = reading;                    // nut dang giu san thi khong sinh su kien gia
  b.stable       = reading;
  b.lastChange   = millis();
  b.pressedEvent = false;
}

void Buttons::begin() {
  initBtn(_stop, PIN_BTN_STOP);
  initBtn(_next, PIN_BTN_NEXT);
}

void Buttons::updateBtn(Btn &b, uint32_t now) {
  bool reading = (digitalRead(b.pin) == LOW);  // noi GND: nhan = LOW

  if (reading != b.lastReading) {              // tin hieu vua doi (co the la rung)
    b.lastReading = reading;
    b.lastChange = now;                        // dem lai thoi gian on dinh
  }

  if ((now - b.lastChange) >= BTN_DEBOUNCE_MS && reading != b.stable) {
    b.stable = reading;                        // da on dinh du lau -> chap nhan
    if (b.stable) b.pressedEvent = true;       // chi bao khi nhan xuong, khong bao khi nha
  }
}

bool Buttons::takeEvent(Btn &b) {
  bool e = b.pressedEvent;
  b.pressedEvent = false;                      // doc xong thi xoa
  return e;
}

void Buttons::update() {
  uint32_t now = millis();
  updateBtn(_stop, now);
  updateBtn(_next, now);
}

bool Buttons::stopPressed() { return takeEvent(_stop); }
bool Buttons::nextPressed() { return takeEvent(_next); }
