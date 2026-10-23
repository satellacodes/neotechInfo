/*
 * display.cpp — semua rendering OLED.
 * Layout fokus NAV: ikon panah kiri, jarak kanan-atas, speed besar kanan-bawah,
 * nama jalan marquee bawah. Tanpa layar voltase / jam mesin / sapaan idle.
 */
#include "app.h"
#include <math.h>

// ============ UTIL ============
void copyAscii(char* dst, size_t cap, const char* src) {
  size_t i = 0;
  for (; src && src[i] && i < cap - 1; i++) {
    unsigned char ch = (unsigned char)src[i];
    dst[i] = (ch >= 32 && ch < 127) ? (char)ch : '?';
  }
  dst[i] = 0;
  for (char* p = dst; *p; p++) if (*p == '|') *p = '/';
  // trim
  size_t n = strlen(dst);
  while (n && dst[n - 1] == ' ') dst[--n] = 0;
  char* s = dst;
  while (*s == ' ') s++;
  if (s != dst) memmove(dst, s, strlen(s) + 1);
}

void fitFont(const char* s, const uint8_t* const* fonts, int n, int maxW) {
  for (int i = 0; i < n; i++) {
    u8g2.setFont(fonts[i]);
    if (u8g2.getStrWidth(s) <= maxW) return;
  }
}

void drawCentered(const char* s, int y) {
  int w = u8g2.getStrWidth(s);
  u8g2.drawStr((128 - w) / 2, y, s);
}

void drawBtBadge() {
  if (bleConnected) {
    u8g2.setFont(u8g2_font_5x7_tr);
    u8g2.drawFrame(108, 2, 19, 11);
    u8g2.drawStr(112, 10, "BT");
  }
}

// ============ BOOT ============
void drawBoot() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_helvB14_tr);
  drawCentered("GL PRO", 28);
  u8g2.setFont(u8g2_font_helvB10_tr);
  drawCentered("NEOTECH", 44);
  u8g2.setFont(u8g2_font_5x7_tr);
  drawCentered("NAV + SPEED", 58);
  u8g2.sendBuffer();
}

// ============ ARROW ============
static void thickLine(int x0, int y0, int x1, int y1) {
  bool horiz = abs(x1 - x0) >= abs(y1 - y0);
  for (int o = -1; o <= 1; o++) {
    if (horiz) u8g2.drawLine(x0, y0 + o, x1, y1 + o);
    else       u8g2.drawLine(x0 + o, y0, x1 + o, y1);
  }
}

static void arrowHead(int tx, int ty, float dx, float dy) {
  const float L = 13.0f, W = 11.0f;
  float bx = tx - dx * L, by = ty - dy * L;
  float px = -dy, py = dx;
  u8g2.drawTriangle(tx, ty,
                    (int)(bx + px * W), (int)(by + py * W),
                    (int)(bx - px * W), (int)(by - py * W));
}

static void drawArrowIcon(const char* d) {
  const float S = 0.7071f;
  if      (!strcmp(d, "left"))        { thickLine(28,46,28,24); thickLine(28,24,16,24); arrowHead(3,24,-1,0); }
  else if (!strcmp(d, "right"))       { thickLine(16,46,16,24); thickLine(16,24,28,24); arrowHead(41,24,1,0); }
  else if (!strcmp(d, "slight-left")) { thickLine(28,46,28,28); thickLine(28,28,17,16); arrowHead(8,7,-S,-S); }
  else if (!strcmp(d, "slight-right")){ thickLine(16,46,16,28); thickLine(16,28,27,16); arrowHead(36,7,S,-S); }
  else if (!strcmp(d, "sharp-left"))  { thickLine(30,46,30,16); thickLine(30,16,16,30); arrowHead(8,38,-S,S); }
  else if (!strcmp(d, "sharp-right")) { thickLine(14,46,14,16); thickLine(14,16,28,30); arrowHead(36,38,S,S); }
  else if (!strncmp(d, "uturn", 5))   { thickLine(14,46,14,14); thickLine(14,14,30,14);
                                        thickLine(30,14,30,30); arrowHead(30,43,0,1); }
  else if (!strncmp(d, "roundabout",10)) {
    u8g2.drawCircle(22,30,10,U8G2_DRAW_ALL);
    u8g2.drawCircle(22,30,9,U8G2_DRAW_ALL);
    thickLine(22,46,22,40);
    thickLine(29,23,32,16); arrowHead(40,5,S,-S);
  }
  else if (!strcmp(d, "arrive")) {
    u8g2.drawDisc(22,18,10);
    u8g2.drawTriangle(13,23,31,23,22,44);
    u8g2.setDrawColor(0); u8g2.drawDisc(22,18,4); u8g2.setDrawColor(1);
  }
  else { thickLine(22,46,22,18); arrowHead(22,3,0,-1); }  // straight/default
}

