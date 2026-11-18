#include <Wire.h>
#include <string.h>
#include "module.h"

// Cach PCF8574 noi voi LCD:
//   P0=RS  P1=RW  P2=E  P3=den nen  P4..P7 = D4..D7
static const uint8_t BIT_RS = 0x01;   // 0 = lenh, 1 = du lieu (ky tu)
static const uint8_t BIT_E  = 0x04;   // xung E: LCD chot du lieu khi E xuong thap
static const uint8_t BIT_BL = 0x08;   // den nen
// RW (P1) luon bang 0 vi chi ghi vao LCD

// ---- Tang thap: gui 4 bit qua I2C ----
void Lcd::write4(uint8_t nibble, bool rs) {
  uint8_t d = (uint8_t)(nibble << 4) | _backlight | (rs ? BIT_RS : 0);
  Wire.beginTransmission(_addr);
  Wire.write(d | BIT_E);   // E len cao
  Wire.write(d);           // E xuong thap -> LCD nhan 4 bit
  if (Wire.endTransmission() != 0) _fail = true;   // ghi nhan loi I2C (day long, mat nguon)
}

// Gui 1 byte = 2 lan 4 bit (nua cao truoc, nua thap sau)
void Lcd::sendByte(uint8_t value, bool rs) {
  write4(value >> 4, rs);
  write4(value & 0x0F, rs);
}

void Lcd::command(uint8_t c) { sendByte(c, false); }

// ---- Do dia chi I2C ----
bool Lcd::probe(uint8_t a) {
  Wire.beginTransmission(a);
  return Wire.endTransmission() == 0;   // 0 = co thiet bi tra loi
}

bool Lcd::findLcd() {
  if (probe(LCD_I2C_ADDR)) { _addr = LCD_I2C_ADDR; return true; }   // thu dia chi trong config truoc
  for (uint8_t a = 0x20; a <= 0x27; a++) if (probe(a)) { _addr = a; return true; }   // PCF8574
  for (uint8_t a = 0x38; a <= 0x3F; a++) if (probe(a)) { _addr = a; return true; }   // PCF8574A
  return false;
}

void Lcd::clearShadow() {
  for (uint8_t r = 0; r < LCD_ROWS; r++) {
    memset(_shadow[r], ' ', LCD_COLS);
    _shadow[r][LCD_COLS] = '\0';
  }
}

// Xoa dau dong dau tien cua shadow -> lan print sau chac chan khac -> ve lai
void Lcd::invalidateShadow() {
  for (uint8_t r = 0; r < LCD_ROWS; r++) _shadow[r][0] = '\0';
}

// ---- Khoi tao LCD (dung ca cho lan ket noi lai) ----
bool Lcd::initLcd(bool verbose) {
  _ready = false;
  _fail  = false;
  if (!findLcd()) {
    if (verbose) Serial.println("LCD: khong tim thay tren I2C (kiem tra day SDA/SCL)");
    return false;
  }
  if (verbose) Serial.printf("LCD: tim thay o dia chi I2C 0x%02X\n", _addr);

  // Trinh tu khoi tao chuan cua HD44780 de vao che do 4 bit
  delay(50);
  write4(0x03, false); delay(5);
  write4(0x03, false); delayMicroseconds(150);
  write4(0x03, false); delayMicroseconds(150);
  write4(0x02, false);            // chuyen sang che do 4 bit

  command(0x28);                  // 4 bit, 2 dong, font 5x8
  command(0x0C);                  // bat hien thi, an con tro
  command(0x06);                  // ghi xong con tro tu dich phai
  command(0x01); delay(2);        // xoa man hinh (lenh cham, cho ~2 ms)

  if (_fail) return false;        // co loi I2C giua chung -> coi nhu that bai
  clearShadow();
  _ready = true;
  return true;
}

// ---- Ham cong khai ----
bool Lcd::begin() {
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  Wire.setClock(LCD_I2C_CLOCK);   // nhanh hon -> ghi LCD it chan loop
  _lastRetry   = millis();
  _lastRefresh = millis();
  clearShadow();
  return initLcd(true);
}

void Lcd::clear() {
  if (!_ready) return;
  command(0x01); delay(2);
  clearShadow();
}

void Lcd::backlight(bool on) {
  _backlight = on ? BIT_BL : 0;
  if (!_ready) return;
  Wire.beginTransmission(_addr);
  Wire.write(_backlight);
  Wire.endTransmission();
}

void Lcd::print(uint8_t row, const char *s) {
  if (row >= LCD_ROWS) return;
  uint32_t now = millis();

  // Mat LCD thi thu ket noi lai dinh ky
  if (!_ready) {
    if (now - _lastRetry < LCD_RETRY_MS) return;
    _lastRetry = now;
    if (!initLcd(false)) return;
  }

  // Ve lai toan bo dinh ky de sua loi hien thi do nhieu
  if (now - _lastRefresh >= LCD_REFRESH_MS) {
    _lastRefresh = now;
    invalidateShadow();
  }

  // Cat hoac dien khoang trang cho du 16 ky tu (de xoa chu cu con sot)
  char line[LCD_COLS + 1];
  uint8_t i = 0;
  for (; i < LCD_COLS && s[i]; i++) line[i] = s[i];
  for (; i < LCD_COLS; i++)         line[i] = ' ';
  line[LCD_COLS] = '\0';

  if (strcmp(line, _shadow[row]) == 0) return;   // khong doi thi khoi gui

  command(0x80 | (row == 0 ? 0x00 : 0x40));      // dat con tro ve dau dong (dong 2 bat dau o 0x40)
  for (i = 0; i < LCD_COLS; i++) sendByte(line[i], true);

  if (_fail) {                                   // gap loi I2C -> danh dau mat LCD, se thu lai sau
    _fail = false;
    _ready = false;
    _lastRetry = now;
    return;
  }
  memcpy(_shadow[row], line, LCD_COLS + 1);
}
