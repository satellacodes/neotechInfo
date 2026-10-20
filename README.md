<div align="center">

  <img src="assets/ProxyGo.png" alt="laravelProd Banner" width="100%"/>

  <br/>
  <br/>

  <p align="center">
    <a href="https://github.com/satellacodes/neotechInfo"><img src="https://img.shields.io/static/v1?label=satellacodes&message=satellacodes&color=00bcd4&logo=github"/></a>
    <a href="https://github.com/satellacodes/neotechInfo"><img src="https://img.shields.io/github/stars/satellacodes/neotechInfo?style=social"/></a>
    <a href="https://github.com/satellacodes/neotechInfo"><img src="https://img.shields.io/github/forks/satellacodes/neotechInfo?style=social"/></a>
    <img src="https://img.shields.io/github/downloads/satellacodes/neotechInfo/total?color=00bcd4&label=downloads&logo=github"/>
    <img src="https://img.shields.io/badge/license-MIT-00bcd4"/>
  </p>

  <p align="center">
    <a href="https://www.instagram.com/satellacodes/"><img src="https://img.shields.io/badge/Instagram-00bcd4?logo=instagram&logoColor=black"/></a>
    <a href="https://www.youtube.com/@satella.i"><img src="https://img.shields.io/badge/YouTube-00bcd4?logo=youtube&logoColor=black"/></a>
    <a href="https://dimasaris.xyz/"><img src="https://img.shields.io/badge/Website-00bcd4?logo=googlechrome&logoColor=black"/></a>
  </p>

</div>

# Neotech Info Display

Display OLED mini untuk **Honda GL Pro Neotech**: tegangan aki realtime, jam mesin hidup, dan navigasi belok-demi-belok dari Google Maps. ESP32 menerima data dari HP lewat **BLE**; HP dikontrol lewat app React Native (Expo).

```
Google Maps ──notifikasi──▶ App (Android) ──BLE──▶ ESP32 ──I²C──▶ OLED 0.96"
                                ▲                     │
                                └──── telemetri ◀─────┘  (tegangan, jam mesin)
```

## Fitur

- **3 mode layar**: Tegangan aki (gauge + status), Waktu mesin (jam:menit, tersimpan di flash), Navigasi (panah, jarak, ETA, nama jalan berjalan).
- **Navigasi otomatis**: app membaca notifikasi Google Maps (ID/EN) dan mengirim belokan ke display.
- **Boot logo** saat pertama dialiri daya, lalu layar idle **"Hai, \<nama\>"**. Nama dan teks kedua bisa diubah dari app dan disimpan di ESP32.
- **Stabil di motor**: debounce deteksi mesin, auto-reconnect BLE, advertising watchdog, OLED di-init ulang berkala.

## Struktur repo

| Folder               | Isi                                               |
| -------------------- | ------------------------------------------------- |
| `fimware/`           | Firmware ESP32 (`main.cpp`, `platformio.ini`)     |
| `neotechApp/`        | App Expo / React Native (Android)                 |
| `.github/workflows/` | CI: build APK + firmware, rilis otomatis saat tag |

## Hardware

| Komponen                                                   | Catatan                                                 |
| ---------------------------------------------------------- | ------------------------------------------------------- |
| ESP32 DevKit (30 pin)                                      | BLE bawaan                                              |
| OLED SSD1306 0.96" I²C 128×64                              | SDA=GPIO21, SCL=GPIO22 (SH1106? ganti konstruktor U8g2) |
| Buck converter 12V→5V ≥2A                                  | input ≥40V lebih aman untuk lonjakan motor              |
| Resistor 10k + 2.2k (1%), 1k, kapasitor 100nF              | pembagi tegangan + filter ADC                           |
| TVS SMBJ16A, dioda Schottky seri, fuse 1A, kapasitor 470µF | proteksi                                                |

**Wiring**

- `+12V` (setelah kunci kontak) → fuse → dioda → buck IN+ ; TVS paralel +12V–GND
- Buck 5V → pin `VIN` ESP32 ; semua GND disatukan
- Pembagi aki: `+12V (sebelum dioda)` → 10k → _titik tengah_ → 2.2k → GND ; _titik tengah_ → 1k → `GPIO34`, 100nF dari titik tengah ke GND
- OLED: 3V3, GND, SDA 21, SCL 22

