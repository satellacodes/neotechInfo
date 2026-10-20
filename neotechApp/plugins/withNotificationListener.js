const fs = require("fs");
const path = require("path");
const {
  withAndroidManifest,
  withGradleProperties,
  withDangerousMod,
} = require("expo/config-plugins");

const KEEP_RULES = `
# react-native-android-notification-listener (Gson butuh nama field asli)
-keep class com.lesimoes.androidnotificationlistener.** { *; }
-keepclassmembers class com.lesimoes.androidnotificationlistener.** { *; }
-keepattributes Signature,*Annotation*
`;

const LISTENER =
  "com.lesimoes.androidnotificationlistener.RNAndroidNotificationListener";
const HEADLESS =
  "com.lesimoes.androidnotificationlistener.RNAndroidNotificationListenerHeadlessJsTaskService";

module.exports = function withNotificationListener(config) {
  config = withGradleProperties(config, (cfg) => {
    const k = "android.enableMinifyInReleaseBuilds";
    const arr = cfg.modResults;
    const i = arr.findIndex((p) => p.type === "property" && p.key === k);
    if (i >= 0) arr[i].value = "false";
    else arr.push({ type: "property", key: k, value: "false" });
    return cfg;
  });

  config = withDangerousMod(config, [
    "android",
    async (cfg) => {
      const f = path.join(
        cfg.modRequest.platformProjectRoot,
        "app",
        "proguard-rules.pro",
      );
      const cur = fs.existsSync(f) ? fs.readFileSync(f, "utf8") : "";
      if (!cur.includes("com.lesimoes.androidnotificationlistener")) {
        fs.writeFileSync(f, cur + KEEP_RULES);
      }
      return cfg;
    },
  ]);

  return withAndroidManifest(config, (cfg) => {
    const manifest = cfg.modResults.manifest;
    const app = manifest.application?.[0];
    if (!app) return cfg;

    manifest.$["xmlns:tools"] = "http://schemas.android.com/tools";
    const cur = (app.$["tools:replace"] || "")
      .split(",")
      .map((x) => x.trim())
      .filter(Boolean);
    if (!cur.includes("android:allowBackup")) cur.push("android:allowBackup");
    app.$["tools:replace"] = cur.join(",");

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
