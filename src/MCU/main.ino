#include <Arduino.h>
#include "config.h"
#include "module.h"

Buttons      buttons;
Pot          pot;
Lcd          lcd;
StepperMotor motor;
UartLink     uartLink;

static uint16_t phanTramSangToc(float pct) {
  return SPEED_MIN
       + (uint16_t)(pct * (SPEED_MAX - SPEED_MIN) / 100.0f + 0.5f);
}

static void capNhatLcd() {
  static uint8_t dong = 0;
  char buf[24];

  if (dong == 0) {
    snprintf(
      buf,
      sizeof(buf),
      "Set:%4usps %s",
      motor.target(),
      motor.isRunning() ? "RUN " : "STOP"
    );
  }
  else {
    snprintf(
      buf,
      sizeof(buf),
      "Now:%4u rpm:%3.0f",
      motor.speed(),
      motor.rpm()
    );
  }

  lcd.print(dong, buf);
  dong = !dong;
}

void setup() {
  Serial.begin(115200);

  // Trang thai an toan luc khoi dong: motor dung.
  motor.begin();

  pot.begin();
  buttons.begin();
  lcd.begin();

  // GPIO16 RX / GPIO17 TX -> Jetson /dev/ttyTHS1
  uartLink.begin();

  Serial.println("ESP32 system ready");
}

void loop() {
  uint32_t now = millis();

  pot.update();
  buttons.update();
  uartLink.update();

  motor.setTarget(
    phanTramSangToc(pot.percent())
  );

  // Lay event RA MOT LAN.
  bool stopByButton = buttons.stopPressed();
  bool runByButton  = buttons.nextPressed();

  bool stopByApp = uartLink.stopCmd();
  bool runByApp  = uartLink.runCmd();

  // STOP uu tien cao hon RUN neu hai lenh den cung luc.
  if (stopByButton || stopByApp) {
    motor.stop();

    uartLink.sendStatus(
      false,
      motor.speed(),
      stopByButton ? "BUTTON" : "APP"
    );

    Serial.println(
      stopByButton
        ? "MOTOR STOP - BUTTON"
        : "MOTOR STOP - APP"
    );
  }
  else if (runByButton || runByApp) {
    motor.run();

    uartLink.sendStatus(
      true,
      motor.speed(),
      runByButton ? "BUTTON" : "APP"
    );

    Serial.println(
      runByButton
        ? "MOTOR RUN - BUTTON"
        : "MOTOR RUN - APP"
    );
  }

  motor.update();

  // LCD
  static uint32_t lastLcd = 0;

  if (now - lastLcd >= 125) {
    lastLcd = now;
    capNhatLcd();
  }

  // USB debug
  static uint32_t lastPrint = 0;

  if (now - lastPrint >= 500) {
    lastPrint = now;

    Serial.printf(
      "run=%d target=%u current=%u %.1frpm pot=%.1f%%\n",
      motor.isRunning(),
      motor.target(),
      motor.speed(),
      motor.rpm(),
      pot.percent()
    );
  }
}