**Kalibrasi**: bandingkan angka display dengan multimeter, lalu set `VBAT_CAL = V_multimeter / V_display` di `main.cpp`. Atur `V_ENGINE_ON` / `V_ENGINE_OFF` sesuai tegangan charging motor kamu.

## Firmware (Arduino IDE)

- Board package: **esp32 by Espressif** (3.x), board _ESP32 Dev Module_
- Library (Library Manager):

| Library                       | Versi  |
| ----------------------------- | ------ |
| U8g2 (olikraus)               | ≥ 2.36 |
| NimBLE-Arduino (h2zero)       | 2.x    |
| ArduinoJson (Benoit Blanchon) | 7.x    |

Buka `fimware/main.cpp` (di Arduino IDE taruh sebagai `fimware/fimware.ino`), pilih port, Upload. Alternatif: `pio run -d fimware -t upload`.

## App

Syarat: Node 22, pnpm, Android SDK, **dev build** (bukan Expo Go).

```bash
cd neotechApp
pnpm install
pnpm exec expo run:android --device     # debug, langsung ke HP
```

Pertama kali: buka app → **Beri akses notifikasi** → aktifkan _neotechApp_. Lalu set Baterai app ke **Tidak dibatasi**.

### Download APK (tanpa build lokal)

Lihat tab **Releases** (APK arm64, ditandatangani dengan debug keystore standar Expo, jadi bisa di-update tanpa uninstall). CI membangun otomatis; rilis baru dibuat dengan:

```bash
git tag v1.1.0 && git push origin v1.1.0
```

Build non-tag tersedia sebagai _artifact_ di tab **Actions**. Pastikan `pnpm-lock.yaml` ikut di-commit (CI memakai `--frozen-lockfile`).

## Protokol BLE

Nama perangkat `GLPro-Neotech` · Service `6E400001-B5A3-F393-E0A9-E50E24DCCA9E`

| Char | UUID akhir | Arah     | Isi                                                     |
| ---- | ---------- | -------- | ------------------------------------------------------- |
| RX   | `…0002…`   | HP → ESP | JSON, write                                             |
| TX   | `…0003…`   | ESP → HP | notify ~1 Hz: `volt,mesin,detik` atau `I\|nama\|baris2` |

| `cmd`                              | Field                                    |
| ---------------------------------- | ---------------------------------------- |
| `mode`                             | `val` 0=tegangan, 1=waktu, 2=nav         |
| `nav`                              | `arrow, distance, street, eta, go, auto` |
| `idle`                             | – (tampilkan layar idle)                 |
| `idle_text`                        | `name` (≤16), `line2` (≤24) — ASCII saja |
| `reset_time` · `contrast` · `ping` | – · `val` 0–255 · –                      |

`arrow`: `straight left right slight-left slight-right sharp-left sharp-right uturn roundabout arrive`

## Diuji pada

- **HP**: Google Pixel 3 XL · **Android 12** · Google Maps bahasa Indonesia
- **Mikrokontroler**: ESP32 DevKit, OLED SSD1306 0.96"
- Format notifikasi Google Maps bisa berbeda antar versi/bahasa. Buka _Lihat debug notifikasi_ di app untuk melihat teks mentah, lalu sesuaikan `neotechApp/src/nav/parser.ts` bila perlu.

## Troubleshooting

| Gejala                                           | Solusi                                                                                                                              |
| ------------------------------------------------ | ----------------------------------------------------------------------------------------------------------------------------------- |
| Navigasi otomatis mati setelah install ulang APK | Matikan lalu nyalakan lagi _Akses notifikasi_ untuk app (atau restart HP). Android sering melepas listener setelah `adb install -r` |
| Navigasi berhenti saat layar mati                | Baterai app → _Tidak dibatasi_                                                                                                      |
| Tidak menemukan ESP32                            | Bluetooth aktif; Android 12+: izin _Perangkat di sekitar_; Android ≤11: izin Lokasi + Lokasi aktif                                  |
| Angka tegangan meleset                           | Atur `VBAT_CAL`                                                                                                                     |

## Lisensi

Lihat [LICENSE](LICENSE).

---

> [!NOTE]
> made by satella and claude chan
