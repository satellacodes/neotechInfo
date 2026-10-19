import { AppRegistry, Platform } from "react-native";
import { handleNotification } from "./controller";
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
