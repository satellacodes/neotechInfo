import React, { useCallback, useEffect, useState } from "react";
import {
  Alert,
  AppState,
  Platform,
  ScrollView,
  StatusBar,
  StyleSheet,
  Switch,
  Text,
  TextInput,
  TouchableOpacity,
  View,
} from "react-native";

import {
  sendJson,
  startAutoConnect,
  stopAutoConnect,
  useBle,
} from "../ble/ble";
import { setAutoNav, useNavDebug } from "../nav/controller";

const ARROWS = [
  "straight",
  "left",
  "right",
  "slight-left",
  "slight-right",
  "sharp-left",
  "sharp-right",
  "uturn",
  "roundabout",
  "arrive",
] as const;

let NotifListener: any = null;
if (Platform.OS === "android") {
  NotifListener = require("react-native-android-notification-listener").default;
}

function fmtEngine(sec: number | null) {
  if (sec == null) return "--";
  const h = Math.floor(sec / 3600);
  const m = Math.floor((sec % 3600) / 60);
  return `${h} jam ${m} menit`;
}

export default function App() {
  const ble = useBle();
  const nav = useNavDebug();

  const [mode, setMode] = useState(0);
  const [arrow, setArrow] = useState<string>("straight");
  const [distance, setDistance] = useState("150 m");
  const [street, setStreet] = useState("Jl. Sudirman");
  const [eta, setEta] = useState("14:32");
  const [notifPerm, setNotifPerm] = useState("unknown");

  const connected = ble.link === "connected";
  const busy = ble.link === "scanning" || ble.link === "connecting";

  // Mulai koneksi otomatis saat app dibuka (idempotent, aman untuk StrictMode)
  useEffect(() => {
    startAutoConnect();
  }, []);

  const refreshPerm = useCallback(async () => {
    if (!NotifListener) return;
    try {
      setNotifPerm(await NotifListener.getPermissionStatus());
    } catch {
      setNotifPerm("unknown");
    }
  }, []);

  useEffect(() => {
    refreshPerm();
    const sub = AppState.addEventListener("change", (s) => {
      if (s === "active") refreshPerm();
    });
    return () => sub.remove();
  }, [refreshPerm]);

  const setModeAndSend = (m: number) => {
    setMode(m);
    sendJson({ cmd: "mode", val: m });
  };

  const sendNav = () => {
    sendJson({ cmd: "nav", arrow, distance, street, eta, go: 1, auto: 0 });
    setMode(2);
  };

  const confirmReset = () =>
    Alert.alert("Reset timer mesin?", "Jam mesin akan kembali ke 0.", [
      { text: "Batal", style: "cancel" },
      {
        text: "Reset",
        style: "destructive",
        onPress: () => sendJson({ cmd: "reset_time" }),
      },
    ]);

  const linkText = connected
    ? "✅ Terhubung ke ESP32"
    : ble.link === "scanning"
      ? "🔍 Mencari ESP32..."
      : ble.link === "connecting"
        ? "🔗 Menyambung..."
        : "❌ Belum terhubung";

  const raw = nav.lastRaw;
  const rawText = raw
    ? [raw.title, raw.text, raw.bigText, raw.subText].filter(Boolean).join("\n")
    : "(belum ada notifikasi Google Maps)";

  return (
    <ScrollView contentContainerStyle={s.root}>
      <StatusBar barStyle="light-content" />
      <Text style={s.title}>GL Pro Smart Display</Text>

      {/* STATUS */}
      <View style={s.card}>
        <Text style={s.status}>{linkText}</Text>
        {ble.error && !connected ? (
          <Text style={s.err}>{ble.error}</Text>
        ) : null}
        {connected && (
          <View style={s.live}>
            <View style={s.liveBox}>
              <Text style={s.liveVal}>
                {ble.voltage != null ? ble.voltage.toFixed(2) : "--"}
                <Text style={s.liveUnit}> V</Text>
              </Text>
              <Text style={s.liveLbl}>
                Tegangan aki · {ble.engineOn ? "mesin ON" : "mesin OFF"}
              </Text>
            </View>
            <View style={s.liveBox}>
              <Text style={s.liveValSm}>{fmtEngine(ble.engineSec)}</Text>
              <Text style={s.liveLbl}>Waktu mesin hidup</Text>
            </View>
          </View>
        )}
      </View>

      <TouchableOpacity
        style={[s.btn, connected && s.btnGray, busy && s.btnDim]}
        disabled={busy}
        onPress={() =>
          connected || busy ? stopAutoConnect() : startAutoConnect()
        }
      >
        <Text style={s.btnTxt}>
          {connected
            ? "Putuskan"
            : busy
              ? "Menghubungkan..."
              : "Hubungkan ke ESP32"}
        </Text>
      </TouchableOpacity>

      {/* MODE */}
      <Text style={s.h}>Mode Display</Text>
      <View style={s.row}>
        {["Tegangan", "Waktu", "Navigasi"].map((l, i) => (
          <TouchableOpacity
            key={l}
            style={[s.mbtn, mode === i && s.mbtnOn]}
            onPress={() => setModeAndSend(i)}
          >
            <Text style={[s.mtxt, mode === i && s.mtxtOn]}>{l}</Text>
          </TouchableOpacity>
        ))}
      </View>
      <TouchableOpacity
        style={[s.btn, s.btnGray]}
        onPress={() => sendJson({ cmd: "idle" })}
      >
        <Text style={s.btnTxt}>Tampilkan Layar Idle</Text>
      </TouchableOpacity>

      {/* NAVIGASI OTOMATIS */}
      {Platform.OS === "android" && (
        <>
          <Text style={s.h}>Navigasi Otomatis (Google Maps)</Text>
          <View style={s.card}>
            <View style={s.switchRow}>
              <Text style={s.status}>Kirim belokan ke display</Text>
              <Switch value={nav.enabled} onValueChange={setAutoNav} />
            </View>

            <Text style={s.lbl}>
              Akses notifikasi:{" "}
              {notifPerm === "authorized" ? "✅ aktif" : "❌ belum diberikan"}
            </Text>
            {notifPerm !== "authorized" && (
              <TouchableOpacity
                style={s.btn}
                onPress={() => NotifListener?.requestPermission()}
              >
                <Text style={s.btnTxt}>Beri Akses Notifikasi</Text>
              </TouchableOpacity>
            )}

            <Text style={s.lbl}>Notifikasi Maps terakhir:</Text>
            <Text style={s.mono}>{rawText}</Text>
            <Text style={s.lbl}>Hasil parsing:</Text>
            <Text style={s.mono}>
              {nav.lastInfo
                ? `arrow: ${nav.lastInfo.arrow}\njarak: ${nav.lastInfo.distance}\njalan: ${nav.lastInfo.street}\neta: ${nav.lastInfo.eta}`
                : "-"}
            </Text>
          </View>
        </>
      )}

      {/* NAVIGASI MANUAL */}
      <Text style={s.h}>Navigasi Manual (tes)</Text>
      <Text style={s.lbl}>Arah:</Text>
      <ScrollView horizontal showsHorizontalScrollIndicator={false}>
        {ARROWS.map((a) => (
          <TouchableOpacity
            key={a}
            style={[s.chip, arrow === a && s.chipOn]}
            onPress={() => setArrow(a)}
          >
            <Text style={[s.chipTxt, arrow === a && s.chipTxtOn]}>{a}</Text>
          </TouchableOpacity>
        ))}
      </ScrollView>
      <TextInput
        style={s.input}
        value={distance}
        onChangeText={setDistance}
        placeholder="150 m"
        placeholderTextColor="#6e7681"
      />
      <TextInput
        style={s.input}
        value={street}
        onChangeText={setStreet}
        placeholder="Nama jalan"
        placeholderTextColor="#6e7681"
      />
      <TextInput
        style={s.input}
        value={eta}
        onChangeText={setEta}
        placeholder="14:32"
        placeholderTextColor="#6e7681"
      />
      <TouchableOpacity style={s.btn} onPress={sendNav}>
        <Text style={s.btnTxt}>Kirim Navigasi</Text>
      </TouchableOpacity>

      <TouchableOpacity style={[s.btn, s.btnRed]} onPress={confirmReset}>
        <Text style={s.btnTxt}>Reset Timer Mesin</Text>
      </TouchableOpacity>

      <Text style={s.hint}>
        Tips di motor: Pengaturan HP → Aplikasi → neotechApp → Baterai → "Tidak
        dibatasi", supaya Android tidak mematikan koneksi BLE saat layar mati.
      </Text>
    </ScrollView>
  );
}

