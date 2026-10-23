/*
 * GL Pro Neotech Smart Display v3.0
 * Fokus: NAVIGASI + KECEPATAN. Hemat daya untuk baterai 3.7 V.
 * Board : ESP32 Dev Module. OLED : SSD1306 128x64 I2C.
 * Power : 3.3V expansion board (dari 3.7 V Li-ion via regulator board).
 */
#include "app.h"

// ============ DEFINISI STATE GLOBAL ============
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

NavData       nav;
int           speedKmh   = 0;
unsigned long speedAt    = 0;
bool          speedActive = false;
bool          speedStale  = false;
bool          navStale    = false;

volatile bool bleConnected    = false;
bool          screenOn        = true;
unsigned long lastBleActivity = 0;
unsigned long lastDraw        = 0;
bool          displayDirty    = true;

QueueHandle_t cmdQueue = nullptr;
NimBLECharacteristic* pTxChar = nullptr;

static unsigned long lastAdvCheck  = 0;
static unsigned long lastDispInit  = 0;
static unsigned long lastStaleChk  = 0;

// ============ SETUP ============
void setup() {
  Serial.begin(115200);
  delay(100);

  setCpuFrequencyMhz(80);   // hemat ~15 mA vs 160 MHz

  memset(&nav, 0, sizeof(nav));

  Wire.begin(PIN_SDA, PIN_SCL);
  Wire.setClock(400000);
  Wire.setTimeOut(50);
  u8g2.begin();
  u8g2.setBusClock(400000);
  u8g2.setContrast(OLED_CONTRAST);

  drawBoot();
  delay(1500);  // splash singkat

  cmdQueue = xQueueCreate(4, sizeof(Msg));  // cukup 4 (nav ~1 Hz)
  setupBLE();

  lastBleActivity = millis();
  lastDispInit    = millis();
  lastAdvCheck    = millis();
  lastStaleChk    = millis();
  displayDirty    = true;
  lastDraw        = 0;

  Serial.println("[BOOT] nav+speed v3.0 ready @80MHz");
}

// ============ LOOP ============
void loop() {
  unsigned long now = millis();

  // 1) Proses perintah BLE (dari queue — tidak blocking)
  processQueue();

  // 2) Advertising watchdog (independen dari layar)
  if (!bleConnected && (now - lastAdvCheck >= ADV_WATCHDOG_MS)) {
    lastAdvCheck = now;
    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    if (!adv->isAdvertising()) adv->start();
  }

  // 3) Deteksi transisi stale (murah: 1 Hz)
  if (now - lastStaleChk >= STALE_CHECK_MS) {
    lastStaleChk = now;
    bool s = speedActive && (now - speedAt >= SPEED_STALE_MS);
    if (s != speedStale) { speedStale = s; displayDirty = true; }
    bool n = nav.autoSrc && nav.lastUpdate != 0
             && (now - nav.lastUpdate >= NAV_STALE_MS);
    if (n != navStale) { navStale = n; displayDirty = true; }
  }

  // 4) Power management layar
  if (bleConnected) lastBleActivity = now;
  bool shouldBeOn = bleConnected || (now - lastBleActivity < SCREEN_OFF_MS);

  if (shouldBeOn != screenOn) {
    screenOn = shouldBeOn;
    u8g2.setPowerSave(screenOn ? 0 : 1);
    if (screenOn) {
      u8g2.initDisplay();
      u8g2.setContrast(OLED_CONTRAST);
      displayDirty = true;
    }
  }

  // 5) Re-init OLED berkala (anti-glitch EMI motor)
  if (screenOn && (now - lastDispInit >= OLED_REINIT_MS)) {
    lastDispInit = now;
    u8g2.initDisplay();
    u8g2.setPowerSave(0);
    u8g2.setContrast(OLED_CONTRAST);
    displayDirty = true;
  }

  // 6) Render — hanya jika dirty ATAU marquee aktif
  if (screenOn) {
    bool marquee = (strlen(nav.street) > 20) && !navStale
                   && (nav.arrow[0] || nav.distance[0]);
    bool need = displayDirty
                || (marquee && (now - lastDraw >= MARQUEE_INTERVAL_MS));
    if (need) {
      lastDraw     = now;
      displayDirty = false;
      drawNavScreen(now);
    }
  }

  delay(10);  // beri waktu FreeRTOS idle task (hemat + BLE auto light-sleep)
}
