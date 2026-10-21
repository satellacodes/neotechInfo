import { AppRegistry, Platform } from "react-native";
import { handleNotification } from "./controller";
import "./speed";
import type { RawNotification } from "./parser";

if (Platform.OS === "android") {
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
