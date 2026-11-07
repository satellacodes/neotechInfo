
#include <Arduino.h>
#include <math.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <NimBLEDevice.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <esp_sleep.h>
#include <driver/rtc_io.h>

// ============ PIN ============
#define PIN_SDA     21
#define PIN_SCL     22
#define PIN_VBAT    34          // ADC1_CH6 (aman dipakai bersama BLE)
#define PIN_BOOT    0           // tombol BOOT bawaan board: bangunkan dari tidur / tahan 8 dtk = reset pelacak

// ---- KEYLESS: keluaran ke relay (tiap relay lewat driver MOSFET N low-side, koil 12 V dari aki) ----
#define PIN_R_IGN     25   // relay kontak: PARALEL dengan saklar kunci asli -> bus kontak dapat +12 V
#define PIN_R_CDI     26   // relay daya CDI: SERI di kabel power CDI
#define PIN_R_START   27   // relay tombol starter: PARALEL dengan tombol starter
#define PIN_BUZZ      16   // buzzer aktif 3-24 V (sisi GND-nya lewat 1 MOSFET N low-side); pengganti klakson, tanpa relay
#define PIN_R_TURN_L  17   // relay +12 V (sekring) ke kabel riting kiri
#define PIN_R_TURN_R  18   // relay +12 V (sekring) ke kabel riting kanan
// ---- KEYLESS: masukan ----
#define PIN_KEYSENSE  33   // bus kontak -> 27k seri, 10k ke GND, zener 3.3 V paralel 10k (RTC GPIO: bisa membangunkan dari tidur)
#define PIN_NEUTRAL   35   // kabel lampu N -> 33k seri, 10k ke GND (GPIO input-only)

// ============ KALIBRASI ============
#define R1_KOHM     10.0f       // resistor atas (ke +12V)
#define R2_KOHM     2.0f        // resistor bawah (ke GND): 5x 10k paralel = 2.0k (jika pakai 2.2k, ganti 2.2f)
#define DIVIDER     ((R1_KOHM + R2_KOHM) / R2_KOHM)   // 6.0 (R2=2.0k) / 5.545 (R2=2.2k)
#define VBAT_CAL    1.000f      // koreksi: isi = V_multimeter / V_display

// Deteksi mesin (disetel dari ukuran di GL Pro: idle dingin tanpa lampu depan 12.9-13.1 V,
// idle + lampu depan hanya 11.3-11.6 V = SAMA dengan kontak ON mesin mati + lampu depan).
// Jadi tegangan rendah TIDAK dianggap mesin mati. Mesin dianggap hidup begitu tegangan >= V_ENGINE_ON
// (3 dtk), lalu tetap dianggap hidup selama dalam ENGINE_HOLD_MS terakhir pernah >= V_ENGINE_ON
// (menggas sebentar menyegarkan penahan) dan tegangan belum jatuh <= V_ENGINE_FLOOR (mesin mati/starter).
#define V_ENGINE_ON      13.0f
#define V_ENGINE_FLOOR   10.0f
#define ENGINE_HOLD_MS   300000UL   // 5 menit
#define ENGINE_DEBOUNCE_MS 3000

#define NAV_STALE_MS  120000UL  // nav otomatis tanpa update > 2 mnt -> "menunggu rute"

// ============ HEMAT DAYA / PARKIR ============
// Sistem dicatu terus dari aki (bukan kabel kontak). Setelah mesin mati dan tidak ada aktivitas
// selama `parkMin` menit, ESP32 tidur dalam (OLED dimatikan). Tiap PARK_WAKE_S detik ia bangun
// sebentar: cek tegangan (mesin menyala? aki lemah?) dan, bila jadwalnya tiba, memancarkan beacon Find Hub.
#define PARK_WAKE_S         10        // jeda bangun saat parkir (menentukan seberapa cepat display muncul setelah mesin hidup)
#define PARK_LOWBAT_WAKE_S  60        // aki lemah: bangun lebih jarang, tanpa beacon
#define V_WAKE_RUN          13.0f     // >= ini saat bangun = mesin hidup/charging -> boot normal
#define V_CUTOFF            11.8f     // < ini saat parkir -> hentikan beacon (selamatkan aki untuk starter)
#define V_RESUME            12.3f     // >= ini -> beacon aktif lagi (aki GL Pro istirahat ~12.4 V; 12.5 tidak akan tercapai)
#define DEF_PARK_MIN        3

// ============ KEYLESS ============
// Mode 0 = MANUAL : kunci asli dipakai; ESP32 hanya menutup relay CDI selama kunci ON.
// Mode 1 = KEYLESS: CDI TERPUTUS selama terkunci. Hidupkan kontak / starter hanya dari app
//                   bila HP terautentikasi (pairing + passkey) DAN berada dalam jarak (RSSI >= ambang).
#define KL_REQUIRE_PAIRING  1        // 1 = kendali relay hanya lewat link terenkripsi + passkey. Jangan 0 kecuali debug.
#define KL_NEAR_DEFAULT     (-72)    // dBm, diganti hasil kalibrasi 5 m dari app
#define KL_NEAR_HYST        6        // dB: keluar dari "dekat" bila < ambang - 6
#define KL_START_MS         3500UL   // starter menyala maks. 3,5 dtk
#define KL_COOL_MS          3000UL   // jeda antar percobaan starter
#define KL_FAIL_MAX         3        // 3 percobaan gagal -> kunci starter 30 dtk (lindungi motor starter)
#define KL_LOCKOUT_MS       30000UL
#define KL_START_MIN_V      10.8f    // aki di bawah ini: starter ditolak
#define V_START_DIP         11.2f    // tegangan turun di bawah ini = starter sedang menarik arus
#define V_START_RECOVER     12.6f    // lalu pulih di atas ini = mesin sudah menyala
#define KL_FAR_WARN_MS      6000UL   // HP/link hilang selama ini saat kontak ON -> kedip strobo pesawat
#define KL_KILL_WARN_MS     5000UL   // peringatan riting sebelum mesin yang sedang jalan dimatikan
#define KL_MOVING_KMH       15       // kill ditolak bila kecepatan GPS dari app di atas ini (kecuali force)
#define KL_AUTOOFF_MS       180000UL // kontak ON tapi mesin tak pernah menyala -> mati sendiri
#define KL_SESSION_IDLE_MS  45000UL  // sesi app tanpa perintah (kontak mati) -> putus & tidur
#define KL_NO_SESSION_MS    4000UL   // tidak ada sesi -> tidur
#define KL_PAIR_MS          120000UL
#define KL_ALARM_COOL_MS    20000UL
#define KL_ADV_WINDOW_MS    200UL    // jendela iklan terhubung tiap bangun
#define KL_BOOST_SEC        120      // setelah sesi: siklus bangun 2 dtk selama 2 menit (kalau Anda kembali sebentar)
static const uint8_t  WAKE_PROF_S[3] = {2, 4, 8};      // Sigap / Normal / Hemat
static const uint16_t LOCK_PROF_S[4] = {30, 120, 300, 0};   // 0 = tidak pernah kunci otomatis

// ============ FIND HUB (FMDN) ============
// Mengikuti firmware referensi GoogleFindMyTools: frame service-data 0xFEAA, tipe 0x41, 20 byte
// "advertisement key" hasil registrasi lewat skrip Python (bukan jalur resmi Fast Pair Google).
#define FMDN_FRAME_TYPE     0x41
#define FIND_SLOT_MS        400UL     // saat nyala & mesin hidup: 400 ms beacon ...
#define APP_SLOT_MS         1600UL    // ... diselingi 1600 ms iklan untuk app (siklus 2 dtk)
#define FIND_TX_DBM         9

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
uint8_t       brightPct      = 100;    // kecerahan 5..100 % (dari app, tersimpan di flash)
uint8_t       savedBright    = 100;

// pelacak Find Hub + hemat daya
bool          fhnEnabled     = false;
bool          fhnKeySet      = false;
uint8_t       fhnKey[20]     = {0};
uint8_t       fhnProfile     = 1;      // 0 ketat, 1 seimbang, 2 hemat
uint8_t       parkMin        = DEF_PARK_MIN;
char          pinStr[10]     = "";     // PIN pengubah pengaturan pelacak ("" = belum diatur)
const char*   errCode        = nullptr;
bool          forceStatus    = false;
unsigned long lastKeepAwake  = 0;
unsigned long advSwitchAt    = 0;
unsigned long lastAdvKick    = 0;
enum AdvMode : uint8_t { ADV_NONE, ADV_APP, ADV_FIND };
AdvMode       advMode        = ADV_NONE;

