#pragma once

// ===== Cau hinh chung: chan GPIO va cac thong so =====

// --- Motor buoc NEMA17 + driver A4988 ---
#define PIN_STEP        25      // chan STEP
#define PIN_DIR         26      // chan DIR
#define PIN_EN          27      // chan ENABLE (muc LOW = bat driver)

#define STEPS_PER_REV   200     // NEMA17 1.8 do = 200 buoc / vong
#define MICROSTEP       1       // 1 = full step (MS1-3 de ho); 16 neu noi MS1-3 len 3V3

// Toc do tinh theo buoc / giay (xung STEP)
#define SPEED_MIN       30      // toc do thap nhat (bien tro = 0%)
#define SPEED_MAX       1500    // toc do cao nhat (bien tro = 100%), cao qua motor se mat buoc
#define SPEED_START     100     // toc do luc bat dau chay lai (de motor khong bi dung rung)
#define RAMP_SPS_PER_S  1000    // gia toc: moi giay tang/giam bao nhieu buoc/giay
#define RAMP_INTERVAL_MS 10     // cu 10 ms cap nhat toc do 1 lan

#define STOP_NHA_MOTOR  true    // true: Stop thi tat driver (motor nhe); false: van giu moment
#define DIR_MAC_DINH    true    // true = chieu thuan (DIR = HIGH)
#define DIR_PAUSE_US    20000   // nghi 20 ms khi dao chieu
#define MAX_CATCHUP     8       // so xung bu toi da khi loop bi tre

// --- Bien tro chinh toc do (chi doc ADC1, khong xung dot WiFi) ---
#define PIN_POT         13
#define POT_SAMPLE_MS   10      // doc ADC moi 10 ms
#define POT_ALPHA       0.10f   // he so loc: nho = muot nhung cham
#define POT_RAW_MIN     100     // duoi gia tri nay = 0%
#define POT_RAW_MAX     4000    // tren gia tri nay = 100%
#define POT_HYSTERESIS  1.0f    // chi doi gia tri bao ra khi lech >= 1%

// --- Nut nhan, noi GND, nhan = LOW (dung INPUT_PULLUP) ---
// Gia dinh: GPIO18 = Stop, GPIO19 = Next (chua xac nhan, doi chieu so do mach)
#define PIN_BTN_STOP    18
#define PIN_BTN_NEXT    19
#define BTN_DEBOUNCE_MS 25      // tin hieu on dinh 25 ms moi tinh la 1 lan nhan

// --- LCD 16x2 qua PCF8574 (I2C) ---
#define PIN_I2C_SDA     21
#define PIN_I2C_SCL     22
#define LCD_I2C_ADDR    0x20    // A0-A2 noi GND -> 0x20 (mach that thuong 0x27 hoac 0x3F, code tu do)
#define LCD_I2C_CLOCK   400000  // neu LCD loi thi ha ve 100000
#define LCD_COLS        16
#define LCD_ROWS        2
#define LCD_RETRY_MS    3000    // mat LCD thi thu ket noi lai moi 3 giay
#define LCD_REFRESH_MS  5000    // ve lai toan bo man hinh moi 5 giay (sua loi hien thi)

// --- UART2 voi Jetson (chua dung trong ban nay) ---
#define PIN_JETSON_RX   16
#define PIN_JETSON_TX   17
#define JETSON_BAUD     115200
