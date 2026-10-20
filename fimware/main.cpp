/*
 * GL Pro Neotech Smart Display  v2.1
 * Fitur : tegangan aki realtime, jam mesin (jam:menit), navigasi BLE dari HP
 * Board : ESP32 Dev Module (Arduino core 2.x / 3.x)
 * OLED  : SSD1306 128x64 I2C (0.96")
 * Lib   : U8g2, NimBLE-Arduino 2.x, ArduinoJson 7
 *
 * Catatan kestabilan:
 *  - Callback BLE TIDAK menyentuh state display. Perintah dimasukkan ke queue,
 *    lalu diproses di loop() -> tidak ada race condition / crash.
 *  - Tidak memakai String Arduino untuk data nav (anti fragmentasi heap).
 *  - Advertising di-restart otomatis + watchdog advertising di loop.
 *  - Deteksi mesin pakai histeresis + debounce.
 *  - OLED di-init ulang berkala (jaga-jaga glitch listrik motor).
 */

#include <Arduino.h>
#include <math.h>
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

// splash saat pertama dialiri daya, lalu layar idle
#define BOOT_MS 2600UL
unsigned long bootStart = 0;

// teks idle (bisa diubah dari app, disimpan di flash)
#define DEF_NAME  "Dimas"
#define DEF_LINE2 "mau kemana hari ini?"
char          idleName[20]  = DEF_NAME;
char          idleLine2[28] = DEF_LINE2;
volatile bool forceInfo     = false;
uint8_t       infoTick      = 0;

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

// ASCII-only, buang '|' (pemisah protokol), trim spasi
void copyLabel(char* dst, size_t cap, const char* src) {
  copyAscii(dst, cap, src);
  for (char* p = dst; *p; p++) if (*p == '|') *p = '/';
  size_t n = strlen(dst);
  while (n && dst[n - 1] == ' ') dst[--n] = 0;
  char* s = dst;
  while (*s == ' ') s++;
  if (s != dst) memmove(dst, s, strlen(s) + 1);
}

