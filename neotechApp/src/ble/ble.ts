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

export const SVC = "6e400001-b5a3-f393-e0a9-e50e24dcca9e";
export const RX = "6e400002-b5a3-f393-e0a9-e50e24dcca9e";
export const TX = "6e400003-b5a3-f393-e0a9-e50e24dcca9e";
export const DEVICE_NAME = "GLPro-Neotech";

const SCAN_TIMEOUT_MS = 12000;

export type LinkState = "off" | "scanning" | "connecting" | "connected";

export interface BleSnapshot {
  link: LinkState;
  error: string | null;
  voltage: number | null;
  engineOn: boolean;
  engineSec: number | null;
  idleName: string | null;
  idleLine2: string | null;
}

let snap: BleSnapshot = {
  link: "off",
  error: null,
  voltage: null,
  engineOn: false,
  engineSec: null,
  idleName: null,
  idleLine2: null,
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

export async function requestBlePermissions(): Promise<boolean> {
  if (Platform.OS !== "android") return true;
  const api = Number(Platform.Version);
  const P = PermissionsAndroid.PERMISSIONS;
  const G = PermissionsAndroid.RESULTS.GRANTED;

  try {
    const need =
      api >= 31
        ? [P.BLUETOOTH_SCAN, P.BLUETOOTH_CONNECT]
        : [P.ACCESS_FINE_LOCATION];
    const have = await Promise.all(
      need.map((p) => PermissionsAndroid.check(p)),
    );
    if (have.every(Boolean)) return true;
  } catch {}

  if (api >= 31) {
    const r = await PermissionsAndroid.requestMultiple([
      P.BLUETOOTH_SCAN,
      P.BLUETOOTH_CONNECT,
      P.ACCESS_FINE_LOCATION,
    ]);
    return r[P.BLUETOOTH_SCAN] === G && r[P.BLUETOOTH_CONNECT] === G;
  }
  const r = await PermissionsAndroid.request(P.ACCESS_FINE_LOCATION);
  return r === G;
}

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
      () =>
        finish(() =>
          reject(new Error("ESP32 tidak ditemukan. Pastikan menyala & dekat.")),
        ),
      timeoutMs,
    );
    m.startDeviceScan(
      [SVC],
      { scanMode: ScanMode.LowLatency, allowDuplicates: false },
      (err, d) => {
        if (err) return finish(() => reject(err));
        if (!d) return;
        const nm = d.name ?? d.localName;
        if (nm && nm !== DEVICE_NAME) return;
        finish(() => resolve(d));
      },
    );
  });
}

function parseTelemetry(b64: string | null | undefined) {
  if (!b64) return;
  const txt = Buffer.from(b64, "base64").toString("utf8");
  if (txt.startsWith("I|")) {
    const q = txt.split("|");
    if (q.length >= 3)
      setSnap({ idleName: q[1], idleLine2: q.slice(2).join("|") });
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
  await new Promise<void>((r) => setTimeout(r, 200));
  const dev = await found.connect({
    autoConnect: false,
    requestMTU: 247,
    timeout: 10000,
    refreshGatt: "OnConnected",
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
    });
    if (wantConnected) scheduleRetry();
  });

  telemetrySub?.remove();
  telemetrySub = dev.monitorCharacteristicForService(SVC, TX, (err, ch) => {
    if (err) return;
    parseTelemetry(ch?.value);
  });

  retryDelay = 1000;
  setSnap({ link: "connected", error: null });
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
  if (retryTimer || !wantConnected) return;
  retryTimer = setTimeout(async () => {
    retryTimer = null;
    if (!wantConnected) return;
    const ok = await ensureConnected();
    if (!ok && wantConnected) {
      retryDelay = Math.min(retryDelay * 2, 10000);
      scheduleRetry();
    }
  }, retryDelay);
}

export function startAutoConnect() {
  userStopped = false;
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

let txChain: Promise<unknown> = Promise.resolve();
export function sendJson(obj: object): Promise<boolean> {
  const run = async () => {
    if (!device || snap.link !== "connected") return false;
    try {
      const b64 = Buffer.from(JSON.stringify(obj), "utf8").toString("base64");
      await device.writeCharacteristicWithResponseForService(SVC, RX, b64);
      return true;
    } catch (e) {
      console.warn("TX gagal", e);
      return false;
    }
  };
  const p = txChain.then(run, run);
  txChain = p;
  return p;
}