const s = StyleSheet.create({
  root: {
    padding: 20,
    paddingTop: 55,
    paddingBottom: 60,
    backgroundColor: "#0d1117",
    flexGrow: 1,
  },
  title: { color: "#fff", fontSize: 22, fontWeight: "800", marginBottom: 14 },
  card: {
    backgroundColor: "#161b22",
    padding: 12,
    borderRadius: 10,
    marginBottom: 10,
  },
  status: { color: "#e6edf3", fontSize: 14 },
  err: { color: "#f85149", fontSize: 12, marginTop: 6 },
  live: { flexDirection: "row", gap: 10, marginTop: 12 },
  liveBox: {
    flex: 1,
    backgroundColor: "#0d1117",
    borderRadius: 8,
    padding: 10,
  },
  liveVal: { color: "#3fb950", fontSize: 28, fontWeight: "800" },
  liveUnit: { fontSize: 16, color: "#8b949e" },
  liveValSm: {
    color: "#58a6ff",
    fontSize: 18,
    fontWeight: "800",
    marginTop: 6,
  },
  liveLbl: { color: "#8b949e", fontSize: 11, marginTop: 4 },
  switchRow: {
    flexDirection: "row",
    justifyContent: "space-between",
    alignItems: "center",
    marginBottom: 8,
  },
  h: {
    color: "#58a6ff",
    fontSize: 15,
    fontWeight: "700",
    marginTop: 20,
    marginBottom: 8,
  },
  btn: {
    backgroundColor: "#238636",
    padding: 14,
    borderRadius: 10,
    marginVertical: 6,
    alignItems: "center",
  },
  btnDim: { backgroundColor: "#1f3b26" },
  btnGray: { backgroundColor: "#30363d" },
  btnRed: { backgroundColor: "#b62324" },
  btnTxt: { color: "#fff", fontWeight: "800", fontSize: 15 },
  row: { flexDirection: "row", gap: 8, marginBottom: 8 },
  mbtn: {
    flex: 1,
    paddingVertical: 12,
    backgroundColor: "#161b22",
    borderRadius: 10,
    alignItems: "center",
  },
  mbtnOn: { backgroundColor: "#238636" },
  mtxt: { color: "#8b949e", fontSize: 13, fontWeight: "600" },
  mtxtOn: { color: "#fff" },
  lbl: { color: "#8b949e", marginTop: 8, marginBottom: 4, fontSize: 12 },
  mono: {
    color: "#c9d1d9",
    fontSize: 12,
    fontFamily: Platform.OS === "android" ? "monospace" : "Menlo",
  },
  chip: {
    paddingHorizontal: 12,
    paddingVertical: 8,
    backgroundColor: "#161b22",
    borderRadius: 20,
    marginRight: 8,
    marginBottom: 6,
  },
  chipOn: { backgroundColor: "#238636" },
  chipTxt: { color: "#8b949e", fontSize: 12 },
  chipTxtOn: { color: "#fff", fontWeight: "700" },
  input: {
    backgroundColor: "#161b22",
    color: "#fff",
    padding: 12,
    borderRadius: 10,
    marginVertical: 5,
  },
  hint: { color: "#6e7681", fontSize: 12, marginTop: 20, lineHeight: 18 },
});
