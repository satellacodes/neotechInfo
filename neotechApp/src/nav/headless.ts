/**
 * Headless task: dijalankan Android setiap ada notifikasi baru, walau app tidak tampil.
 * Harus di-import dari index.js (entry point) agar terdaftar sebelum UI dibuat.
 */
import { AppRegistry, Platform } from "react-native";
import { handleNotification } from "./controller";
import "./speed"; // daftarkan task GPS (TaskManager.defineTask) sebelum UI dibuat
import type { RawNotification } from "./parser";

if (Platform.OS === "android") {
  // require di dalam guard supaya bundle iOS/web tidak memuat modul native Android
  const {
    RNAndroidNotificationListenerHeadlessJsName,
  } = require("react-native-android-notification-listener");

  const task = async ({
    notification,
  }: {
    notification?: string | RawNotification;
  }) => {
    if (!notification) return;
    try {
      const n: RawNotification =
        typeof notification === "string"
          ? JSON.parse(notification)
          : notification;
      await handleNotification(n);
    } catch (e) {
      console.warn("headless notif error", e);
    }
  };

  AppRegistry.registerHeadlessTask(
    RNAndroidNotificationListenerHeadlessJsName,
    () => task,
  );
}
