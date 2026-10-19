import { useSyncExternalStore } from "react";
import { ensureConnected, keepConnected, sendJson } from "../ble/ble";
import {
  MAPS_PKG,
  NavInfo,
  parseMapsNotification,
  RawNotification,
} from "./parser";

export interface NavDebug {
  enabled: boolean;
  lastRaw: RawNotification | null;
  lastInfo: NavInfo | null;
  lastSentAt: number;
  count: number;
}

let state: NavDebug = {
  enabled: true,
  lastRaw: null,
  lastInfo: null,
  lastSentAt: 0,
  count: 0,
};
const listeners = new Set<() => void>();
function set(p: Partial<NavDebug>) {
  state = { ...state, ...p };
  listeners.forEach((l) => l());
}
function subscribe(l: () => void) {
  listeners.add(l);
  return () => {
    listeners.delete(l);
  };
}
export function useNavDebug(): NavDebug {
  return useSyncExternalStore(
    subscribe,
    () => state,
    () => state,
  );
}
export function setAutoNav(on: boolean) {
  set({ enabled: on });
  if (on) keepConnected();
}

let lastKey = "";
let lastSent = 0;

export async function handleNotification(raw: RawNotification) {
  if (raw.app !== MAPS_PKG) return;
  set({ lastRaw: raw });

  const info = parseMapsNotification(raw);
  if (!info) return;
  set({ lastInfo: info, count: state.count + 1 });
  if (!state.enabled) return;

  const key = JSON.stringify(info);
  const now = Date.now();
  if (key === lastKey && now - lastSent < 5000) return; // dedupe

  keepConnected();
  if (!(await ensureConnected())) return;

  const ok = await sendJson({ cmd: "nav", ...info, go: 1, auto: 1 });
  if (ok) {
    lastKey = key;
    lastSent = now;
    set({ lastSentAt: now });
  }
}
