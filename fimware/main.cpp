

#include <Wire.h>
#include <U8g2lib.h>
#include <NimBLEDevice.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

// ============ PIN ============
#define PIN_SDA     21
#define PIN_SCL     22
#define PIN_VBAT    34          // ADC1_CH6 (aman dipakai bersama BLE)

// ============ KALIBRASI ============
#define R1_KOHM     10.0f       // resistor atas (ke +12V)
#define R2_KOHM     2.2f        // resistor bawah (ke GND)
#define DIVIDER     ((R1_KOHM + R2_KOHM) / R2_KOHM)   // 5.545
#define VBAT_CAL    1.000f      // koreksi: isi = V_multimeter / V_display

#define V_ENGINE_ON   13.2f     // >= ini (3 dtk) -> mesin hidup / charging
#define V_ENGINE_OFF  12.9f     // <= ini (3 dtk) -> mesin mati
#define ENGINE_DEBOUNCE_MS 3000

#define NAV_STALE_MS  120000UL  // nav otomatis tanpa update > 2 mnt -> "menunggu rute"

// ============ BLE ============
#define DEVICE_NAME "GLPro-Neotech"
#define SVC_UUID  "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define RX_UUID   "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define TX_UUID   "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"
#define MSG_MAX   300

U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

NimBLEServer*         pServer = nullptr;
NimBLECharacteristic* pTxChar = nullptr;
volatile bool         bleConnected = false;

struct Msg { uint16_t len; char data[MSG_MAX]; };
QueueHandle_t cmdQueue = nullptr;

Preferences prefs;

// ============ STATE ============
int           displayMode    = 0;      // 0=V, 1=waktu, 2=nav
bool          idleDisplay    = true;   // true = sapaan "Hai Dimas"
float         batteryVoltage = 0.0f;
float         vbatFiltered   = 0.0f;
bool          engineRunning  = false;
unsigned long totalEngineSec = 0;
unsigned long sessionSec     = 0;
unsigned long lastEngineTick = 0;
unsigned long engineAccMs    = 0;
unsigned long lastSavedSec   = 0;
unsigned long engineCondSince = 0;
unsigned long lastVbatRead   = 0;
unsigned long lastDraw       = 0;
unsigned long lastNotify     = 0;
unsigned long lastDispInit   = 0;
unsigned long lastAdvCheck   = 0;
uint8_t       contrastVal    = 255;

struct NavData {
  char arrow[16];
  char distance[20];
  char street[64];
  char eta[12];
  bool autoSrc;
  unsigned long lastUpdate;
} nav;

// ============ BLE CALLBACKS (jalan di task BLE -> hanya enqueue) ============
class SrvCb : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* s, NimBLEConnInfo& info) override {
    bleConnected = true;
    // interval 30-60 ms, latency 0, supervision timeout 4 dtk (stabil di motor)
    s->updateConnParams(info.getConnHandle(), 24, 48, 0, 400);
  }
  void onDisconnect(NimBLEServer* s, NimBLEConnInfo& info, int reason) override {
    bleConnected = false;
  }
};

class RxCb : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& info) override {
    NimBLEAttValue v = c->getValue();
    size_t n = v.length();
    if (n == 0 || n >= MSG_MAX) return;
    Msg m;
    memcpy(m.data, v.data(), n);
    m.data[n] = 0;
    m.len = n;
    if (xQueueSend(cmdQueue, &m, 0) != pdTRUE) {   // penuh -> buang yang terlama
      Msg tmp;
      xQueueReceive(cmdQueue, &tmp, 0);
      xQueueSend(cmdQueue, &m, 0);
    }
  }
};

// ============ UTIL ============
// Salin string ke buffer, ganti karakter non-ASCII jadi '?'
void copyAscii(char* dst, size_t cap, const char* src) {
  size_t i = 0;
  for (; src && src[i] && i < cap - 1; i++) {
    unsigned char ch = (unsigned char)src[i];
    dst[i] = (ch >= 32 && ch < 127) ? (char)ch : '?';
  }
  dst[i] = 0;
}

// ============ BACA TEGANGAN ============
float readVoltageRaw() {
  uint32_t sum = 0;
  const int N = 16;
  for (int i = 0; i < N; i++) {
    sum += analogReadMilliVolts(PIN_VBAT);
    delayMicroseconds(80);
  }
  float mv = (float)sum / N;
  return (mv / 1000.0f) * DIVIDER * VBAT_CAL;
}

void updateVoltage() {
  float raw = readVoltageRaw();
  if (vbatFiltered <= 0.1f) vbatFiltered = raw;
  else                      vbatFiltered = vbatFiltered * 0.8f + raw * 0.2f;
  batteryVoltage = vbatFiltered;
}

