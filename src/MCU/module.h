#pragma once
#include <Arduino.h>
#include "config.h"

// =====================================================================
// Buttons
// =====================================================================
class Buttons {
public:
  void begin();
  void update();
  bool stopPressed();
  bool nextPressed();

private:
  struct Btn {
    uint8_t  pin;
    bool     lastReading;
    bool     stable;
    uint32_t lastChange;
    bool     pressedEvent;
  };

  Btn _stop;
  Btn _next;

  void initBtn(Btn &b, uint8_t pin);
  void updateBtn(Btn &b, uint32_t now);
  bool takeEvent(Btn &b);
};

// =====================================================================
// Potentiometer
// =====================================================================
class Pot {
public:
  void  begin();
  void  update();
  float percent() const { return _reported; }

private:
  float    _filtered   = 0.0f;
  float    _reported   = 0.0f;
  uint32_t _lastSample = 0;

  float toPercent(float raw) const;
};

// =====================================================================
// LCD
// =====================================================================
class Lcd {
public:
  bool begin();
  void print(uint8_t row, const char *s);
  void clear();
  void backlight(bool on);
  bool isReady() const { return _ready; }

private:
  uint8_t  _addr      = LCD_I2C_ADDR;
  bool     _ready     = false;
  bool     _fail      = false;
  uint8_t  _backlight = 0x08;
  uint32_t _lastRetry   = 0;
  uint32_t _lastRefresh = 0;
  char     _shadow[LCD_ROWS][LCD_COLS + 1];

  bool initLcd(bool verbose);
  bool probe(uint8_t a);
  bool findLcd();
  void write4(uint8_t nibble, bool rs);
  void sendByte(uint8_t value, bool rs);
  void command(uint8_t c);
  void clearShadow();
  void invalidateShadow();
};

// =====================================================================
// Stepper motor
// =====================================================================
class StepperMotor {
public:
  void begin();
  void update();

  void run();
  void stop();
  void toggleDir();
  void setTarget(uint16_t sps);

  bool     isRunning() const  { return _running; }
  bool     dirForward() const { return _forward; }
  uint16_t target() const     { return _target; }
  uint16_t speed() const      { return _running ? (uint16_t)_cur : 0; }
  float    rpm() const;

private:
  bool     _running = false;
  bool     _forward = DIR_MAC_DINH;
  uint16_t _target  = SPEED_MIN;
  float    _cur     = SPEED_START;
  uint32_t _periodUs   = 0;
  uint32_t _nextUs     = 0;
  uint32_t _lastRampMs = 0;

  void applyDir();
  void applyEnable();
  void updatePeriod();
};

// =====================================================================
// UART2 ESP32 <-> Jetson
// =====================================================================
class UartLink {
public:
  void begin();
  void update();

  bool runCmd();
  bool stopCmd();

  // Gui thay doi trang thai motor len App.
  // source = "APP" hoac "BUTTON"
  void sendStatus(bool running, uint16_t sps, const char *source);

private:
  bool _runCmd  = false;
  bool _stopCmd = false;

  char   _rx[32];
  size_t _rxLen = 0;

  void processLine();
};