// bertahan selama deep sleep
RTC_DATA_ATTR uint32_t rtcSecAcc  = 0;      // detik sejak beacon Find Hub terakhir
RTC_DATA_ATTR uint16_t rtcBoost   = 0;      // detik sisa siklus bangun cepat setelah sesi app
RTC_DATA_ATTR uint8_t  rtcLowBatt = 0;

// splash saat pertama dialiri daya, lalu layar idle
#define BOOT_MS 2600UL
unsigned long bootStart = 0;

// teks idle (bisa diubah dari app, disimpan di flash)
#define DEF_NAME  "Dimas"
#define DEF_LINE2 "mau kemana hari ini?"
char          idleName[20]  = DEF_NAME;
char          idleLine2[28] = DEF_LINE2;
volatile bool forceInfo     = false;

// kecepatan GPS dari HP (cmd "speed")
int           speedKmh      = 0;
unsigned long speedAt       = 0;
bool          speedActive   = false;   // true = HP mengirim kecepatan -> layout nav menampilkan speed
#define SPEED_STALE_MS 5000UL
uint8_t       infoTick      = 0;

struct NavData {
  char arrow[16];
  char distance[20];
  char street[64];
  char eta[12];
  bool autoSrc;
  unsigned long lastUpdate;
} nav;

// ============ KEYLESS: STATE ============
uint8_t       klMode        = 0;          // 0 manual, 1 keyless
bool          ignOn         = false;      // sesi kontak keyless: relay kontak + CDI hidup
bool          ignRelayOn    = false;      // kondisi fisik relay kontak (juga hidup sebentar saat "cari motor")
bool          cdiOn         = false;
bool          ignSawCharge  = false;      // sejak kontak ON pernah terlihat pengisian (mesin pernah hidup)
unsigned long ignAt         = 0;
unsigned long lastHighMs    = 0;          // terakhir tegangan >= V_ENGINE_ON
uint8_t       wakeProf      = 1;          // 0 sigap 2s, 1 normal 4s, 2 hemat 8s
uint8_t       lockProf      = 1;          // 0 30s, 1 2mnt, 2 5mnt, 3 mati
bool          neutralReq    = true;       // starter butuh posisi netral
bool          autoOff       = true;       // kontak ON tanpa mesin -> mati sendiri
const bool    hornIgn       = false;      // (v4.1) buzzer langsung dari aki: tidak butuh bus kontak. Field 'hi' di frame K dipertahankan = 0
bool          warnFar       = true;       // kedip pesawat + 2 bip bila HP menjauh saat kontak ON
int8_t        rssiThr       = KL_NEAR_DEFAULT;
float         rssiEma       = -100.0f;
int           rssiN         = 0;
bool          nearOk        = false;
volatile uint16_t connHandle = 0xFFFF;
unsigned long lastRssiAt    = 0;
unsigned long calibUntil    = 0;
long          calibSum      = 0;
int           calibCnt      = 0;
volatile unsigned long pairUntil = 0;
uint32_t      pairPin       = 0;
unsigned long oledForceUntil = 0;
uint16_t      tamperCount   = 0;
bool          klForce       = false;      // kirim frame K secepatnya
char          noticeBuf[28] = "";
bool          bleReady      = false;
bool          quietWake     = false;      // bangun karena sesi app saat parkir: OLED tetap mati
uint8_t       frameN        = 0;

inline bool pairingActive() { return pairUntil != 0 && (long)(pairUntil - millis()) > 0; }
void setNotice(const char* s) { strncpy(noticeBuf, s, sizeof(noticeBuf) - 1); noticeBuf[sizeof(noticeBuf) - 1] = 0; }

