/*
 * GL Pro Neotech Smart Display
 * Fitur: Tegangan aki, jam mesin, navigasi BLE
 * Board: ESP32 Dev Module
 */

#include <Wire.h>
#include <U8g2lib.h>
#include <NimBLEDevice.h>
#include <Preferences.h>
#include <ArduinoJson.h>

// ============ PIN ============
#define PIN_SDA     21
#define PIN_SCL     22
#define PIN_VBAT    34

// ============ KALIBRASI ============
#define R1_KOHM     10.0f
#define R2_KOHM     2.2f
#define DIVIDER     ((R1_KOHM + R2_KOHM) / R2_KOHM)   // 5.545
#define V_ENGINE_ON 13.0f   // >13V = mesin hidup/charging

// ============ BLE UUID ============
#define SVC_UUID  "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define RX_UUID   "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define TX_UUID   "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

NimBLEServer*         pServer = nullptr;
NimBLECharacteristic* pTxChar = nullptr;
bool                  bleConnected = false;

Preferences prefs;

// ============ STATE ============
int           displayMode  = 0;        // 0=V,1=time,2=nav
bool          idleDisplay  = true;
float         batteryVoltage = 0.0f;
float         vbatFiltered   = 0.0f;
bool          engineRunning  = false;
unsigned long totalEngineSec = 0;      // total waktu mesin hidup
unsigned long lastEngineTick = 0;
unsigned long lastVbatRead   = 0;
unsigned long lastDraw       = 0;
unsigned long lastSaved      = 0;
unsigned long lastMinuteMark = 0;

struct {
  String arrow      = "";
  String distance   = "";
  String street     = "";
  String eta        = "";
  unsigned long lastUpdate = 0;
} nav;

// ============ BLE CALLBACKS ============
class SrvCb : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* pServer,NimBLEConnInfo& connInfo) override {
    bleConnected = true;
    NimBLEDevice::setMTU(247);            // packet besar untuk JSON nav
    Serial.println("[BLE] connected");
  }
  void onDisconnect(NimBLEServer* pServer,NimBLEConnInfo& connInfo, int reason) override {
    bleConnected = false;
    Serial.println("[BLE] disconnected");
    NimBLEDevice::startAdvertising();
  }
};

void handleCommand(const String& js);

class RxCb : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo) override {
    std::string value = pCharacteristic->getValue();
    if (value.empty()) return;
    handleCommand(String(v.c_str()));
  }
};

// ============ BACA TEGANGAN ============
float readVoltageRaw() {
  uint32_t sum = 0;
  const int N = 16;
  for (int i = 0; i < N; i++) {
    sum += analogReadMilliVolts(PIN_VBAT);
    delayMicroseconds(80);
  }
  float mv = (float)sum / N;
  return (mv / 1000.0f) * DIVIDER;
}

void updateVoltage() {
  float raw = readVoltageRaw();
  if (vbatFiltered <= 0.1f) vbatFiltered = raw;
  else                       vbatFiltered = vbatFiltered * 0.8f + raw * 0.2f;
  batteryVoltage = vbatFiltered;
}

