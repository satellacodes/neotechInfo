/**
 * BLE link ke ESP32 (singleton).
 *
 * Kenapa singleton di level modul?
 *  - BleManager TIDAK boleh di-destroy() di cleanup useEffect: React StrictMode (dev)
 *    menjalankan mount -> cleanup -> mount, sehingga manager yang sudah di-destroy
 *    dipakai lagi dan semua scan/connect gagal diam-diam. Itu penyebab utama "tidak bisa konek".
 *  - Modul yang sama dipakai oleh UI dan oleh headless task (listener notifikasi Google Maps).
 */
import { Buffer } from "buffer";
import { useSyncExternalStore } from "react";
import { PermissionsAndroid, Platform } from "react-native";
import {
  BleManager,
  Device,
  ScanMode,
  State,
  Subscription,
} from "react-native-ble-plx";

// lowercase supaya cocok dengan filter scan Android
export const SVC = "6e400001-b5a3-f393-e0a9-e50e24dcca9e";
export const RX = "6e400002-b5a3-f393-e0a9-e50e24dcca9e";
export const TX = "6e400003-b5a3-f393-e0a9-e50e24dcca9e";
export const DEVICE_NAME = "GLPro-Neotech";

const SCAN_TIMEOUT_MS = 12000;

export type LinkState = "off" | "scanning" | "connecting" | "connected";

/** status keyless dari ESP32 (frame "K|...") */
export interface KlState {
  mode: number;      // 0 manual (kunci asli), 1 keyless
  ign: boolean;      // kontak keyless ON
  cdi: boolean;      // relay CDI tertutup
  st: number;        // 0 terkunci, 1 kontak ON, 2 starter, 3 mesin hidup, 4 hitung mundur kill, 5 sekuens (cari/alarm)
  near: boolean;     // HP dalam jarak
  rssi: number;      // dBm (0 = belum ada)
  neutral: number;   // 1 netral, 0 gigi, 2 tidak diketahui
  run: boolean;
  thr: number;       // ambang dekat (dBm)
  wake: number;      // profil bangun 0..2
  lock: number;      // profil kunci otomatis 0..3
  nr: boolean;       // wajib netral
  ao: boolean;       // kontak mati otomatis
  hi: boolean;       // (tidak dipakai sejak v4.1, selalu false)
  wf: boolean;       // peringatan HP menjauh
  tamper: number;    // jumlah percobaan pembobolan
  pair: boolean;     // ESP sedang membuka mode pairing
  calib: boolean;
}

export interface BleSnapshot {
  link: LinkState;
  error: string | null;
  voltage: number | null;
  engineOn: boolean;
  engineSec: number | null;
  /** teks idle yang tersimpan di ESP32 (null = belum diterima) */
  idleName: string | null;
  idleLine2: string | null;
  /** pengaturan yang tersimpan di ESP32 (null = belum diterima) */
  bright: number | null;
  findOn: boolean | null;
  findProf: number | null;
  findKey: boolean | null;
  parkMin: number | null;
  pinSet: boolean | null;
  /** galat perintah dari firmware (kode: pin, pinfmt, key, nokey); `at` berubah tiap kejadian */
  fwError: { code: string; at: number } | null;
  /** info kejadian dari firmware (start_ok, locked, calib:-72, ...) */
  fwNotice: { code: string; at: number } | null;
  kl: KlState | null;
  /** ESP32 memberi tahu ia tidur (hemat daya): app tidak menyambung ulang sampai Anda menekan sesuatu */
  sleeping: boolean;
  /** menunggu Anda mengetik PIN pairing dari layar OLED */
  needPin: boolean;
}

