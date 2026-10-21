import * as Location from "expo-location";
import * as TaskManager from "expo-task-manager";
import { useSyncExternalStore } from "react";
import { Platform } from "react-native";

import { keepConnected, sendJson } from "../ble/ble";

export const SPEED_TASK = "glpro-speed-task";

export interface SpeedState {
  running: boolean;
  kmh: number | null;
  accuracy: number | null;
  error: string | null;
}

let state: SpeedState = {
  running: false,
  kmh: null,
  accuracy: null,
  error: null,
};
const listeners = new Set<() => void>();
function set(p: Partial<SpeedState>) {
  state = { ...state, ...p };
  listeners.forEach((l) => l());
}
function subscribe(l: () => void) {
  listeners.add(l);
  return () => {
    listeners.delete(l);
  };
}
export function useSpeed(): SpeedState {
  return useSyncExternalStore(
    subscribe,
    () => state,
    () => state,
  );
}

let smooth: number | null = null;
let lastSent = -1;
let lastSentAt = 0;

const MAX_ACCURACY_M = 60;
const STANDSTILL_KMH = 3;

function onLocations(locs: Location.LocationObject[]) {
  const l = locs[locs.length - 1];
  if (!l) return;
  const acc = l.coords.accuracy ?? null;
  const v = l.coords.speed;

  if (v == null || v < 0 || (acc != null && acc > MAX_ACCURACY_M)) {
    set({ accuracy: acc });
    return;
  }

  const kmh = v * 3.6;
  smooth = smooth == null ? kmh : 0.7 * kmh + 0.3 * smooth;
  const shown = smooth < STANDSTILL_KMH ? 0 : Math.round(smooth);
  set({ kmh: shown, accuracy: acc, error: null });

  const now = Date.now();
  if (shown !== lastSent || now - lastSentAt > 2000) {
    lastSent = shown;
    lastSentAt = now;
    keepConnected();
    void sendJson({ cmd: "speed", kmh: shown });
  }
}

if (Platform.OS === "android") {
  TaskManager.defineTask(SPEED_TASK, async ({ data, error }) => {
    if (error) {
      set({ error: error.message });
      return;
    }
    const locations = (
      data as { locations?: Location.LocationObject[] } | undefined
    )?.locations;
    if (locations?.length) onLocations(locations);
  });
}

export async function syncSpeedRunning() {
  if (Platform.OS !== "android") return;
  try {
    const on = await Location.hasStartedLocationUpdatesAsync(SPEED_TASK);
    set({ running: on });
  } catch {
    set({ running: false });
  }
}

export async function startSpeed(): Promise<{ ok: boolean; msg?: string }> {
  try {
    const fg = await Location.requestForegroundPermissionsAsync();
    if (fg.status !== "granted")
      return { ok: false, msg: "Izin lokasi ditolak" };

    if (!(await Location.hasServicesEnabledAsync()))
      return { ok: false, msg: "Aktifkan Lokasi/GPS di HP dulu" };

    if (await Location.hasStartedLocationUpdatesAsync(SPEED_TASK)) {
      await Location.stopLocationUpdatesAsync(SPEED_TASK);
    }
    smooth = null;
    lastSent = -1;

    await Location.startLocationUpdatesAsync(SPEED_TASK, {
      accuracy: Location.Accuracy.BestForNavigation,
      timeInterval: 1000,
      distanceInterval: 0,
      foregroundService: {
        notificationTitle: "GL Pro Display",
        notificationBody: "Kecepatan GPS aktif",
        notificationColor: "#2ea8ff",
      },
    });
    set({ running: true, error: null });
    return { ok: true };
  } catch (e: any) {
    set({ running: false, error: String(e?.message ?? e) });
    return { ok: false, msg: String(e?.message ?? e) };
  }
}

export async function stopSpeed() {
  try {
    if (await Location.hasStartedLocationUpdatesAsync(SPEED_TASK)) {
      await Location.stopLocationUpdatesAsync(SPEED_TASK);
    }
  } catch {}
  set({ running: false, kmh: null, accuracy: null });
  smooth = null;
  void sendJson({ cmd: "speed", kmh: -1 });
}