// ============ DRAW HELPERS ============
void drawArrowIcon(int ox, int oy, const String& d) {
  int cx = ox + 14, cy = oy + 14;

  if (d == "left" || d == "turn-left" || d == "sharp-left") {
    u8g2.drawLine(cx + 10, cy, cx - 6, cy);
    u8g2.drawLine(cx + 10, cy + 1, cx - 6, cy + 1);
    u8g2.drawLine(cx - 6, cy, cx + 2, cy - 8);
    u8g2.drawLine(cx - 6, cy + 1, cx + 2, cy + 9);
  }
  else if (d == "right" || d == "turn-right" || d == "sharp-right") {
    u8g2.drawLine(cx - 10, cy, cx + 6, cy);
    u8g2.drawLine(cx - 10, cy + 1, cx + 6, cy + 1);
    u8g2.drawLine(cx + 6, cy, cx - 2, cy - 8);
    u8g2.drawLine(cx + 6, cy + 1, cx - 2, cy + 9);
  }
  else if (d == "slight-left") {
    u8g2.drawLine(cx + 5, cy + 8, cx - 6, cy - 6);
    u8g2.drawLine(cx - 6, cy - 6, cx - 8, cy + 1);
    u8g2.drawLine(cx - 6, cy - 6, cx + 1, cy - 4);
  }
  else if (d == "slight-right") {
    u8g2.drawLine(cx - 5, cy + 8, cx + 6, cy - 6);
    u8g2.drawLine(cx + 6, cy - 6, cx + 8, cy + 1);
    u8g2.drawLine(cx + 6, cy - 6, cx - 1, cy - 4);
  }
  else if (d.startsWith("uturn")) {
    u8g2.drawLine(cx - 6, cy + 8, cx - 6, cy - 2);
    u8g2.drawLine(cx - 6, cy - 2, cx + 6, cy - 2);
    u8g2.drawLine(cx + 6, cy - 2, cx + 6, cy + 4);
    u8g2.drawLine(cx + 6, cy + 6, cx + 2, cy + 2);
    u8g2.drawLine(cx + 6, cy + 6, cx + 10, cy + 2);
  }
  else if (d.startsWith("roundabout")) {
    u8g2.drawCircle(cx, cy + 2, 6, U8G2_DRAW_ALL);
    u8g2.drawLine(cx + 5, cy - 1, cx + 10, cy - 8);
    u8g2.drawLine(cx + 10, cy - 8, cx + 5, cy - 8);
    u8g2.drawLine(cx + 10, cy - 8, cx + 10, cy - 3);
  }
  else { // straight / default
    u8g2.drawLine(cx, cy + 8, cx, cy - 6);
    u8g2.drawLine(cx, cy + 8, cx + 1, cy - 6);
    u8g2.drawLine(cx, cy - 6, cx - 6, cy + 2);
    u8g2.drawLine(cx + 1, cy - 6, cx + 7, cy + 2);
  }
}

// ============ DRAW MODES ============
void drawIdle() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_helvB08_tr);
  u8g2.drawStr(2, 10, "Hai Dimas,");
  u8g2.setFont(u8g2_font_helvB12_tr);
  u8g2.drawStr(2, 28, "mau kemana");
  u8g2.drawStr(2, 46, "hari ini?");

  u8g2.setFont(u8g2_font_5x7_tr);
  char buf[32];
  snprintf(buf, sizeof(buf), "%.1fV  %s",
           batteryVoltage, bleConnected ? "BT-OK" : "BT--");
  u8g2.drawStr(2, 62, buf);
  u8g2.sendBuffer();
}

void drawVoltage() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_helvB08_tr);
  u8g2.drawStr(2, 10, "TEGANGAN AKI");

  // nilai besar, rata kanan
  u8g2.setFont(u8g2_font_helvB24_tn);
  char v[8];
  snprintf(v, sizeof(v), "%.1f", batteryVoltage);
  int w = u8g2.getStrWidth(v);
  u8g2.drawStr(126 - w - 16, 42, v);
  u8g2.setFont(u8g2_font_helvB14_tr);
  u8g2.drawStr(110, 42, "V");

  // status
  u8g2.setFont(u8g2_font_6x10_tr);
  const char* s = "OK";
  if      (batteryVoltage < 11.8f) s = "AKI LEMAH!";
  else if (batteryVoltage > 14.9f) s = "OVER CHARGE";
  else if (batteryVoltage > 13.0f) s = "CHARGING";
  u8g2.drawStr(2, 62, s);
  if (bleConnected) u8g2.drawStr(105, 62, "BT");

  u8g2.sendBuffer();
}

void drawTime() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_helvB08_tr);
  u8g2.drawStr(2, 10, "WAKTU MESIN");

  unsigned long h = totalEngineSec / 3600;
  unsigned long m = (totalEngineSec / 60) % 60;
  unsigned long s = totalEngineSec % 60;
  char buf[12];
  snprintf(buf, sizeof(buf), "%02lu:%02lu", h, m);

  u8g2.setFont(u8g2_font_helvB24_tn);
  int w = u8g2.getStrWidth(buf);
  u8g2.drawStr((128 - w) / 2, 42, buf);

  u8g2.setFont(u8g2_font_6x10_tr);
  u8g2.drawStr(2, 62, engineRunning ? "ENGINE ON" : "ENGINE OFF");
  snprintf(buf, sizeof(buf), "%02lu s", s);
  u8g2.drawStr(96, 62, buf);

  u8g2.sendBuffer();
}