// ============ NAV WAITING ============
static void drawNavWaiting() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_helvB08_tr);
  u8g2.drawStr(2, 10, "NAVIGASI");
  drawBtBadge();

  u8g2.setFont(speedActive ? u8g2_font_helvB10_tr : u8g2_font_helvB12_tr);
  u8g2.drawStr(2, 34, "Menunggu");
  u8g2.drawStr(2, 50, "rute...");

  if (speedActive) {
    char sp[8];
    strcpy(sp, speedStale ? "--" : "");
    if (!speedStale) snprintf(sp, sizeof(sp), "%d", speedKmh);
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

// ============ NAV UTAMA ============
void drawNavScreen(unsigned long now) {
  bool empty = (nav.arrow[0] == 0 && nav.distance[0] == 0 && nav.street[0] == 0);
  bool stale = nav.autoSrc && nav.lastUpdate != 0 && navStale;
  if (empty || stale) { drawNavWaiting(); return; }

  u8g2.clearBuffer();
  drawArrowIcon(nav.arrow);

  static const uint8_t* const F_DIST[]    = {u8g2_font_helvB18_tr, u8g2_font_helvB14_tr,
                                             u8g2_font_helvB12_tr};
  static const uint8_t* const F_DIST_SP[] = {u8g2_font_helvB14_tr, u8g2_font_helvB12_tr};

  if (speedActive) {
    // Layout: jarak y=15 (14px), speed besar y=46, "km/h" + ETA kecil kanan
    fitFont(nav.distance, F_DIST_SP, 2, 80);
    int dw = u8g2.getStrWidth(nav.distance);
    u8g2.drawStr(126 - dw, 15, nav.distance);

    char sp[8];
    strcpy(sp, speedStale ? "--" : "");
    if (!speedStale) snprintf(sp, sizeof(sp), "%d", speedKmh);
    u8g2.setFont(u8g2_font_helvB24_tr);
    int sw = u8g2.getStrWidth(sp);
    u8g2.drawStr(99 - sw, 46, sp);

    u8g2.setFont(u8g2_font_5x7_tr);
    u8g2.drawStr(126 - u8g2.getStrWidth("km/h"), 36, "km/h");
    if (nav.eta[0]) u8g2.drawStr(126 - u8g2.getStrWidth(nav.eta), 46, nav.eta);
  } else {
    fitFont(nav.distance, F_DIST, 3, 80);
    int dw = u8g2.getStrWidth(nav.distance);
    u8g2.drawStr(126 - dw, 26, nav.distance);
    if (nav.eta[0]) {
      char e[20];
      snprintf(e, sizeof(e), "ETA %s", nav.eta);
      u8g2.setFont(u8g2_font_helvB08_tr);
      u8g2.drawStr(126 - u8g2.getStrWidth(e), 42, e);
    }
  }

  if (!bleConnected) {
    u8g2.setFont(u8g2_font_5x7_tr);
    u8g2.drawStr(100, 8, "BT!");
  }

  // Nama jalan (marquee jika panjang)
  u8g2.drawHLine(0, 48, 128);
  u8g2.setFont(u8g2_font_helvB08_tr);
  int sw = u8g2.getStrWidth(nav.street);
  u8g2.setClipWindow(0, 50, 128, 64);
  if (sw <= 124) {
    u8g2.drawStr(2, 62, nav.street);
  } else {
    int cycle = sw + 40;
    int pos   = (int)((now / MARQUEE_INTERVAL_MS) % cycle);
    u8g2.drawStr(2 - pos, 62, nav.street);
    u8g2.drawStr(2 - pos + cycle, 62, nav.street);
  }
  u8g2.setMaxClipWindow();
  u8g2.sendBuffer();
}