// ============ ENGINE ============
void updateEngine(unsigned long now) {
  // kandidat perpindahan state (histeresis)
  bool wantToggle = engineRunning ? (batteryVoltage <= V_ENGINE_OFF)
                                  : (batteryVoltage >= V_ENGINE_ON);
  if (wantToggle) {
    if (engineCondSince == 0) engineCondSince = now;
    else if (now - engineCondSince >= ENGINE_DEBOUNCE_MS) {
      engineRunning = !engineRunning;
      engineCondSince = 0;
      lastEngineTick = now;
      engineAccMs = 0;
      if (!engineRunning) {                      // mesin mati -> simpan
        prefs.putULong("engsec", totalEngineSec);
        lastSavedSec = totalEngineSec;
      }
    }
  } else {
    engineCondSince = 0;
  }

  if (engineRunning) {
    unsigned long dt = now - lastEngineTick;
    lastEngineTick = now;
    engineAccMs += dt;
    while (engineAccMs >= 1000) {
      engineAccMs -= 1000;
      totalEngineSec++;
      sessionSec++;
    }
    // simpan tiap 60 detik (kunci kontak bisa dimatikan kapan saja -> listrik putus)
    if (totalEngineSec - lastSavedSec >= 60) {
      prefs.putULong("engsec", totalEngineSec);
      lastSavedSec = totalEngineSec;
    }
  }
}

// ============ ARROW ============
// Tebal 3 px
void thickLine(int x0, int y0, int x1, int y1) {
  bool horiz = abs(x1 - x0) >= abs(y1 - y0);
  for (int o = -1; o <= 1; o++) {
    if (horiz) u8g2.drawLine(x0, y0 + o, x1, y1 + o);
    else       u8g2.drawLine(x0 + o, y0, x1 + o, y1);
  }
}

// Kepala panah: ujung (tx,ty), arah (dx,dy) ternormalisasi
void arrowHead(int tx, int ty, float dx, float dy) {
  const float L = 13.0f, W = 11.0f;
  float bx = tx - dx * L, by = ty - dy * L;
  float px = -dy, py = dx;
  u8g2.drawTriangle(tx, ty,
                    (int)(bx + px * W), (int)(by + py * W),
                    (int)(bx - px * W), (int)(by - py * W));
}

// Area ikon: x 0..43, y 0..47
void drawArrowIcon(const char* d) {
  const float S = 0.7071f;
  if (!strcmp(d, "left")) {
    thickLine(28, 46, 28, 24); thickLine(28, 24, 16, 24); arrowHead(3, 24, -1, 0);
  } else if (!strcmp(d, "right")) {
    thickLine(16, 46, 16, 24); thickLine(16, 24, 28, 24); arrowHead(41, 24, 1, 0);
  } else if (!strcmp(d, "slight-left")) {
    thickLine(28, 46, 28, 28); thickLine(28, 28, 17, 16); arrowHead(8, 7, -S, -S);
  } else if (!strcmp(d, "slight-right")) {
    thickLine(16, 46, 16, 28); thickLine(16, 28, 27, 16); arrowHead(36, 7, S, -S);
  } else if (!strcmp(d, "sharp-left")) {
    thickLine(30, 46, 30, 16); thickLine(30, 16, 16, 30); arrowHead(8, 38, -S, S);
  } else if (!strcmp(d, "sharp-right")) {
    thickLine(14, 46, 14, 16); thickLine(14, 16, 28, 30); arrowHead(36, 38, S, S);
  } else if (!strncmp(d, "uturn", 5)) {
    thickLine(14, 46, 14, 14); thickLine(14, 14, 30, 14); thickLine(30, 14, 30, 30);
    arrowHead(30, 43, 0, 1);
  } else if (!strncmp(d, "roundabout", 10)) {
    u8g2.drawCircle(22, 30, 10, U8G2_DRAW_ALL);
    u8g2.drawCircle(22, 30, 9,  U8G2_DRAW_ALL);
    thickLine(22, 46, 22, 40);
    thickLine(29, 23, 32, 16); arrowHead(40, 5, S, -S);
  } else if (!strcmp(d, "arrive")) {
    u8g2.drawDisc(22, 18, 10);
    u8g2.drawTriangle(13, 23, 31, 23, 22, 44);
    u8g2.setDrawColor(0);
    u8g2.drawDisc(22, 18, 4);
    u8g2.setDrawColor(1);
  } else {                                   // straight / default
    thickLine(22, 46, 22, 18); arrowHead(22, 3, 0, -1);
  }
}