void drawNav() {
  u8g2.clearBuffer();

  // ikon panah kiri
  drawArrowIcon(0, 2, nav.arrow);

  // jarak (kanan atas)
  u8g2.setFont(u8g2_font_helvB14_tn);
  String d = nav.distance;
  int dw = u8g2.getStrWidth(d.c_str());
  if (dw > 84) dw = 84;
  u8g2.drawStr(126 - dw, 18, d.c_str());

  // nama jalan (kanan)
  u8g2.setFont(u8g2_font_6x10_tr);
  String st = nav.street;
  if (st.length() > 19) st = st.substring(0, 17) + "..";
  u8g2.drawStr(36, 36, st.c_str());

  // ETA bawah
  if (nav.eta.length() > 0) {
    String eta = "ETA " + nav.eta;
    u8g2.drawStr(2, 62, eta.c_str());
  }
  if (!bleConnected) u8g2.drawStr(108, 62, "BT!");

  u8g2.sendBuffer();
}

// ============ COMMAND HANDLER ============
void handleCommand(const String& js) {
  JsonDocument doc;
  if (deserializeJson(doc, js) != DeserializationError::Ok) {
    Serial.println("[CMD] JSON invalid");
    return;
  }
  const char* cmd = doc["cmd"] | "";

  if (strcmp(cmd, "mode") == 0) {
    displayMode = constrain((int)(doc["val"] | 0), 0, 2);
    idleDisplay = false;
    lastDraw    = 0;                     // force redraw
  }
  else if (strcmp(cmd, "nav") == 0) {
    nav.arrow    = String((const char*)(doc["arrow"]    | ""));
    nav.distance = String((const char*)(doc["distance"] | ""));
    nav.street   = String((const char*)(doc["street"]   | ""));
    nav.eta      = String((const char*)(doc["eta"]      | ""));
    nav.lastUpdate = millis();
    displayMode = 2;
    idleDisplay = false;
    lastDraw    = 0;
  }
  else if (strcmp(cmd, "idle") == 0) {
    idleDisplay = true;
    lastDraw    = 0;
  }
  else if (strcmp(cmd, "reset_time") == 0) {
    totalEngineSec = 0;
    prefs.putULong("engsec", 0);
  }
  else if (strcmp(cmd, "ping") == 0) {
    if (pTxChar && bleConnected) {
      pTxChar->setValue("pong");
      pTxChar->notify();
    }
  }
}

// ============ SETUP ============
void setup() {
  Serial.begin(115200);
  delay(200);

  analogSetAttenuation(ADC_11db);
  analogReadResolution(12);

  Wire.begin(PIN_SDA, PIN_SCL);
  Wire.setClock(400000);
  u8g2.begin();
  u8g2.setContrast(255);

  prefs.begin("glpro", false);
  totalEngineSec = prefs.getULong("engsec", 0);

  // BLE
  NimBLEDevice::init("GLPro-Neotech");
  NimBLEDevice::setMTU(247);
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);  // max power utk jarak

  pServer = NimBLEDevice::createServer();
  pServer->setCallbacks(new SrvCb());

  auto* svc = pServer->createService(SVC_UUID);

  auto* pRx = svc->createCharacteristic(
    RX_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  pRx->setCallbacks(new RxCb());

  pTxChar = svc->createCharacteristic(TX_UUID, NIMBLE_PROPERTY::NOTIFY);

  svc->start();

  auto* adv = NimBLEDevice::getAdvertising();
  adv->addServiceUUID(SVC_UUID);
  adv->setScanResponseData(true);
  // adv->setMinPreferred(0x06);
  // adv->setMinPreferred(0x12);
  adv->start();

  updateVoltage();
  Serial.println("[BOOT] ready");
}

// ============ LOOP ============
void loop() {
  unsigned long now = millis();

  // --- Tegangan tiap 400 ms ---
  if (now - lastVbatRead >= 400) {
    lastVbatRead = now;
    updateVoltage();
  }

  // --- Deteksi mesin hidup ---
  bool running = (batteryVoltage > V_ENGINE_ON);
  if (running && !engineRunning) {
    lastEngineTick = now;
    engineRunning  = true;
  } else if (!running && engineRunning) {
    engineRunning = false;
    prefs.putULong("engsec", totalEngineSec);  // save saat mesin mati
  }

  if (engineRunning) {
    unsigned long dt = now - lastEngineTick;
    if (dt >= 1000) {
      totalEngineSec += dt / 1000;
      lastEngineTick += (dt / 1000) * 1000;
    }
    // save tiap ganti menit
    if ((totalEngineSec / 60) != lastMinuteMark) {
      lastMinuteMark = totalEngineSec / 60;
      prefs.putULong("engsec", totalEngineSec);
    }
  }

  // --- Render 200 ms ---
  if (now - lastDraw >= 200) {
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

  delay(2);   // beri napas RTOS
}