// ---------- store kecil untuk UI ----------
let snap: BleSnapshot = {
  link: "off",
  error: null,
  voltage: null,
  engineOn: false,
  engineSec: null,
  idleName: null,
  idleLine2: null,
  bright: null,
  findOn: null,
  findProf: null,
  findKey: null,
  parkMin: null,
  pinSet: null,
  fwError: null,
  fwNotice: null,
  kl: null,
  sleeping: false,
  needPin: false,
};
const listeners = new Set<() => void>();
function setSnap(p: Partial<BleSnapshot>) {
  snap = { ...snap, ...p };
  listeners.forEach((l) => l());
}
function subscribe(l: () => void) {
  listeners.add(l);
  return () => {
    listeners.delete(l);
  };
}
const getSnapshot = () => snap;
export function useBle(): BleSnapshot {
  return useSyncExternalStore(subscribe, getSnapshot, getSnapshot);
}

// ---------- manager ----------
let manager: BleManager | null = null;
function mgr(): BleManager {
  if (!manager) manager = new BleManager();
  return manager;
}

let device: Device | null = null;
let disconnectSub: Subscription | null = null;
let telemetrySub: Subscription | null = null;
let wantConnected = false;
let userStopped = false;
let busy: Promise<boolean> | null = null;
let retryTimer: ReturnType<typeof setTimeout> | null = null;
let retryDelay = 1000;
let linkVerified = false; // true setelah satu tulis ke ESP32 berhasil (artinya pairing/enkripsi beres)

// ---------- izin ----------
export async function requestBlePermissions(): Promise<boolean> {
  if (Platform.OS !== "android") return true;
  const api = Number(Platform.Version);
  const P = PermissionsAndroid.PERMISSIONS;
  const G = PermissionsAndroid.RESULTS.GRANTED;

  // Cek dulu TANPA dialog. Saat jalan di headless task (tidak ada Activity),
  // memanggil request*() bisa gagal/hang dan koneksi BLE tidak pernah terbentuk.
  try {
    const need = api >= 31 ? [P.BLUETOOTH_SCAN, P.BLUETOOTH_CONNECT] : [P.ACCESS_FINE_LOCATION];
    const have = await Promise.all(need.map((p) => PermissionsAndroid.check(p)));
    if (have.every(Boolean)) return true;
  } catch {}

  if (api >= 31) {
    // Android 12+: BLUETOOTH_SCAN & BLUETOOTH_CONNECT wajib. Lokasi diminta juga (tidak fatal).
    const r = await PermissionsAndroid.requestMultiple([
      P.BLUETOOTH_SCAN,
      P.BLUETOOTH_CONNECT,
      P.ACCESS_FINE_LOCATION,
    ]);
    return r[P.BLUETOOTH_SCAN] === G && r[P.BLUETOOTH_CONNECT] === G;
  }
  // (api < 31 jatuh ke bawah)
  // Android 11 ke bawah: hanya lokasi (izin BLUETOOTH_SCAN/CONNECT tidak ada di OS ini,
  // jika diminta hasilnya bukan GRANTED -> itulah bug pengecekan `every(GRANTED)` sebelumnya)
  const r = await PermissionsAndroid.request(P.ACCESS_FINE_LOCATION);
  return r === G;
}

// ---------- scan ----------
function scan(m: BleManager, timeoutMs: number): Promise<Device> {
  return new Promise((resolve, reject) => {
    let done = false;
    const finish = (fn: () => void) => {
      if (done) return;
      done = true;
      clearTimeout(timer);
      m.stopDeviceScan();
      fn();
    };
    const timer = setTimeout(
      () => finish(() => reject(new Error("ESP32 tidak ditemukan. Pastikan menyala & dekat."))),
      timeoutMs,
    );
    // Filter by service UUID: nama sering hanya ada di scan response, jadi jangan mengandalkan nama.
    m.startDeviceScan(
      [SVC],
      { scanMode: ScanMode.LowLatency, allowDuplicates: false },
      (err, d) => {
        if (err) return finish(() => reject(err));
        if (!d) return;
        const nm = d.name ?? d.localName;
        if (nm && nm !== DEVICE_NAME) return; // perangkat lain dengan UUID sama
        finish(() => resolve(d));
      },
    );
  });
}