// ============ BLE CALLBACKS (jalan di task BLE -> hanya enqueue / set flag) ============
class SrvCb : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* s, NimBLEConnInfo& info) override {
    bleConnected = true;
    connHandle   = info.getConnHandle();
    rssiN = 0; nearOk = false;
    // interval 30-60 ms, latency 0, supervision timeout 4 dtk (stabil di motor)
    s->updateConnParams(info.getConnHandle(), 24, 48, 0, 400);
    // di luar mode pairing passkey diacak -> HP asing tidak bisa menebak
    if (!pairingActive()) NimBLEDevice::setSecurityPasskey(100000 + (esp_random() % 900000));
  }
  void onDisconnect(NimBLEServer* s, NimBLEConnInfo& info, int reason) override {
    bleConnected = false;
    connHandle   = 0xFFFF;
    nearOk = false; rssiN = 0;
  }
  void onAuthenticationComplete(NimBLEConnInfo& info) override {
    if (info.isEncrypted() && pairingActive()) {   // pairing sukses -> tutup mode pairing
      pairUntil = 0;
      NimBLEDevice::setSecurityPasskey(100000 + (esp_random() % 900000));
      setNotice("paired");
      klForce = true;
    }
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
  if (klMode == 1 && !ignOn && !ignRelayOn) {          // keyless terkunci: mesin tak mungkin hidup (tegangan tinggi = charger)
    if (engineRunning) { engineRunning = false; prefs.putULong("engsec", totalEngineSec); lastSavedSec = totalEngineSec; }
    engineCondSince = 0;
    return;
  }
  // kandidat perpindahan state (histeresis)
  if (batteryVoltage >= V_ENGINE_ON) lastHighMs = now;
  bool wantToggle = engineRunning
      ? (batteryVoltage <= V_ENGINE_FLOOR || (now - lastHighMs) >= ENGINE_HOLD_MS)
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
  u8g2.drawStr(2, 8, klMode ? (ignOn ? "KEYLESS  ON" : "KEYLESS LOCK") : "GL PRO NEOTECH");
  char v[10];
  snprintf(v, sizeof(v), "%.1fV", batteryVoltage);
  int vw = u8g2.getStrWidth(v);
  u8g2.drawStr(126 - vw, 8, v);
  int bx = 126 - vw - 8;
  if (bleConnected) u8g2.drawDisc(bx, 5, 2);     // BT tersambung = bulat penuh
  else              u8g2.drawCircle(bx, 5, 2);   // belum = bulat kosong
  if (fhnEnabled && fhnKeySet) u8g2.drawStr(77, 8, "FH");   // pelacak Find Hub aktif
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
    static const uint8_t* const F_TITLE[] = {u8g2_font_helvB10_tr, u8g2_font_helvB08_tr};
    fitFont("GL PRO NEOTECH", F_TITLE, 2, 124);   // jangan terpotong di tepi layar
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
  int tChg = 2 + (int)(124 * ((V_ENGINE_ON - VMIN) / (VMAX - VMIN)));
  u8g2.drawVLine(tLow, 43, 2);
  u8g2.drawVLine(tChg, 43, 2);

  // status
  const char* s;
  bool warn = false;
  // Mesin hidup + beban lampu: 11.0-12.4 V di idle itu normal di motor ini (bukan aki lemah)
  float lowTh = engineRunning ? 10.8f : 11.8f;
  if      (batteryVoltage < lowTh)  { s = "AKI LEMAH!";  warn = true; }
  else if (batteryVoltage > 14.9f) { s = "OVER CHARGE!"; warn = true; }
  else if (batteryVoltage >= V_ENGINE_ON) s = "CHARGING";
  else if (engineRunning)                 s = "BEBAN BESAR";
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
  // saat speed tampil di kanan, label mengecil agar tidak menempel pada angka 3 digit
  u8g2.setFont(speedActive ? u8g2_font_helvB10_tr : u8g2_font_helvB12_tr);
  u8g2.drawStr(2, 34, "Menunggu");
  u8g2.drawStr(2, 50, "rute...");

  if (speedActive) {                             // kecepatan tetap terlihat sebelum rute dimulai
    char sp[8];
    if (millis() - speedAt < SPEED_STALE_MS) snprintf(sp, sizeof(sp), "%d", speedKmh);
    else                                      strcpy(sp, "--");
    u8g2.setFont(u8g2_font_helvB18_tr);
    u8g2.drawStr(126 - u8g2.getStrWidth(sp), 38, sp);
    u8g2.setFont(u8g2_font_5x7_tr);
    u8g2.drawStr(126 - u8g2.getStrWidth("km/h"), 48, "km/h");
    u8g2.drawStr(2, 62, "Mulai rute di Maps");
  } else {
    u8g2.setFont(u8g2_font_5x7_tr);
    u8g2.drawStr(2, 62, "Mulai rute di Google Maps");
  }
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
  static const uint8_t* const F_DIST[]    = {u8g2_font_helvB18_tr, u8g2_font_helvB14_tr,
                                             u8g2_font_helvB12_tr};
  // saat speed tampil, jarak memakai font lebih kecil supaya descender koma/"m" tidak menyentuh angka speed
  static const uint8_t* const F_DIST_SP[] = {u8g2_font_helvB14_tr, u8g2_font_helvB12_tr};

  if (speedActive) {
    // ---- layout dengan kecepatan ----
    // jarak y=15 (font 14) | kecepatan besar y=46 (kanan: "km/h" lalu ETA, font kecil)
    fitFont(nav.distance, F_DIST_SP, 2, 80);
    int dw = u8g2.getStrWidth(nav.distance);
    u8g2.drawStr(126 - dw, 15, nav.distance);

    char sp[8];
    if (millis() - speedAt < SPEED_STALE_MS) snprintf(sp, sizeof(sp), "%d", speedKmh);
    else                                      strcpy(sp, "--");      // GPS hilang/basi
    u8g2.setFont(u8g2_font_helvB24_tr);
    int sw = u8g2.getStrWidth(sp);
    u8g2.drawStr(99 - sw, 46, sp);

    u8g2.setFont(u8g2_font_5x7_tr);
    u8g2.drawStr(126 - u8g2.getStrWidth("km/h"), 36, "km/h");
    if (nav.eta[0]) u8g2.drawStr(126 - u8g2.getStrWidth(nav.eta), 46, nav.eta);
  } else {
    // ---- layout tanpa kecepatan (seperti sebelumnya) ----
    fitFont(nav.distance, F_DIST, 3, 80);
    int dw = u8g2.getStrWidth(nav.distance);
    u8g2.drawStr(126 - dw, 26, nav.distance);
    if (nav.eta[0]) {
      char e[20];
      snprintf(e, sizeof(e), "ETA %s", nav.eta);
      u8g2.setFont(u8g2_font_helvB08_tr);
      int ew = u8g2.getStrWidth(e);
      u8g2.drawStr(126 - ew, 42, e);
    }
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

// ============ KECERAHAN ============
uint8_t pctToContrast(uint8_t p) {
  p = constrain(p, 5, 100);
  float c = powf(p / 100.0f, 1.6f) * 255.0f;      // kurva gamma: slider terasa merata di mata
  return (uint8_t)constrain((int)c, 6, 255);
}
void applyBrightness() {
  contrastVal = pctToContrast(brightPct);
  u8g2.setContrast(contrastVal);
}

// ============ FIND HUB (FMDN) ============
struct FindProfile { uint16_t everySec; uint16_t windowMs; };
static const FindProfile FIND_PROF[3] = {
  {60,  8000},    // 0 Ketat    : jendela 8 dtk tiap ~1 mnt
  {300, 10000},   // 1 Seimbang : jendela 10 dtk tiap ~5 mnt
  {900, 10000},   // 2 Hemat    : jendela 10 dtk tiap ~15 mnt
};

static int hexv(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}
// 40 karakter hex -> 20 byte. Tolak semua-nol / semua-FF (kunci placeholder).
bool parseHexKey(const char* s, uint8_t* out) {
  if (!s || strlen(s) != 40) return false;
  uint8_t orv = 0, andv = 0xFF;
  for (int i = 0; i < 20; i++) {
    int hi = hexv(s[2 * i]), lo = hexv(s[2 * i + 1]);
    if (hi < 0 || lo < 0) return false;
    out[i] = (uint8_t)((hi << 4) | lo);
    orv |= out[i];
    andv &= out[i];
  }
  return orv != 0 && andv != 0xFF;
}

// iklan untuk app (connectable, UUID layanan + nama di scan response)
void setAppAdv() {
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->stop();
  NimBLEAdvertisementData advData;
  advData.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
  advData.addServiceUUID(SVC_UUID);          // 128-bit UUID di paket utama (untuk filter scan HP)
  NimBLEAdvertisementData scanData;
  scanData.setName(DEVICE_NAME);             // nama di scan response
  adv->setConnectableMode(BLE_GAP_CONN_MODE_UND);
  adv->setDiscoverableMode(BLE_GAP_DISC_MODE_GEN);
  adv->enableScanResponse(true);
  adv->setMinInterval(48);                   // 30 ms
  adv->setMaxInterval(80);                   // 50 ms
  adv->setAdvertisementData(advData);
  adv->setScanResponseData(scanData);
  adv->start();
  advMode = ADV_APP;
}

// beacon Find Hub: non-connectable, service data 0xFEAA = [tipe][20 byte kunci][flags=0]
void setFindAdv() {
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->stop();
  uint8_t sd[22];
  sd[0] = FMDN_FRAME_TYPE;
  memcpy(sd + 1, fhnKey, 20);
  sd[21] = 0x00;
  NimBLEAdvertisementData d;
  d.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
  d.setServiceData(NimBLEUUID((uint16_t)0xFEAA), sd, sizeof(sd));
  adv->setConnectableMode(BLE_GAP_CONN_MODE_NON);
  adv->setDiscoverableMode(BLE_GAP_DISC_MODE_GEN);
  adv->enableScanResponse(false);
  adv->setMinInterval(64);                   // 40 ms
  adv->setMaxInterval(64);
  adv->setAdvertisementData(d);
  adv->start();
  advMode = ADV_FIND;
}

// Penjadwal iklan saat bangun: app terhubung -> tidak ada beacon (pemilik ada di dekat motor);
// tidak terhubung & pelacak aktif -> selang-seling 400 ms beacon / 1600 ms iklan app.
void advTick(unsigned long now) {
  if (bleConnected) { advMode = ADV_APP; return; }
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  bool find = fhnEnabled && fhnKeySet;
  if (!find) {
    if ((advMode != ADV_APP || !adv->isAdvertising()) && now - lastAdvKick >= 1000) {
      lastAdvKick = now;
      setAppAdv();
    }
    return;
  }
  if (advMode == ADV_APP && !adv->isAdvertising() && now - lastAdvKick >= 500) {
    lastAdvKick = now;                       // baru putus dari app: nyalakan lagi
    setAppAdv();
    advSwitchAt = now + APP_SLOT_MS;
    return;
  }
  if (now < advSwitchAt) return;
  if (advMode == ADV_FIND) { setAppAdv(); advSwitchAt = now + APP_SLOT_MS; }
  else                     { setFindAdv(); advSwitchAt = now + FIND_SLOT_MS; }
}

void wipeTracker() {
  fhnEnabled = false;
  fhnKeySet  = false;
  memset(fhnKey, 0, sizeof(fhnKey));
  pinStr[0] = 0;
  prefs.remove("fhn_key");
  prefs.remove("fhn_en");
  prefs.remove("pin");
}

// Pintu PIN untuk perintah pelacak. allowSet=true: jika PIN belum ada, 4-8 digit di "pin" menjadi PIN baru.
bool pinGate(JsonDocument& doc, bool allowSet) {
  const char* given = doc["pin"] | "";
  if (pinStr[0]) {
    if (!strcmp(given, pinStr)) return true;
    errCode = "pin";
    return false;
  }
  if (!allowSet) return true;
  size_t n = strlen(given);
  bool digits = n >= 4 && n <= 8;
  for (size_t i = 0; digits && i < n; i++) digits = given[i] >= '0' && given[i] <= '9';
  if (!digits) { errCode = "pinfmt"; return false; }
  strcpy(pinStr, given);
  prefs.putString("pin", pinStr);
  return true;
}

// ============ KEYLESS: RELAY & MASUKAN ============
void relaysInit() {
  const uint8_t p[] = {PIN_R_IGN, PIN_R_CDI, PIN_R_START, PIN_BUZZ, PIN_R_TURN_L, PIN_R_TURN_R};
  for (uint8_t pin : p) { pinMode(pin, OUTPUT); digitalWrite(pin, LOW); }
  pinMode(PIN_KEYSENSE, INPUT);
  pinMode(PIN_NEUTRAL, INPUT);
}
void rlyIgn(bool on) { ignRelayOn = on; digitalWrite(PIN_R_IGN, on ? HIGH : LOW); }
void rlyCdi(bool on) { cdiOn = on;      digitalWrite(PIN_R_CDI, on ? HIGH : LOW); }
void starterOff()    { digitalWrite(PIN_R_START, LOW); }
bool keyBusHigh()    { return digitalRead(PIN_KEYSENSE) == HIGH; }
// lampu N menarik kabel ke GND saat netral (0 V); di gigi lain kabel ~ +12 V lewat lampu
bool isNeutral() {
  uint32_t s = 0;
  for (int i = 0; i < 8; i++) s += analogReadMilliVolts(PIN_NEUTRAL);
  return (s / 8) < 800;
}
unsigned long sinceHigh(unsigned long now) { return lastHighMs ? (now - lastHighMs) : 0xFFFFFFFFUL; }
bool engineLikelyRunning(unsigned long now) { return engineRunning || sinceHigh(now) < 30000UL; }

// penanda "sedang dalam sesi kontak" disimpan agar reset mendadak (brown-out) saat jalan tidak mematikan motor
void saveRunFlag(bool on) { prefs.putBool("ign_run", on); }

void setIgnSession(bool on) {
  if (on) {
    rlyIgn(true);
    rlyCdi(true);
    ignOn = true;
    ignAt = millis();
    ignSawCharge = false;
  } else {
    rlyCdi(false);
    rlyIgn(false);
    ignOn = false;
  }
  saveRunFlag(ignOn);
  klForce = true;
}

// ============ KEYLESS: SEKUENS RITING / KLAKSON ============
struct FStep { uint16_t t; uint8_t m; };          // m bit0 = riting kiri+kanan, bit1 = buzzer (keadaan mutlak pada waktu t)
// "Cari motor": 3 kali kilat ganda ala lampu pesawat, buzzer 2x cepat dengan jeda
static const FStep SEQ_FIND[] = {
  {0, 3}, {120, 2}, {150, 0}, {220, 1}, {340, 0}, {600, 2}, {750, 0},
  {1100, 1}, {1220, 0}, {1320, 1}, {1440, 0},
  {2200, 1}, {2320, 0}, {2420, 1}, {2540, 0}, {2650, 0},
};
// "HP menjauh / link putus": 4 riting kedip ganda ala lampu strobo pesawat, 3 siklus (siklus pertama + 2 bip)
static const FStep SEQ_WARN[] = {
  {0, 3}, {90, 0}, {170, 3}, {260, 0},
  {1200, 1}, {1290, 0}, {1370, 1}, {1460, 0},
  {2400, 1}, {2490, 0}, {2570, 1}, {2660, 0}, {2750, 0},
};
// masuk fase terkunci: 2 siklus strobo pesawat + 1 bip pendek (konfirmasi kunci)
static const FStep SEQ_LOCK[] = {
  {0, 3}, {90, 0}, {170, 1}, {260, 0},
  {1200, 1}, {1290, 0}, {1370, 1}, {1460, 0}, {1550, 0},
};
static FStep seqDyn[40];                           // alarm / peringatan kill (dibangun saat dipakai)
enum SeqKind : uint8_t { SQ_NONE, SQ_FIND, SQ_WARN, SQ_ALARM, SQ_KILLWARN, SQ_LOCK };
SeqKind        seqKind = SQ_NONE;
const FStep*   seqTbl  = nullptr;
uint8_t        seqLen = 0, seqIdx = 0;
unsigned long  seqT0 = 0, seqPreUntil = 0, alarmCoolUntil = 0;
bool           seqTempIgn = false;

void seqOut(uint8_t m) {
  digitalWrite(PIN_R_TURN_L, (m & 1) ? HIGH : LOW);
  digitalWrite(PIN_R_TURN_R, (m & 1) ? HIGH : LOW);
  digitalWrite(PIN_BUZZ,     (m & 2) ? HIGH : LOW);
}
void seqStop() {
  seqOut(0);
  seqKind = SQ_NONE;
  seqPreUntil = 0;
  klForce = true;
}
bool seqStart(uint8_t k) {
  if (seqKind != SQ_NONE) return false;
  int n = 0;
  switch (k) {
    case SQ_FIND: seqTbl = SEQ_FIND; seqLen = sizeof(SEQ_FIND) / sizeof(FStep); break;
    case SQ_WARN: seqTbl = SEQ_WARN; seqLen = sizeof(SEQ_WARN) / sizeof(FStep); break;
    case SQ_LOCK: seqTbl = SEQ_LOCK; seqLen = sizeof(SEQ_LOCK) / sizeof(FStep); break;
    case SQ_ALARM:                                   // 10 dtk: riting menyala 0,5 dtk + buzzer 0,25 dtk, tiap 1 dtk
      for (int i = 0; i < 10; i++) {
        seqDyn[n++] = {(uint16_t)(i * 1000), 3};
        seqDyn[n++] = {(uint16_t)(i * 1000 + 250), 1};
        seqDyn[n++] = {(uint16_t)(i * 1000 + 500), 0};
      }
      seqTbl = seqDyn; seqLen = n; break;
    case SQ_KILLWARN:                                // 5 dtk riting kedip 2 Hz
      for (int i = 0; i < 10; i++) {
        seqDyn[n++] = {(uint16_t)(i * 500), 1};
        seqDyn[n++] = {(uint16_t)(i * 500 + 250), 0};
      }
      seqTbl = seqDyn; seqLen = n; break;
    default: return false;
  }
  seqKind = (SeqKind)k;
  seqIdx = 0;
  seqT0 = millis();
  klForce = true;
  return true;
}
void lockNow(const char* why);
void seqTick(unsigned long now) {
  if (seqKind == SQ_NONE) return;
  if (seqPreUntil) {
    if ((long)(now - seqPreUntil) < 0) return;
    seqPreUntil = 0;
    seqT0 = now;
  }
  unsigned long el = now - seqT0;
  while (seqIdx < seqLen && el >= seqTbl[seqIdx].t) seqOut(seqTbl[seqIdx++].m);
  if (seqIdx >= seqLen) {
    uint8_t k = seqKind;
    seqStop();
    if (k == SQ_ALARM)    alarmCoolUntil = now + KL_ALARM_COOL_MS;
    if (k == SQ_KILLWARN) lockNow("killed");
  }
}

// ============ KEYLESS: STARTER ============
enum StartPh : uint8_t { SP_NONE, SP_PREIGN, SP_CRANK, SP_COOL };
StartPh        spPh = SP_NONE;
unsigned long  spAt = 0, spCrankAt = 0, spFastAt = 0, startLockUntil = 0;
float          spMinV = 99, spV = 0;
uint8_t        spRecov = 0, startFails = 0;
bool           spOk = false;

void startAbort(const char* why) {
  starterOff();
  spPh = SP_NONE;
  if (why) errCode = why;
  klForce = true;
}

void lockNow(const char* why) {
  starterOff();
  spPh = SP_NONE;
  if (seqKind != SQ_NONE) seqStop();
  setIgnSession(false);
  if (why) { setNotice(why); seqStart(SQ_LOCK); }   // kedip strobo + bip: konfirmasi terkunci
  klForce = true;
}

void startTick(unsigned long now) {
  switch (spPh) {
    case SP_NONE: return;
    case SP_PREIGN: {
      if ((long)(now - spAt) < 0) return;
      float v = readVoltageRaw();
      if (v < KL_START_MIN_V)                { lockNow(nullptr); errCode = "lowbat"; return; }
      if (neutralReq && !isNeutral())        { startAbort("gear"); return; }
      digitalWrite(PIN_R_START, HIGH);
      spCrankAt = now; spFastAt = 0; spMinV = v; spV = v; spRecov = 0; spOk = false;
      spPh = SP_CRANK;
      setNotice("cranking");
      klForce = true;
      return;
    }
    case SP_CRANK: {
      if (!bleConnected) { startAbort("link"); spAt = now + KL_COOL_MS; spPh = SP_COOL; return; }   // link putus -> starter berhenti
      if (now - spFastAt >= 40) {
        spFastAt = now;
        float v = readVoltageRaw();
        spV = 0.5f * spV + 0.5f * v;
        if (spV < spMinV) spMinV = spV;
        if (spMinV < V_START_DIP && spV >= V_START_RECOVER) { if (spRecov < 255) spRecov++; }
        else spRecov = 0;
      }
      bool done    = spRecov >= 5;                         // pulih >= 200 ms = mesin menyala
      bool timeout = (now - spCrankAt) >= KL_START_MS;
      if (done || timeout) {
        starterOff();
        spOk = done;
        spAt = now + KL_COOL_MS;
        spPh = SP_COOL;
        setNotice(done ? "start_ok" : "start_end");
        klForce = true;
      }
      return;
    }
    case SP_COOL:
      if ((long)(now - spAt) < 0) return;
      spPh = SP_NONE;
      if (spOk || engineRunning) startFails = 0;
      else if (++startFails >= KL_FAIL_MAX) {
        startFails = 0;
        startLockUntil = now + KL_LOCKOUT_MS;
        setNotice("cool");
      }
      klForce = true;
      return;
  }
}

// ============ KEYLESS: PROXIMITY (RSSI link terhubung) ============
void rssiTick(unsigned long now) {
  if (!bleConnected || connHandle == 0xFFFF) { nearOk = false; rssiN = 0; return; }
  if (now - lastRssiAt < 300) return;
  lastRssiAt = now;
  int8_t r = 0;
  if (ble_gap_conn_rssi(connHandle, &r) != 0 || r >= 0 || r < -110) return;
  rssiEma = rssiN ? rssiEma * 0.7f + (float)r * 0.3f : (float)r;
  if (rssiN < 100) rssiN++;
  if (calibUntil) {                                      // kalibrasi: rata-rata RSSI selama 6 dtk
    calibSum += r; calibCnt++;
    if ((long)(now - calibUntil) >= 0) {
      calibUntil = 0;
      int avg = calibCnt ? (int)(calibSum / calibCnt) : -72;
      rssiThr = (int8_t)constrain(avg - 4, -95, -40);   // margin 4 dB
      prefs.putChar("rssi_thr", rssiThr);
      snprintf(noticeBuf, sizeof(noticeBuf), "calib:%d", (int)rssiThr);
      klForce = true;
    }
  }
  if (!nearOk) { if (rssiN >= 3 && rssiEma >= (float)rssiThr) nearOk = true; }
  else if (rssiEma < (float)rssiThr - KL_NEAR_HYST) nearOk = false;
}

// ============ KEYLESS: PAIRING (LE Secure Connections, passkey tampil di OLED) ============
void startPairing(unsigned long ms, bool wipeBonds) {
  if (wipeBonds) NimBLEDevice::deleteAllBonds();
  pairPin = 100000 + (esp_random() % 900000);
  NimBLEDevice::setSecurityPasskey(pairPin);
  pairUntil = millis() + ms;
  oledForceUntil = pairUntil;
  klForce = true;
}
void endPairing() {
  pairUntil = 0;
  NimBLEDevice::setSecurityPasskey(100000 + (esp_random() % 900000));
}

// ============ KEYLESS: OLED (mati total saat sesi diam: hemat daya) ============
bool oledInited = false, oledOn = false;
void oledEnsure() {
  if (!oledInited) {
    Wire.begin(PIN_SDA, PIN_SCL);
    Wire.setClock(400000);
    Wire.setTimeOut(50);
    u8g2.begin();
    u8g2.setBusClock(400000);
    oledInited = true;
    oledOn = false;
  }
  if (!oledOn) {
    u8g2.setPowerSave(0);
    applyBrightness();
    oledOn = true;
    lastDraw = 0;
    lastDispInit = millis();
  }
}
void oledOff() {
  if (oledInited && oledOn) { u8g2.setPowerSave(1); oledOn = false; }
}

// ============ KEYLESS: KILL / KUNCI ============
void killRequest(bool force, unsigned long now) {
  if (seqKind == SQ_KILLWARN) return;
  if (!ignOn && !cdiOn) { lockNow("locked"); return; }
  if (!engineLikelyRunning(now)) { lockNow("locked"); return; }
  bool fresh = speedActive && (now - speedAt) < SPEED_STALE_MS;
  if (fresh && speedKmh > KL_MOVING_KMH && !force) { errCode = "moving"; return; }
  if (seqKind != SQ_NONE) seqStop();
  seqStart(SQ_KILLWARN);
  setNotice("killing");
}

// ============ KEYLESS: TICK UTAMA (dipanggil tiap putaran loop) ============
void klTick(unsigned long now) {
  if (pairUntil && (long)(now - pairUntil) >= 0) endPairing();
  rssiTick(now);
  seqTick(now);
  startTick(now);
  if (ignOn && engineRunning) ignSawCharge = true;

  bool keyBus = keyBusHigh();

  if (klMode == 0) {
    // MANUAL: CDI mengikuti kunci asli (150 ms debounce). Bila ESP mati, pakai saklar bypass darurat.
    static unsigned long chg = 0;
    bool want = keyBus && !ignRelayOn;
    if (want != cdiOn) { if (!chg) chg = now; else if (now - chg >= 150) { rlyCdi(want); chg = 0; } }
    else chg = 0;
    return;
  }

  // ---- KEYLESS ----
  // 1) tamper: bus kontak hidup / mesin mengisi padahal bukan kita yang menyalakan
  static unsigned long tampSince = 0;
  bool intrusion = !ignRelayOn && !ignOn && keyBus;      // (tegangan tinggi saja bisa dari charger aki: bukan tanda pembobolan)
  if (intrusion && seqKind == SQ_NONE && spPh == SP_NONE) {
    if (!tampSince) tampSince = now;
    else if (now - tampSince >= 400 && (long)(now - alarmCoolUntil) >= 0) {
      tampSince = 0;
      if (tamperCount < 999) { tamperCount++; prefs.putUShort("tamper", tamperCount); }
      seqStart(SQ_ALARM);
      setNotice("tamper");
    }
  } else if (!intrusion) {
    tampSince = 0;
  }

  if (!ignOn) return;

  // 2) HP hilang saat kontak ON
  static unsigned long absentSince = 0;
  static bool warned = false;
  bool absent = !bleConnected || (rssiN >= 3 && !nearOk);
  if (!absent) { absentSince = 0; warned = false; }
  else {
    if (!absentSince) absentSince = now;
    unsigned long gone = now - absentSince;
    if (warnFar && !warned && gone >= KL_FAR_WARN_MS && seqKind == SQ_NONE) { warned = true; seqStart(SQ_WARN); }
    uint16_t lk = LOCK_PROF_S[lockProf & 3];
    // kunci hanya bila HP sudah lama hilang DAN tidak ada pengisian terlihat selama itu (mesin tidak sedang digas)
    if (lk && gone >= lk * 1000UL && sinceHigh(now) >= lk * 1000UL && seqKind == SQ_NONE && spPh == SP_NONE) {
      lockNow("locked_far");
      return;
    }
  }

  // 3) kontak ON tapi mesin tak pernah hidup -> mati sendiri (hemat aki)
  if (autoOff && !ignSawCharge && spPh == SP_NONE && seqKind == SQ_NONE && (now - ignAt) >= KL_AUTOOFF_MS)
    lockNow("autooff");
}

// ============ BLE INIT (dipakai normal & siklus parkir) ============
void bleInitFull() {
  NimBLEDevice::init(DEVICE_NAME);
  NimBLEDevice::setMTU(247);
  NimBLEDevice::setPower(FIND_TX_DBM);
#if KL_REQUIRE_PAIRING
  NimBLEDevice::setSecurityAuth(true, true, true);          // bonding + MITM + Secure Connections
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY);   // HP mengetik passkey yang tampil di OLED
  NimBLEDevice::setSecurityPasskey(pairingActive() ? pairPin : (100000 + (esp_random() % 900000)));
#endif
  pServer = NimBLEDevice::createServer();
  pServer->setCallbacks(new SrvCb());
  pServer->advertiseOnDisconnect(false);     // iklan diatur advTick()/parkedWake()

  NimBLEService* svc = pServer->createService(SVC_UUID);
  uint32_t rxProps = NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR;
#if KL_REQUIRE_PAIRING
  rxProps |= NIMBLE_PROPERTY::WRITE_AUTHEN;                 // tulis hanya dari HP yang sudah pairing (autentik)
#endif
  NimBLECharacteristic* pRx = svc->createCharacteristic(RX_UUID, rxProps);
  pRx->setCallbacks(new RxCb());
  pTxChar = svc->createCharacteristic(TX_UUID, NIMBLE_PROPERTY::NOTIFY);
  svc->start();
  bleReady = true;
}

// ============ KEYLESS: PERINTAH DARI APP ============
void handleKl(const char* cmd, JsonDocument& doc) {
#if !KL_REQUIRE_PAIRING
  errCode = "nopair";
  return;
#endif
  unsigned long now = millis();
  bool busy = (spPh != SP_NONE) || (seqKind != SQ_NONE && seqKind != SQ_ALARM && seqKind != SQ_WARN && seqKind != SQ_LOCK);

  if (!strcmp(cmd, "kl_mode")) {
    if (ignOn || engineLikelyRunning(now) || busy) { errCode = "busy"; return; }
    klMode = ((int)(doc["val"] | 0)) ? 1 : 0;
    prefs.putUChar("kl_mode", klMode);
    if (klMode == 1) rlyCdi(false);          // masuk keyless: CDI langsung putus
    setNotice(klMode ? "mode_kl" : "mode_man");
  }
  else if (!strcmp(cmd, "kl_ign")) {
    if ((int)(doc["en"] | 0) == 1) {
      if (klMode != 1)  { errCode = "mode"; return; }
      if (!nearOk)      { errCode = "far";   return; }
      if (busy)         { errCode = "busy";  return; }
      if (batteryVoltage < KL_START_MIN_V) { errCode = "lowbat"; return; }
      if (seqKind == SQ_ALARM || seqKind == SQ_WARN || seqKind == SQ_LOCK) seqStop();
      setIgnSession(true);
      setNotice("ign_on");
    } else {
      if (klMode != 1) { errCode = "mode"; return; }
      killRequest((int)(doc["force"] | 0) == 1, now);
    }
  }
  else if (!strcmp(cmd, "kl_start")) {
    if (klMode != 1)                       { errCode = "mode";    return; }
    if (!nearOk)                           { errCode = "far";     return; }
    if (busy)                              { errCode = "busy";    return; }
    if (engineLikelyRunning(now))          { errCode = "running"; return; }
    if ((long)(now - startLockUntil) < 0)  { errCode = "cool";    return; }
    if (batteryVoltage < KL_START_MIN_V)   { errCode = "lowbat";  return; }
    if (seqKind == SQ_ALARM || seqKind == SQ_WARN || seqKind == SQ_LOCK) seqStop();
    if (!ignOn) { setIgnSession(true); spAt = now + 450; }      // beri waktu bus kontak stabil
    else        { spAt = now + 50; }
    spPh = SP_PREIGN;
    setNotice("starting");
  }
  else if (!strcmp(cmd, "kl_stop")) {
    if (spPh == SP_CRANK || spPh == SP_PREIGN) startAbort(nullptr);
    spPh = SP_NONE;
  }
  else if (!strcmp(cmd, "kl_kill")) {
    if (klMode != 1) { errCode = "mode"; return; }
    killRequest((int)(doc["force"] | 0) == 1, now);
  }
  else if (!strcmp(cmd, "kl_cancel")) {
    if (seqKind == SQ_KILLWARN) { seqStop(); setNotice("cancel"); }
  }
  else if (!strcmp(cmd, "kl_find")) {
    if (ignOn && engineLikelyRunning(now)) { errCode = "busy"; return; }
    if (seqKind == SQ_ALARM || seqKind == SQ_WARN || seqKind == SQ_LOCK) seqStop();
    if (!seqStart(SQ_FIND)) { errCode = "busy"; return; }
  }
  else if (!strcmp(cmd, "kl_calib")) {
    if (!bleConnected) return;
    calibUntil = now + 6000; calibSum = 0; calibCnt = 0;
    setNotice("calibrating");
  }
  else if (!strcmp(cmd, "kl_cfg")) {
    if (doc["wp"].is<int>()) { wakeProf = (uint8_t)constrain((int)doc["wp"], 0, 2); prefs.putUChar("kl_wp", wakeProf); }
    if (doc["lk"].is<int>()) { lockProf = (uint8_t)constrain((int)doc["lk"], 0, 3); prefs.putUChar("kl_lk", lockProf); }
    if (doc["nr"].is<int>()) { neutralReq = (int)doc["nr"] != 0; prefs.putBool("kl_nr", neutralReq); }
    if (doc["ao"].is<int>()) { autoOff    = (int)doc["ao"] != 0; prefs.putBool("kl_ao", autoOff); }
    if (doc["wf"].is<int>()) { warnFar    = (int)doc["wf"] != 0; prefs.putBool("kl_wf", warnFar); }
  }
  else if (!strcmp(cmd, "kl_pair")) {
    startPairing(KL_PAIR_MS, false);          // tambah HP lain (tanpa menghapus yang lama)
    setNotice("pair_open");
  }
  else if (!strcmp(cmd, "kl_ack")) {
    tamperCount = 0;
    prefs.putUShort("tamper", 0);
  }
  klForce = true;
}

// ============ TIDUR / PARKIR ============
// Periode bangun: keyless = profil (2/4/8 dtk; 2 dtk selama "boost" setelah sesi), manual = 10 dtk.
uint8_t wakePeriod() {
  if (klMode == 0) return PARK_WAKE_S;
  uint8_t p = WAKE_PROF_S[wakeProf > 2 ? 1 : wakeProf];
  if (rtcBoost > 0) { p = 2; rtcBoost = rtcBoost > p ? rtcBoost - p : 0; }
  if (rtcLowBatt && p < 8) p = 8;               // aki lemah: lebih jarang
  return p;
}

void goSleep(uint32_t sec) {
  esp_sleep_enable_timer_wakeup((uint64_t)sec * 1000000ULL);
  rtc_gpio_pullup_en((gpio_num_t)PIN_BOOT);
  rtc_gpio_pulldown_dis((gpio_num_t)PIN_BOOT);
  esp_sleep_enable_ext0_wakeup((gpio_num_t)PIN_BOOT, 0);                        // tombol BOOT membangunkan
  esp_sleep_enable_ext1_wakeup(1ULL << PIN_KEYSENSE, ESP_EXT1_WAKEUP_ANY_HIGH); // bus kontak hidup (kunci diputar) membangunkan
  esp_deep_sleep_start();
  while (true) {}
}

// Dipanggil di awal setup() saat bangun dari deep sleep.
// Kembali (-> boot normal) hanya jika: tombol BOOT / kunci diputar / mesin mengisi / HP terhubung.
void parkedWake(esp_sleep_wakeup_cause_t cause) {
  analogSetAttenuation(ADC_11db);
  analogReadResolution(12);
  if (cause == ESP_SLEEP_WAKEUP_EXT0 || cause == ESP_SLEEP_WAKEUP_EXT1) return;   // BOOT / kunci kontak
  float v = readVoltageRaw();
  if (keyBusHigh() || (klMode == 0 && v >= V_WAKE_RUN)) return;                   // kontak / mesin hidup (keyless: charger aki tidak membangunkan)

  if (rtcLowBatt) { if (v >= V_RESUME) rtcLowBatt = 0; }
  else if (v < V_CUTOFF) rtcLowBatt = 1;

  uint8_t period = wakePeriod();
  rtcSecAcc += period;
  uint8_t prof = fhnProfile > 2 ? 1 : fhnProfile;
  bool beacon = !rtcLowBatt && fhnEnabled && fhnKeySet && rtcSecAcc >= FIND_PROF[prof].everySec;

  bleInitFull();
  if (beacon) {                                // jendela beacon Find Hub (non-connectable)
    rtcSecAcc = 0;
    setFindAdv();
    delay(FIND_PROF[prof].windowMs);
  }
  setAppAdv();                                 // jendela iklan terhubung: app dapat menyambung
  unsigned long t0 = millis();
  while (millis() - t0 < KL_ADV_WINDOW_MS && !bleConnected) delay(5);
  if (bleConnected) {                          // HP menyambung: lanjut boot, OLED tetap mati
    quietWake = true;
    return;
  }
  NimBLEDevice::getAdvertising()->stop();
  NimBLEDevice::deinit(true);
  goSleep(period);
}

// Masuk mode parkir dari mode normal: simpan jam mesin, putus app, matikan OLED & BLE, tidur.
void enterPark() {
  if (totalEngineSec != lastSavedSec) {
    prefs.putULong("engsec", totalEngineSec);
    lastSavedSec = totalEngineSec;
  }
  if (bleConnected && pTxChar && pServer) {    // beri tahu app: ESP tidur, jangan menyambung ulang terus-menerus
    pTxChar->setValue((const uint8_t*)"Z|1", 3);
    pTxChar->notify();
    delay(120);
    pServer->disconnect(connHandle);
    delay(250);
  }
  if (oledInited) u8g2.setPowerSave(1);        // SSD1306 display OFF (arus ~µA)
  NimBLEAdvertising* a = NimBLEDevice::getAdvertising();
  if (a) a->stop();
  NimBLEDevice::deinit(true);
  rtcSecAcc  = 100000;                         // jendela beacon pertama langsung di bangun berikutnya
  rtcBoost   = (klMode == 1) ? KL_BOOST_SEC : 0;
  rtcLowBatt = (batteryVoltage < V_CUTOFF) ? 1 : 0;
  uint8_t period = wakePeriod();
  prefs.end();
  setCpuFrequencyMhz(80);
  goSleep(period);
}

// ============ COMMAND HANDLER (dipanggil dari loop) ============
void handleCommand(const char* js) {
  JsonDocument doc;
  if (deserializeJson(doc, js) != DeserializationError::Ok) {
    Serial.println("[CMD] JSON invalid");
    return;
  }
  const char* cmd = doc["cmd"] | "";
  // perintah dari pengguna = aktivitas (menunda tidur); aliran otomatis speed/nav/ping tidak dihitung
  if (strcmp(cmd, "speed") && strcmp(cmd, "nav") && strcmp(cmd, "ping")) lastKeepAwake = millis();

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
  else if (!strcmp(cmd, "speed")) {
    int k = (int)(doc["kmh"] | -1);
    if (k < 0) {                                 // HP mematikan fitur kecepatan
      speedActive = false;
    } else {
      speedKmh    = constrain(k, 0, 299);
      speedAt     = millis();
      speedActive = true;
    }
    // tidak perlu force redraw: layar nav sudah refresh tiap 100 ms
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
  else if (!strcmp(cmd, "bright")) {
    brightPct = (uint8_t)constrain((int)(doc["val"] | 100), 5, 100);
    applyBrightness();
    if ((int)(doc["save"] | 0) == 1 && brightPct != savedBright) {   // simpan hanya saat slider dilepas
      prefs.putUChar("bright", brightPct);
      savedBright = brightPct;
    }
    forceStatus = true;
  }
  else if (!strcmp(cmd, "park")) {
    parkMin = (uint8_t)constrain((int)(doc["min"] | DEF_PARK_MIN), 1, 30);
    prefs.putUChar("parkmin", parkMin);
    forceStatus = true;
  }
  else if (!strcmp(cmd, "find_key")) {
    if (!pinGate(doc, true)) return;
    uint8_t k[20];
    if (!parseHexKey(doc["key"] | "", k)) { errCode = "key"; return; }
    memcpy(fhnKey, k, 20);
    fhnKeySet = true;
    prefs.putBytes("fhn_key", fhnKey, 20);
    forceStatus = true;
  }
  else if (!strcmp(cmd, "find")) {
    if (!pinGate(doc, false)) return;
    bool en = (int)(doc["en"] | 0) == 1;
    if (en && !fhnKeySet) { errCode = "nokey"; return; }
    fhnEnabled = en;
    if (doc["prof"].is<int>()) {
      fhnProfile = (uint8_t)constrain((int)doc["prof"], 0, 2);
      prefs.putUChar("fhn_prof", fhnProfile);
    }
    prefs.putBool("fhn_en", fhnEnabled);
    advSwitchAt = 0;                           // terapkan jadwal iklan baru segera
    forceStatus = true;
  }
  else if (!strcmp(cmd, "find_clear")) {
    if (!pinGate(doc, false)) return;
    wipeTracker();
    forceStatus = true;
  }
  else if (!strncmp(cmd, "kl_", 3)) {
    handleKl(cmd, doc);
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

// ============ TELEMETRI KE HP ============
// Frame (pemisah '|'):  E|kode galat   N|kode info   Z|1 ESP akan tidur
//   K|mode|ign|cdi|st|near|rssi|netral|mesin|ambang|wake|lock|nr|ao|hi|wf|tamper|pair|calib
//   S|kecerahan|find|profil|kunci|tidur|pin     I|nama|baris2     volt,mesin,detik (CSV)
void notifyTelemetry(unsigned long now) {
  if (!bleConnected || !pTxChar) return;
  bool fast = klForce || spPh != SP_NONE || seqKind == SQ_KILLWARN;
  if (now - lastNotify < (fast ? 300UL : 1000UL)) return;
  lastNotify = now;
  char buf[112];
  int n;
  frameN++;
  if (errCode) {                                 // galat perintah
    n = snprintf(buf, sizeof(buf), "E|%s", errCode);
    errCode = nullptr;
  } else if (noticeBuf[0]) {                     // info kejadian (start_ok, locked, ...)
    n = snprintf(buf, sizeof(buf), "N|%s", noticeBuf);
    noticeBuf[0] = 0;
  } else if (forceInfo) {
    forceInfo = false;
    n = snprintf(buf, sizeof(buf), "I|%s|%s", idleName, idleLine2);
  } else if (klForce || (frameN % 3) == 1) {     // status keyless
    klForce = false;
    int st = 0;
    if (spPh == SP_PREIGN || spPh == SP_CRANK) st = 2;
    else if (seqKind == SQ_KILLWARN)           st = 4;
    else if (seqKind == SQ_FIND || seqKind == SQ_ALARM) st = 5;
    else if (ignOn && engineRunning)           st = 3;
    else if (ignOn)                            st = 1;
    int neu = (klMode == 1 && ignRelayOn) ? (isNeutral() ? 1 : 0) : 2;   // 2 = tidak diketahui (kontak mati)
    n = snprintf(buf, sizeof(buf), "K|%u|%d|%d|%d|%d|%d|%d|%d|%d|%u|%u|%d|%d|%d|%d|%u|%d|%d",
                 klMode, ignOn ? 1 : 0, cdiOn ? 1 : 0, st, nearOk ? 1 : 0,
                 rssiN ? (int)rssiEma : 0, neu, engineRunning ? 1 : 0, (int)rssiThr,
                 wakeProf, lockProf, neutralReq ? 1 : 0, autoOff ? 1 : 0, hornIgn ? 1 : 0, warnFar ? 1 : 0,
                 tamperCount, pairingActive() ? 1 : 0, calibUntil ? 1 : 0);
  } else if (forceStatus || (frameN % 15) == 2) {   // status pengaturan
    forceStatus = false;
    n = snprintf(buf, sizeof(buf), "S|%u|%d|%u|%d|%u|%d", brightPct, fhnEnabled ? 1 : 0,
                 fhnProfile, fhnKeySet ? 1 : 0, parkMin, pinStr[0] ? 1 : 0);
  } else if ((frameN % 15) == 8) {               // teks idle
    n = snprintf(buf, sizeof(buf), "I|%s|%s", idleName, idleLine2);
  } else {                                       // telemetri CSV
    n = snprintf(buf, sizeof(buf), "%.2f,%d,%lu",
                 batteryVoltage, engineRunning ? 1 : 0, totalEngineSec);
  }
  pTxChar->setValue((const uint8_t*)buf, n);
  pTxChar->notify();
}

// ============ LAYAR KHUSUS ============
void drawPairing(unsigned long now) {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_helvB10_tr);
  drawCentered("PAIRING", 14);
  char pin[12];
  snprintf(pin, sizeof(pin), "%06lu", (unsigned long)pairPin);
  u8g2.setFont(u8g2_font_helvB18_tr);
  drawCentered(pin, 44);
  u8g2.setFont(u8g2_font_5x7_tr);
  char t[32];
  long left = (long)(pairUntil - now) / 1000;
  snprintf(t, sizeof(t), "Ketik PIN ini di HP  (%lds)", left < 0 ? 0L : left);
  drawCentered(t, 60);
  u8g2.sendBuffer();
}

void drawAlarm(unsigned long now) {
  u8g2.clearBuffer();
  bool inv = (now / 250) % 2;
  if (inv) u8g2.drawBox(0, 0, 128, 64);
  u8g2.setDrawColor(inv ? 0 : 1);
  u8g2.setFont(u8g2_font_helvB18_tr);
  drawCentered("ALARM!", 34);
  u8g2.setFont(u8g2_font_5x7_tr);
  drawCentered("KUNCI / MESIN TANPA IZIN", 54);
  u8g2.setDrawColor(1);
  u8g2.sendBuffer();
}

// ============ SETUP ============
void loadSettings() {
  totalEngineSec = prefs.getULong("engsec", 0);
  lastSavedSec   = totalEngineSec;
  brightPct   = (uint8_t)constrain((int)prefs.getUChar("bright", 100), 5, 100);
  savedBright = brightPct;
  parkMin     = (uint8_t)constrain((int)prefs.getUChar("parkmin", DEF_PARK_MIN), 1, 30);
  fhnEnabled  = prefs.getBool("fhn_en", false);
  fhnProfile  = prefs.getUChar("fhn_prof", 1);
  if (fhnProfile > 2) fhnProfile = 1;
  fhnKeySet   = prefs.getBytes("fhn_key", fhnKey, 20) == 20;
  if (!fhnKeySet) fhnEnabled = false;
  {
    String pn = prefs.getString("pin", "");
    strncpy(pinStr, pn.c_str(), sizeof(pinStr) - 1);
    pinStr[sizeof(pinStr) - 1] = 0;
  }
  klMode     = prefs.getUChar("kl_mode", 0) ? 1 : 0;
  wakeProf   = (uint8_t)constrain((int)prefs.getUChar("kl_wp", 1), 0, 2);
  lockProf   = (uint8_t)constrain((int)prefs.getUChar("kl_lk", 1), 0, 3);
  neutralReq = prefs.getBool("kl_nr", true);
  autoOff    = prefs.getBool("kl_ao", true);
  warnFar    = prefs.getBool("kl_wf", true);
  rssiThr    = prefs.getChar("rssi_thr", KL_NEAR_DEFAULT);
  tamperCount = prefs.getUShort("tamper", 0);
}

void setup() {
  // 1) PALING AWAL: semua relay mati, lalu pulihkan relay yang harus tetap hidup setelah reset mendadak
  relaysInit();
  esp_sleep_wakeup_cause_t wc = esp_sleep_get_wakeup_cause();
  if (wc != ESP_SLEEP_WAKEUP_UNDEFINED) setCpuFrequencyMhz(80);   // siklus parkir: hemat arus
  analogSetAttenuation(ADC_11db);
  analogReadResolution(12);
  pinMode(PIN_BOOT, INPUT_PULLUP);

  prefs.begin("glpro", false);
  loadSettings();

  esp_reset_reason_t rr = esp_reset_reason();
  bool warmReset = (rr == ESP_RST_BROWNOUT || rr == ESP_RST_PANIC || rr == ESP_RST_INT_WDT ||
                    rr == ESP_RST_TASK_WDT || rr == ESP_RST_WDT || rr == ESP_RST_SW);
  if (klMode == 1 && warmReset && prefs.getBool("ign_run", false)) {
    // ESP ter-reset saat kontak ON (sedang jalan): kembalikan kontak + CDI segera, jangan matikan motor di jalan
    rlyIgn(true); rlyCdi(true);
    ignOn = true; ignAt = millis(); ignSawCharge = true;
  } else {
    prefs.putBool("ign_run", false);
    if (klMode == 0 && keyBusHigh()) rlyCdi(true);   // manual: kunci sudah ON -> CDI hidup tanpa menunggu boot penuh
  }

  // 2) bangun dari deep sleep: siklus parkir. Kembali hanya bila ada alasan boot normal.
  if (wc != ESP_SLEEP_WAKEUP_UNDEFINED) parkedWake(wc);

  if (!quietWake) { Serial.begin(115200); delay(100); }
  setCpuFrequencyMhz(quietWake ? 80 : 160);
  enableLoopWDT();                                   // loop macet -> reset -> relay (termasuk starter) lepas

  memset(&nav, 0, sizeof(nav));

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
  if (quietWake) {
    bootStart = millis() - BOOT_MS - 1;              // tanpa splash, OLED tidak dinyalakan
  } else {
    oledEnsure();
    drawBoot(0);                                     // logo langsung tampil
  }

  cmdQueue = xQueueCreate(6, sizeof(Msg));

  if (!bleReady) bleInitFull();
  if (wc == ESP_SLEEP_WAKEUP_UNDEFINED && NimBLEDevice::getNumBonds() == 0)
    startPairing(180000UL, false);                   // belum ada HP terpasang: buka pairing 3 menit

  if (!bleConnected) { setAppAdv(); advSwitchAt = millis() + APP_SLOT_MS; }

  lastDispInit = millis();
  lastKeepAwake = millis();
  if (!quietWake) Serial.println("[BOOT] ready");
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
  klTick(now);
  processQueue();
  if (!bleConnected) speedActive = false;        // HP putus -> kembali ke layout tanpa speed
  notifyTelemetry(now);

  // iklan: selalu bisa ditemukan HP saat tidak terhubung + jadwal beacon Find Hub
  advTick(now);

  // tombol BOOT: tahan 1 dtk = nyalakan layar 8 dtk; 3 dtk = mode pairing (hapus semua HP lama);
  // 10 dtk = hapus kunci + PIN pelacak
  static unsigned long btnSince = 0;
  static bool btnPair = false, btnWiped = false;
  if (digitalRead(PIN_BOOT) == LOW) {
    lastKeepAwake = now;
    if (!btnSince) btnSince = now;
    unsigned long held = now - btnSince;
    if (held >= 1000) oledForceUntil = max(oledForceUntil, now + 8000);
    if (!btnPair && held >= 3000) {
      btnPair = true;
      startPairing(KL_PAIR_MS, true);
    }
    if (!btnWiped && held >= 10000) {
      btnWiped = true;
      wipeTracker();
      forceStatus = true;
      oledEnsure();
      u8g2.clearBuffer();
      u8g2.setFont(u8g2_font_helvB10_tr);
      drawCentered("TRACKER RESET", 36);
      u8g2.sendBuffer();
      delay(1500);
    }
  } else {
    btnSince = 0;
    btnPair = false;
    btnWiped = false;
  }

  // parkir: tak ada alasan tetap bangun + tak ada aktivitas -> tidur dalam
  bool boot0 = (now - bootStart) < BOOT_MS;
  bool keyBus = keyBusHigh();
  bool busy = engineRunning || (klMode == 0 && batteryVoltage >= V_WAKE_RUN) || keyBus || ignOn || ignRelayOn ||
              spPh != SP_NONE || seqKind != SQ_NONE || pairingActive() || calibUntil;
  if (busy) lastKeepAwake = now;
  unsigned long idleLimit = (klMode == 1) ? (bleConnected ? KL_SESSION_IDLE_MS : KL_NO_SESSION_MS)
                                          : (unsigned long)parkMin * 60000UL;
  if (!boot0 && now - lastKeepAwake >= idleLimit) enterPark();

  // OLED: keyless + kontak mati + sesi diam = padam total (hemat daya)
  bool booting = boot0 && !quietWake;
  bool wantOled = booting || klMode == 0 || ignOn || pairingActive() || seqKind == SQ_ALARM ||
                  (long)(oledForceUntil - now) > 0;
  if (wantOled) oledEnsure(); else oledOff();

  // re-init OLED tiap 30 dtk (pulih dari glitch tegangan/EMI)
  if (oledOn && now - lastDispInit >= 30000) {
    lastDispInit = now;
    u8g2.initDisplay();
    u8g2.setPowerSave(0);
    u8g2.setContrast(contrastVal);
    lastDraw = 0;
  }

  // render
  if (oledOn) {
    unsigned long interval = booting ? 40 : (idleDisplay ? 60 : (displayMode == 2 ? 100 : 250));
    if (pairingActive() || seqKind == SQ_ALARM) interval = 100;
    if (now - lastDraw >= interval) {
      lastDraw = now;
      if (booting) {
        drawBoot(now - bootStart);
      } else if (seqKind == SQ_ALARM) {
        drawAlarm(now);
      } else if (pairingActive()) {
        drawPairing(now);
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
  }

  delay(2);
}