// ============ DRAW ============
void drawBtBadge() {
  if (bleConnected) {
    u8g2.setFont(u8g2_font_5x7_tr);
    u8g2.drawFrame(108, 2, 19, 11);
    u8g2.drawStr(112, 10, "BT");
  }
}

void drawIdle() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_helvB08_tr);
  u8g2.drawStr(2, 10, "Hai Dimas,");
  u8g2.setFont(u8g2_font_helvB12_tr);
  u8g2.drawStr(2, 28, "mau kemana");
  u8g2.drawStr(2, 46, "hari ini?");

  u8g2.setFont(u8g2_font_5x7_tr);
  char buf[32];
  snprintf(buf, sizeof(buf), "%.1fV   %s", batteryVoltage,
           bleConnected ? "BT-OK" : "BT--");
  u8g2.drawStr(2, 62, buf);
  u8g2.sendBuffer();
}

void drawVoltage() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_helvB08_tr);
  u8g2.drawStr(2, 10, "TEGANGAN AKI");
  drawBtBadge();

  // nilai besar, di tengah
  char v[8];
  snprintf(v, sizeof(v), "%.1f", batteryVoltage);
  u8g2.setFont(u8g2_font_helvB24_tr);
  int w = u8g2.getStrWidth(v);
  u8g2.setFont(u8g2_font_helvB14_tr);
  int wv = u8g2.getStrWidth("V");
  int x0 = (128 - (w + 4 + wv)) / 2;
  u8g2.setFont(u8g2_font_helvB24_tr);
  u8g2.drawStr(x0, 40, v);
  u8g2.setFont(u8g2_font_helvB14_tr);
  u8g2.drawStr(x0 + w + 4, 40, "V");

  // gauge 10.5 - 15.0 V
  const float VMIN = 10.5f, VMAX = 15.0f;
  float f = (batteryVoltage - VMIN) / (VMAX - VMIN);
  if (f < 0) f = 0; if (f > 1) f = 1;
  u8g2.drawFrame(2, 45, 124, 7);
  u8g2.drawBox(4, 47, (int)(120 * f), 3);
  int tLow = 2 + (int)(124 * ((11.8f - VMIN) / (VMAX - VMIN)));
  int tChg = 2 + (int)(124 * ((13.2f - VMIN) / (VMAX - VMIN)));
  u8g2.drawVLine(tLow, 43, 2);
  u8g2.drawVLine(tChg, 43, 2);

  // status
  const char* s;
  bool warn = false;
  if      (batteryVoltage < 11.8f) { s = "AKI LEMAH!";  warn = true; }
  else if (batteryVoltage > 14.9f) { s = "OVER CHARGE!"; warn = true; }
  else if (batteryVoltage >= V_ENGINE_ON) s = "CHARGING";
  else if (batteryVoltage >= 12.4f) s = "AKI OK";
  else                              s = "AKI CUKUP";

  u8g2.setFont(u8g2_font_helvB08_tr);
  if (warn && ((millis() / 500) % 2)) {
    u8g2.drawBox(0, 54, 128, 10);
    u8g2.setDrawColor(0);
    u8g2.drawStr(2, 62, s);
    u8g2.setDrawColor(1);
  } else {
    u8g2.drawStr(2, 62, s);
  }
  u8g2.sendBuffer();
}

void drawTime() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_helvB08_tr);
  u8g2.drawStr(2, 10, "WAKTU MESIN HIDUP");
  drawBtBadge();

  unsigned long h = totalEngineSec / 3600;
  unsigned long m = (totalEngineSec / 60) % 60;
  char hh[8], mm[4];
  snprintf(hh, sizeof(hh), "%lu", h);
  snprintf(mm, sizeof(mm), "%02lu", m);

  u8g2.setFont(u8g2_font_helvB24_tr);
  int wh = u8g2.getStrWidth(hh);
  int wm = u8g2.getStrWidth(mm);
  int wc = u8g2.getStrWidth(":");
  int x0 = (128 - (wh + wc + wm + 4)) / 2;
  u8g2.drawStr(x0, 42, hh);
  bool colon = !engineRunning || ((millis() / 500) % 2);   // kedip saat mesin hidup
  if (colon) u8g2.drawStr(x0 + wh + 2, 42, ":");
  u8g2.drawStr(x0 + wh + wc + 4, 42, mm);

  u8g2.setFont(u8g2_font_5x7_tr);
  u8g2.drawStr(x0, 51, "JAM");
  u8g2.drawStr(x0 + wh + wc + 4, 51, "MENIT");

  u8g2.setFont(u8g2_font_helvB08_tr);
  u8g2.drawStr(2, 62, engineRunning ? "ENGINE ON" : "ENGINE OFF");
  char buf[20];
  snprintf(buf, sizeof(buf), "SESI %lu:%02lu", sessionSec / 3600, (sessionSec / 60) % 60);
  int sw = u8g2.getStrWidth(buf);
  u8g2.drawStr(126 - sw, 62, buf);
  u8g2.sendBuffer();
}