// Pilih font terbesar yang muat dalam maxW piksel
void fitFont(const char* s, const uint8_t* const* fonts, int n, int maxW) {
  for (int i = 0; i < n; i++) {
    u8g2.setFont(fonts[i]);
    if (u8g2.getStrWidth(s) <= maxW) return;
  }
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

static void drawCentered(const char* s, int y) {
  int w = u8g2.getStrWidth(s);
  u8g2.drawStr((128 - w) / 2, y, s);
}

// Layar idle: status bar tipis, sapaan besar di tengah, garis jalan bergerak
void drawIdle(unsigned long now) {
  static const uint8_t* const F_NAME[]  = {u8g2_font_helvB18_tr, u8g2_font_helvB14_tr,
                                           u8g2_font_helvB12_tr, u8g2_font_helvB10_tr,
                                           u8g2_font_helvB08_tr};
  static const uint8_t* const F_LINE2[] = {u8g2_font_helvR10_tr, u8g2_font_helvR08_tr,
                                           u8g2_font_5x7_tr};
  u8g2.clearBuffer();

  // status bar
  u8g2.setFont(u8g2_font_5x7_tr);
  u8g2.drawStr(2, 8, "GL PRO NEOTECH");
  char v[10];
  snprintf(v, sizeof(v), "%.1fV", batteryVoltage);
  int vw = u8g2.getStrWidth(v);
  u8g2.drawStr(126 - vw, 8, v);
  int bx = 126 - vw - 8;
  if (bleConnected) u8g2.drawDisc(bx, 5, 2);     // BT tersambung = bulat penuh
  else              u8g2.drawCircle(bx, 5, 2);   // belum = bulat kosong
  u8g2.drawHLine(0, 11, 128);

  // sapaan
  char greet[40];
  snprintf(greet, sizeof(greet), "Hai, %s", idleName);
  fitFont(greet, F_NAME, 5, 124);
  drawCentered(greet, 36);

  // teks kedua
  fitFont(idleLine2, F_LINE2, 3, 124);
  drawCentered(idleLine2, 52);

  // garis jalan bergerak
  int off = (int)((now / 40) % 14);
  for (int x = -off; x < 128; x += 14) u8g2.drawHLine(x, 62, 8);

  u8g2.sendBuffer();
}

// ============ BOOT LOGO ============
void drawBike(int ox, int oy, unsigned long t) {
  const int R = 9;
  const int rcx = ox + 11, fcx = ox + 73, cy = oy + 20;
  const int cxs[2] = {rcx, fcx};
  const float a = t / 90.0f;
  for (int i = 0; i < 2; i++) {                  // roda + jari-jari berputar
    u8g2.drawCircle(cxs[i], cy, R, U8G2_DRAW_ALL);
    u8g2.drawCircle(cxs[i], cy, R - 1, U8G2_DRAW_ALL);
    u8g2.drawDisc(cxs[i], cy, 2);
    for (int k = 0; k < 2; k++) {
      float th = a + k * 1.5708f;
      int dx = (int)(cosf(th) * (R - 2)), dy = (int)(sinf(th) * (R - 2));
      u8g2.drawLine(cxs[i] - dx, cy - dy, cxs[i] + dx, cy + dy);
    }
  }
  u8g2.drawRBox(ox + 12, oy + 8, 24, 4, 1);      // jok
  u8g2.drawRBox(ox + 36, oy + 4, 19, 8, 3);      // tangki
  u8g2.drawBox(ox + 29, oy + 15, 15, 9);         // mesin
  u8g2.drawLine(ox + 14, oy + 12, ox + 30, oy + 17);
  u8g2.drawLine(ox + 11, oy + 20, ox + 30, oy + 22);   // swingarm
  u8g2.drawLine(ox + 12, oy + 12, ox + 11, oy + 20);   // fender belakang
  u8g2.drawLine(ox + 73, oy + 20, ox + 60, oy + 5);    // fork
  u8g2.drawLine(ox + 74, oy + 20, ox + 61, oy + 5);
  u8g2.drawLine(ox + 55, oy + 12, ox + 60, oy + 6);
  u8g2.drawHLine(ox + 57, oy + 3, 7);                  // stang
  u8g2.drawHLine(ox + 57, oy + 4, 7);
  u8g2.drawDisc(ox + 64, oy + 9, 2);                   // lampu
  u8g2.drawLine(ox + 30, oy + 25, ox + 16, oy + 27);   // knalpot
  u8g2.drawLine(ox + 16, oy + 27, ox + 13, oy + 25);
}

void drawBoot(unsigned long el) {
  float t = el / 900.0f;                         // motor meluncur masuk dari kiri
  if (t > 1) t = 1;
  float e = 1 - (1 - t) * (1 - t) * (1 - t);
  int ox = 22 - (int)((1 - e) * 110);

  u8g2.clearBuffer();
  drawBike(ox, 4, el);
  if (el > 700) {
    u8g2.setFont(u8g2_font_helvB10_tr);
    drawCentered("GL PRO NEOTECH", 52);
  }
  int bw = (int)(96.0f * el / BOOT_MS);
  if (bw > 96) bw = 96;
  u8g2.drawFrame(14, 58, 100, 5);
  u8g2.drawBox(16, 60, bw, 1);
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
  else if (!strcmp(cmd, "idle_text")) {
    if (doc["name"].is<const char*>())
      copyLabel(idleName, sizeof(idleName), doc["name"].as<const char*>());
    if (doc["line2"].is<const char*>())
      copyLabel(idleLine2, sizeof(idleLine2), doc["line2"].as<const char*>());
    if (!idleName[0])  strcpy(idleName, DEF_NAME);       // kosong -> default
    if (!idleLine2[0]) strcpy(idleLine2, DEF_LINE2);
    prefs.putString("name", idleName);
    prefs.putString("line2", idleLine2);
    idleDisplay = true;                                  // langsung tampilkan hasilnya
    forceInfo   = true;
    lastDraw    = 0;
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
  char buf[72];
  int n;
  if (forceInfo || ++infoTick >= 5) {            // tiap ~5 dtk: kirim teks idle ("I|nama|baris2")
    infoTick  = 0;
    forceInfo = false;
    n = snprintf(buf, sizeof(buf), "I|%s|%s", idleName, idleLine2);
  } else {                                       // selain itu: telemetri CSV
    n = snprintf(buf, sizeof(buf), "%.2f,%d,%lu",
                 batteryVoltage, engineRunning ? 1 : 0, totalEngineSec);
  }
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

  {
    String n  = prefs.getString("name", DEF_NAME);
    String l2 = prefs.getString("line2", DEF_LINE2);
    copyLabel(idleName, sizeof(idleName), n.c_str());
    copyLabel(idleLine2, sizeof(idleLine2), l2.c_str());
    if (!idleName[0])  strcpy(idleName, DEF_NAME);
    if (!idleLine2[0]) strcpy(idleLine2, DEF_LINE2);
  }

  updateVoltage();
  bootStart = millis();
  drawBoot(0);                               // logo langsung tampil

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
  bool booting = (now - bootStart) < BOOT_MS;
  unsigned long interval = booting ? 40 : (idleDisplay ? 60 : (displayMode == 2 ? 100 : 250));
  if (now - lastDraw >= interval) {
    lastDraw = now;
    if (booting) {
      drawBoot(now - bootStart);
    } else if (idleDisplay) {
      drawIdle(now);
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