function parseTelemetry(b64: string | null | undefined) {
  if (!b64) return;
  const txt = Buffer.from(b64, "base64").toString("utf8");
  if (txt.startsWith("I|")) {
    // "I|nama|baris2" — teks idle yang tersimpan di ESP32
    const q = txt.split("|");
    if (q.length >= 3) setSnap({ idleName: q[1], idleLine2: q.slice(2).join("|") });
    return;
  }
  if (txt.startsWith("S|")) {
    // "S|kecerahan|find|profil|kunci|tidur|pin" — pengaturan tersimpan di ESP32
    const q = txt.split("|");
    if (q.length >= 7) {
      setSnap({
        bright: parseInt(q[1], 10),
        findOn: q[2] === "1",
        findProf: parseInt(q[3], 10),
        findKey: q[4] === "1",
        parkMin: parseInt(q[5], 10),
        pinSet: q[6] === "1",
      });
    }
    return;
  }
  if (txt.startsWith("K|")) {
    const q = txt.split("|").slice(1).map((x) => parseInt(x, 10));
    if (q.length >= 18 && q.every((x) => Number.isFinite(x))) {
      setSnap({
        kl: {
          mode: q[0], ign: q[1] === 1, cdi: q[2] === 1, st: q[3], near: q[4] === 1, rssi: q[5],
          neutral: q[6], run: q[7] === 1, thr: q[8], wake: q[9], lock: q[10], nr: q[11] === 1,
          ao: q[12] === 1, hi: q[13] === 1, wf: q[14] === 1, tamper: q[15], pair: q[16] === 1, calib: q[17] === 1,
        },
      });
    }
    return;
  }
  if (txt.startsWith("N|")) {
    setSnap({ fwNotice: { code: txt.slice(2), at: Date.now() } });
    return;
  }
  if (txt.startsWith("Z|")) {
    // ESP32 akan tidur: putuskan dengan tenang & jangan menyambung ulang otomatis
    setSnap({ sleeping: true });
    return;
  }
  if (txt.startsWith("E|")) {
    setSnap({ fwError: { code: txt.slice(2), at: Date.now() } });
    return;
  }
  const p = txt.split(",");
  if (p.length < 3) return;
  const v = parseFloat(p[0]);
  const e = p[1] === "1";
  const s = parseInt(p[2], 10);
  if (Number.isFinite(v) && Number.isFinite(s)) {
    setSnap({ voltage: v, engineOn: e, engineSec: s });
  }
}

async function connectOnce(): Promise<boolean> {
  if (!(await requestBlePermissions())) {
    setSnap({ link: "off", error: "Izin Bluetooth ditolak" });
    return false;
  }
  const m = mgr();
  const st = await m.state();
  if (st !== State.PoweredOn) {
    setSnap({ link: "off", error: "Bluetooth mati — nyalakan dulu" });
    return false;
  }

  setSnap({ link: "scanning", error: null });
  const found = await scan(m, SCAN_TIMEOUT_MS);

  setSnap({ link: "connecting" });
  await new Promise<void>((r) => setTimeout(r, 200)); // beri jeda setelah stop scan
  const dev = await found.connect({
    autoConnect: false,
    requestMTU: 247,
    timeout: 10000,
    refreshGatt: "OnConnected", // buang cache GATT lama (penting setelah ganti firmware)
  });
  await dev.discoverAllServicesAndCharacteristics();

  device = dev;
  disconnectSub?.remove();
  disconnectSub = dev.onDisconnected(() => {
    telemetrySub?.remove();
    telemetrySub = null;
    device = null;
    setSnap({
      link: "off",
      voltage: null,
      engineSec: null,
      idleName: null,
      idleLine2: null,
      bright: null,
      findOn: null,
      findProf: null,
      findKey: null,
      parkMin: null,
      pinSet: null,
      kl: null,
      needPin: false,
    });
    linkVerified = false;
    if (wantConnected) scheduleRetry();
  });

  telemetrySub?.remove();
  telemetrySub = dev.monitorCharacteristicForService(SVC, TX, (err, ch) => {
    if (err) return; // error saat disconnect -> ditangani onDisconnected
    parseTelemetry(ch?.value);
  });

  retryDelay = 1000;
  setSnap({ link: "connected", error: null, sleeping: false });
  return true;
}