void drawNavWaiting() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_helvB08_tr);
  u8g2.drawStr(2, 10, "NAVIGASI");
  drawBtBadge();
  u8g2.setFont(u8g2_font_helvB12_tr);
  u8g2.drawStr(2, 34, "Menunggu");
  u8g2.drawStr(2, 50, "rute...");
  u8g2.setFont(u8g2_font_5x7_tr);
  u8g2.drawStr(2, 62, "Mulai rute di Google Maps");
  u8g2.sendBuffer();
}

void drawNav() {
  bool empty = (nav.arrow[0] == 0 && nav.distance[0] == 0 && nav.street[0] == 0);
  bool stale = nav.autoSrc && nav.lastUpdate != 0 && (millis() - nav.lastUpdate > NAV_STALE_MS);
  if (empty || stale) { drawNavWaiting(); return; }

  u8g2.clearBuffer();

  // ikon panah (kiri)
  drawArrowIcon(nav.arrow);

  // jarak (kanan atas) - font besar, turun ke font lebih kecil jika kepanjangan
  u8g2.setFont(u8g2_font_helvB18_tr);
  int dw = u8g2.getStrWidth(nav.distance);
  if (dw > 80) { u8g2.setFont(u8g2_font_helvB14_tr); dw = u8g2.getStrWidth(nav.distance); }
  u8g2.drawStr(126 - dw, 26, nav.distance);

  // ETA
  if (nav.eta[0]) {
    char e[20];
    snprintf(e, sizeof(e), "ETA %s", nav.eta);
    u8g2.setFont(u8g2_font_helvB08_tr);
    int ew = u8g2.getStrWidth(e);
    u8g2.drawStr(126 - ew, 42, e);
  }
  if (bleConnected == false) {
    u8g2.setFont(u8g2_font_5x7_tr);
    u8g2.drawStr(100, 8, "BT!");
  }

  // garis pemisah + nama jalan (marquee jika panjang)
  u8g2.drawHLine(0, 48, 128);
  u8g2.setFont(u8g2_font_helvB08_tr);
  int sw = u8g2.getStrWidth(nav.street);
  u8g2.setClipWindow(0, 50, 128, 64);
  if (sw <= 124) {
    u8g2.drawStr(2, 62, nav.street);
  } else {
    int cycle = sw + 40;
    int pos = (int)((millis() / 45) % cycle);
    u8g2.drawStr(2 - pos, 62, nav.street);
    u8g2.drawStr(2 - pos + cycle, 62, nav.street);
  }
  u8g2.setMaxClipWindow();

  u8g2.sendBuffer();
}

// ============ COMMAND HANDLER (dipanggil dari loop) ============
void handleCommand(const char* js) {
  JsonDocument doc;
  if (deserializeJson(doc, js) != DeserializationError::Ok) {
    Serial.println("[CMD] JSON invalid");
    return;
  }
  const char* cmd = doc["cmd"] | "";

  if (!strcmp(cmd, "mode")) {
    displayMode = constrain((int)(doc["val"] | 0), 0, 2);
    idleDisplay = false;
    lastDraw = 0;
  }
  else if (!strcmp(cmd, "nav")) {
    copyAscii(nav.arrow,    sizeof(nav.arrow),    doc["arrow"]    | "");
    copyAscii(nav.distance, sizeof(nav.distance), doc["distance"] | "");
    copyAscii(nav.street,   sizeof(nav.street),   doc["street"]   | "");
    copyAscii(nav.eta,      sizeof(nav.eta),      doc["eta"]      | "");
    nav.autoSrc    = (int)(doc["auto"] | 0) == 1;
    nav.lastUpdate = millis();
    if ((int)(doc["go"] | 1) == 1) {          // pindah ke layar nav
      displayMode = 2;
      idleDisplay = false;
    }
    lastDraw = 0;
  }
  else if (!strcmp(cmd, "idle")) {
    idleDisplay = true;
    lastDraw = 0;
  }
  else if (!strcmp(cmd, "contrast")) {
    contrastVal = constrain((int)(doc["val"] | 255), 0, 255);
    u8g2.setContrast(contrastVal);
  }
  else if (!strcmp(cmd, "reset_time")) {
    totalEngineSec = 0;
    sessionSec = 0;
    lastSavedSec = 0;
    prefs.putULong("engsec", 0);
  }
  else if (!strcmp(cmd, "ping")) {
    if (pTxChar && bleConnected) {
      pTxChar->setValue((const uint8_t*)"pong", 4);
      pTxChar->notify();
    }
  }
}

