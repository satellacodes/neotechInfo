const { withAndroidManifest } = require("expo/config-plugins");

const LISTENER =
  "com.lesimoes.androidnotificationlistener.RNAndroidNotificationListener";
const HEADLESS =
  "com.lesimoes.androidnotificationlistener.RNAndroidNotificationListenerHeadlessJsTaskService";

module.exports = function withNotificationListener(config) {
  return withAndroidManifest(config, (cfg) => {
    const app = cfg.modResults.manifest.application?.[0];
    if (!app) return cfg;
    app.service = app.service ?? [];
    const has = (n) => app.service.some((s) => s.$?.["android:name"] === n);

    if (!has(LISTENER)) {
      app.service.push({
        $: {
          "android:name": LISTENER,
          "android:label": "@string/app_name",
          "android:exported": "true",
          "android:permission":
            "android.permission.BIND_NOTIFICATION_LISTENER_SERVICE",
        },
        "intent-filter": [
          {
            action: [
              {
                $: {
                  "android:name":
                    "android.service.notification.NotificationListenerService",
                },
              },
            ],
          },
        ],
      });
    }
    if (!has(HEADLESS)) {
      app.service.push({ $: { "android:name": HEADLESS } });
    }
    return cfg;
  });
};
