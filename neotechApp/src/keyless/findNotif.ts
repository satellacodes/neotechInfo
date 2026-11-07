/**
 * Notifikasi "Cari motor": notifikasi menetap di bilah notifikasi HP.
 * Mengetuknya membuka app lalu memerintahkan ESP32: 4 riting kilat ganda 3x + buzzer 2x.
 *
 * Memakai paket `expo-notifications` (harus dipasang: `npx expo install expo-notifications`).
 * Jika paket belum ada, fitur ini otomatis nonaktif dan sisa app tetap jalan.
 */
import { Platform } from "react-native";

let N: any = null;
try {
  N = require("expo-notifications");
} catch {
  N = null;
}

const CHANNEL = "find-bike";
const CATEGORY = "find";
const NOTIF_ID = "find-bike";

export const findNotifAvailable = !!N && Platform.OS === "android";

export async function isFindNotifShown(): Promise<boolean> {
  if (!findNotifAvailable) return false;
  try {
    const list = await N.getPresentedNotificationsAsync();
    return list.some((n: any) => n?.request?.identifier === NOTIF_ID);
  } catch {
    return false;
  }
}

export async function enableFindNotif(): Promise<boolean> {
  if (!findNotifAvailable) return false;
  try {
    const perm = await N.requestPermissionsAsync();
    if (!perm.granted) return false;
    await N.setNotificationChannelAsync(CHANNEL, {
      name: "Cari motor",
      importance: N.AndroidImportance.LOW,
      sound: null,
      vibrationPattern: null,
    });
    await N.setNotificationCategoryAsync(CATEGORY, [
      { identifier: "FIND", buttonTitle: "Cari motor", options: { opensAppToForeground: true } },
    ]);
    await N.scheduleNotificationAsync({
      identifier: NOTIF_ID,
      content: {
        title: "GL Pro — cari motor",
        body: "Ketuk untuk menyalakan riting & buzzer motor",
        categoryIdentifier: CATEGORY,
        sticky: true,
        autoDismiss: false,
        data: { action: "find" },
      },
      trigger: { channelId: CHANNEL },
    });
    return true;
  } catch (e) {
    console.warn("notif gagal", e);
    return false;
  }
}

export async function disableFindNotif(): Promise<void> {
  if (!findNotifAvailable) return;
  try {
    await N.dismissNotificationAsync(NOTIF_ID);
    await N.cancelScheduledNotificationAsync(NOTIF_ID);
  } catch {}
}

/** Panggil `cb` setiap notifikasi "Cari motor" diketuk (termasuk yang membuka app dari keadaan mati). */
export function listenFindNotif(cb: () => void): () => void {
  if (!findNotifAvailable) return () => {};
  let lastHandled = "";
  const handle = (resp: any) => {
    const id = String(resp?.notification?.date ?? "") + String(resp?.actionIdentifier ?? "");
    if (resp?.notification?.request?.content?.data?.action !== "find") return;
    if (id === lastHandled) return;
    lastHandled = id;
    cb();
  };
  const sub = N.addNotificationResponseReceivedListener(handle);
  N.getLastNotificationResponseAsync?.()
    .then((r: any) => r && handle(r))
    .catch(() => {});
  return () => sub?.remove?.();
}