void processQueue() {
  Msg m;
  int guard = 6;                                // batasi per putaran loop
  while (guard-- > 0 && xQueueReceive(cmdQueue, &m, 0) == pdTRUE) {
    handleCommand(m.data);
  }
}

// ============ TELEMETRI KE HP (1 Hz, CSV pendek muat di MTU default) ============
void notifyTelemetry(unsigned long now) {
  if (!bleConnected || !pTxChar) return;
  if (now - lastNotify < 1000) return;
  lastNotify = now;
  char buf[32];
  int n = snprintf(buf, sizeof(buf), "%.2f,%d,%lu",
                   batteryVoltage, engineRunning ? 1 : 0, totalEngineSec);
  pTxChar->setValue((const uint8_t*)buf, n);
  pTxChar->notify();
}

// ============ ADVERTISING ============
void setupAdvertising() {
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();

  NimBLEAdvertisementData advData;
  advData.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
  advData.addServiceUUID(SVC_UUID);          // 128-bit UUID di paket utama (untuk filter scan HP)

  NimBLEAdvertisementData scanData;
  scanData.setName(DEVICE_NAME);             // nama di scan response

  adv->setAdvertisementData(advData);
  adv->setScanResponseData(scanData);
  adv->start();
}

// ============ SETUP ============
void setup() {
  Serial.begin(115200);
  delay(100);
  setCpuFrequencyMhz(160);                   // lebih adem di dalam box motor

  analogSetAttenuation(ADC_11db);
  analogReadResolution(12);

  memset(&nav, 0, sizeof(nav));

  Wire.begin(PIN_SDA, PIN_SCL);
  Wire.setClock(400000);
  Wire.setTimeOut(50);                       // jangan hang jika I2C error
  u8g2.begin();
  u8g2.setBusClock(400000);
  u8g2.setContrast(contrastVal);

  prefs.begin("glpro", false);
  totalEngineSec = prefs.getULong("engsec", 0);
  lastSavedSec   = totalEngineSec;

  updateVoltage();
  drawIdle();                                // tampil sapaan secepat mungkin

  cmdQueue = xQueueCreate(6, sizeof(Msg));

  // BLE
  NimBLEDevice::init(DEVICE_NAME);
  NimBLEDevice::setMTU(247);
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);

  pServer = NimBLEDevice::createServer();
  pServer->setCallbacks(new SrvCb());
  pServer->advertiseOnDisconnect(true);

  NimBLEService* svc = pServer->createService(SVC_UUID);

  NimBLECharacteristic* pRx = svc->createCharacteristic(
      RX_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  pRx->setCallbacks(new RxCb());

  pTxChar = svc->createCharacteristic(TX_UUID, NIMBLE_PROPERTY::NOTIFY);

  svc->start();
  setupAdvertising();

  lastDispInit = millis();
  Serial.println("[BOOT] ready");
}

// ============ LOOP ============
void loop() {
  unsigned long now = millis();

  // tegangan tiap 400 ms
  if (now - lastVbatRead >= 400) {
    lastVbatRead = now;
    updateVoltage();
  }

  updateEngine(now);
  processQueue();
  notifyTelemetry(now);

  // watchdog advertising: pastikan selalu bisa ditemukan HP saat tidak terhubung
  if (now - lastAdvCheck >= 5000) {
    lastAdvCheck = now;
    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    if (!bleConnected && !adv->isAdvertising()) adv->start();
  }

  // re-init OLED tiap 30 dtk (pulih dari glitch tegangan/EMI)
  if (now - lastDispInit >= 30000) {
    lastDispInit = now;
    u8g2.initDisplay();
    u8g2.setPowerSave(0);
    u8g2.setContrast(contrastVal);
    lastDraw = 0;
  }

  // render
  unsigned long interval = (!idleDisplay && displayMode == 2) ? 100 : 250;
  if (now - lastDraw >= interval) {
    lastDraw = now;
    if (idleDisplay) {
      drawIdle();
    } else {
      switch (displayMode) {
        case 0: drawVoltage(); break;
        case 1: drawTime();    break;
        case 2: drawNav();     break;
      }
    }
  }

  delay(2);
}