export function ensureConnected(): Promise<boolean> {
  if (device && snap.link === "connected") return Promise.resolve(true);
  if (busy) return busy;
  busy = connectOnce()
    .catch((e: any) => {
      setSnap({ link: "off", error: String(e?.message ?? e) });
      return false;
    })
    .finally(() => {
      busy = null;
    });
  return busy;
}

function scheduleRetry() {
  if (retryTimer || !wantConnected || snap.sleeping) return;
  retryTimer = setTimeout(async () => {
    retryTimer = null;
    if (!wantConnected) return;
    const ok = await ensureConnected();
    if (!ok && wantConnected) {
      // ESP32 yang sedang parkir (tidur) tidak akan menjawab: jangan memindai terus-menerus (hemat baterai HP)
      retryDelay = Math.min(retryDelay * 2, 30000);
      scheduleRetry();
    }
  }, retryDelay);
}

/** Mulai koneksi otomatis + reconnect otomatis (backoff 1s → 10s). Aman dipanggil berulang. */
export function startAutoConnect() {
  userStopped = false;
  if (snap.sleeping) setSnap({ sleeping: false });
  if (wantConnected) {
    void ensureConnected();
    return;
  }
  wantConnected = true;
  retryDelay = 1000;
  void ensureConnected().then((ok) => {
    if (!ok && wantConnected) scheduleRetry();
  });
}

/** Dipakai headless task: sambungkan hanya jika user tidak sengaja memutus. */
export function keepConnected() {
  if (!userStopped) startAutoConnect();
}

export function stopAutoConnect() {
  userStopped = true;
  wantConnected = false;
  if (retryTimer) clearTimeout(retryTimer);
  retryTimer = null;
  try {
    mgr().stopDeviceScan();
  } catch {}
  device?.cancelConnection().catch(() => {});
}

// ---------- kirim (diserialkan agar tidak bentrok operasi GATT) ----------
let txChain: Promise<unknown> = Promise.resolve();
export function sendJson(obj: object): Promise<boolean> {
  const run = async () => {
    // Tulis pertama setelah menyambung memicu dialog pairing Android (ketik PIN dari OLED):
    // beri beberapa kali kesempatan sebelum menyerah.
    const attempts = linkVerified ? 1 : 8;
    for (let i = 0; i < attempts; i++) {
      if (!device || snap.link !== "connected") return false;
      try {
        const b64 = Buffer.from(JSON.stringify(obj), "utf8").toString("base64");
        await device.writeCharacteristicWithResponseForService(SVC, RX, b64);
        if (!linkVerified) {
          linkVerified = true;
          setSnap({ needPin: false });
        }
        return true;
      } catch (e) {
        console.warn("TX gagal", e);
        if (linkVerified || i === attempts - 1) return false;
        setSnap({ needPin: true });
        await new Promise<void>((r) => setTimeout(r, 2500));
      }
    }
    setSnap({ needPin: false });
    return false;
  };
  const p = txChain.then(run, run);
  txChain = p;
  return p;
}

/** Pastikan terhubung (membangunkan ESP32 yang tidur). Menunggu sampai `ms` ms. */
export async function wakeAndConnect(ms = 25000): Promise<boolean> {
  if (device && snap.link === "connected") return true;
  startAutoConnect();
  const t0 = Date.now();
  while (Date.now() - t0 < ms) {
    if (snap.link === "connected" && device) return true;
    await new Promise<void>((r) => setTimeout(r, 250));
    if (snap.link === "off" && !busy && !retryTimer && Date.now() - t0 > 1500) startAutoConnect();
  }
  return snap.link === "connected";
}

export const getBleSnapshot = () => snap;
