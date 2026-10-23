#pragma once
#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <NimBLEDevice.h>
#include <ArduinoJson.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

// ============ PIN ============
#define PIN_SDA 21
#define PIN_SCL 22

// ============ BLE ============
#define DEVICE_NAME "GLPro-Neotech"
#define SVC_UUID "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define RX_UUID  "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define TX_UUID  "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"
#define MSG_MAX  300

// ============ TIMING ============
#define NAV_STALE_MS        120000UL  // nav basi > 2 menit -> "menunggu rute"
#define SPEED_STALE_MS      5000UL    // speed basi > 5 dtk -> "--"
#define SCREEN_OFF_MS       60000UL   // BLE putus > 1 mnt -> OLED off (hemat)
#define OLED_REINIT_MS      60000UL   // re-init OLED berkala (anti-EMI motor)
#define ADV_WATCHDOG_MS     3000UL    // cek iklan BLE
#define STALE_CHECK_MS      1000UL    // poll transisi stale

// ============ KONFIG OLED ============
#define OLED_CONTRAST       180       // 0..255. Turun = hemat, tapi kurang terang
#define MARQUEE_INTERVAL_MS 45UL      // 22 fps saat nama jalan panjang

// ============ STATE ============
struct NavData {
  char arrow[16];
  char distance[20];
  char street[64];
  char eta[12];
  bool autoSrc;
  unsigned long lastUpdate;
};

struct Msg { uint16_t len; char data[MSG_MAX]; };

// Objek display global (dipakai di main.cpp, display.cpp, ble_nav.cpp)
extern U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2;

// State global
extern NavData nav;
extern int speedKmh;
extern unsigned long speedAt;
extern bool speedActive;
extern bool speedStale;
extern bool navStale;

extern volatile bool bleConnected;
extern bool screenOn;
extern unsigned long lastBleActivity;
extern unsigned long lastDraw;
extern bool displayDirty;

extern QueueHandle_t cmdQueue;
extern NimBLECharacteristic* pTxChar;

// Util (diimplementasi di display.cpp)
void copyAscii(char* dst, size_t cap, const char* src);
void fitFont(const char* s, const uint8_t* const* fonts, int n, int maxW);
void drawCentered(const char* s, int y);
void drawBtBadge();

// Display (display.cpp)
void drawBoot();
void drawNavScreen(unsigned long now);

// BLE (ble_nav.cpp)
void setupBLE();
void processQueue();
