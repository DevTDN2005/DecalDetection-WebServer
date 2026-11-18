#include <ctype.h>
#include <string.h>
#include "module.h"

// =====================================================================
// UART2 ESP32 <-> Jetson
//
// Jetson -> ESP32:
//   PING\n
//   RUN\n
//   STOP\n
//
// ESP32 -> Jetson:
//   PONG\n
//   STATE:RUN SOURCE:APP SPEED:100\n
//   STATE:STOP SOURCE:BUTTON SPEED:0\n
// =====================================================================

void UartLink::begin() {
  _runCmd = false;
  _stopCmd = false;
  _rxLen = 0;

  Serial2.begin(
    JETSON_BAUD,
    SERIAL_8N1,
    PIN_JETSON_RX,
    PIN_JETSON_TX
  );
}

void UartLink::processLine() {
  _rx[_rxLen] = '\0';

  for (size_t i = 0; i < _rxLen; i++) {
    _rx[i] = (char)toupper((unsigned char)_rx[i]);
  }

  if (strcmp(_rx, "PING") == 0) {
    Serial2.print("PONG\n");
  }
  else if (strcmp(_rx, "RUN") == 0 || strcmp(_rx, "R") == 0) {
    _runCmd = true;
  }
  else if (strcmp(_rx, "STOP") == 0 || strcmp(_rx, "S") == 0) {
    _stopCmd = true;
  }

  _rxLen = 0;
}

void UartLink::update() {
  while (Serial2.available()) {
    char c = (char)Serial2.read();

    if (c == '\r')
      continue;

    if (c == '\n') {
      processLine();
      continue;
    }

    if (_rxLen < sizeof(_rx) - 1) {
      _rx[_rxLen++] = c;
    }
    else {
      _rxLen = 0;
    }
  }
}

bool UartLink::runCmd() {
  bool value = _runCmd;
  _runCmd = false;
  return value;
}

bool UartLink::stopCmd() {
  bool value = _stopCmd;
  _stopCmd = false;
  return value;
}

void UartLink::sendStatus(
  bool running,
  uint16_t sps,
  const char *source
) {
  Serial2.printf(
    "STATE:%s SOURCE:%s SPEED:%u\n",
    running ? "RUN" : "STOP",
    source,
    sps
  );
}
