import React, { useCallback, useEffect, useRef, useState } from "react";
import {
  AppState,
  Platform,
  RefreshControl,
  ScrollView,
  StatusBar,
  StyleSheet,
  Switch,
  Text,
  TextInput,
  View,
} from "react-native";
import { useSafeAreaInsets } from "react-native-safe-area-context";
import Animated, { FadeIn } from "react-native-reanimated";

import {
  sendJson,
  startAutoConnect,
  stopAutoConnect,
  useBle,
} from "../ble/ble";
import { setAutoNav, useNavDebug } from "../nav/controller";
import {
  startSpeed,
  stopSpeed,
  syncSpeedRunning,
  useSpeed,
} from "../nav/speed";
import {
  Button,
  Card,
  Collapsible,
  ErrorBoundary,
  PressableScale,
  Segmented,
  Skeleton,
  StatusPill,
  useToast,
} from "../ui/components";
import { C, R, S, T } from "../ui/theme";

const DEF_NAME = "Dimas";
const DEF_LINE2 = "mau kemana hari ini?";
const NAME_MAX = 16;
const LINE2_MAX = 24;

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

/** OLED hanya menampilkan ASCII — buang aksen & karakter lain, '|' dipakai protokol */
const toLabel = (v: string) =>
  v
    .normalize("NFD")
    .replace(/[̀-ͯ]/g, "")
    .replace(/[^\x20-\x7e]/g, "")
    .replace(/\|/g, "/");

function fmtEngine(sec: number | null) {
  if (sec == null) return null;
  return { h: Math.floor(sec / 3600), m: Math.floor((sec % 3600) / 60) };
}

function voltStatus(v: number) {
  if (v < 11.8) return { t: "Aki lemah", c: C.danger };
  if (v > 14.9) return { t: "Over charge", c: C.danger };
  if (v >= 13.2) return { t: "Charging", c: C.ok };
  if (v >= 12.4) return { t: "Aki OK", c: C.ok };
  return { t: "Aki cukup", c: C.warn };
}

function Screen() {
  const insets = useSafeAreaInsets();
  const ble = useBle();
  const nav = useNavDebug();
  const speed = useSpeed();
  const [speedBusy, setSpeedBusy] = useState(false);
  const { toast, show } = useToast();

  const [mode, setMode] = useState(0);
  const [name, setName] = useState(DEF_NAME);
  const [line2, setLine2] = useState(DEF_LINE2);
  const dirty = useRef(false);
  const [saving, setSaving] = useState(false);

  const [arrow, setArrow] = useState<string>("straight");
  const [distance, setDistance] = useState("150 m");
  const [street, setStreet] = useState("Jl. Sudirman");
  const [eta, setEta] = useState("14:32");
  const [showManual, setShowManual] = useState(false);
  const [showDebug, setShowDebug] = useState(false);
  const [notifPerm, setNotifPerm] = useState("unknown");

  const connected = ble.link === "connected";
  const busy = ble.link === "scanning" || ble.link === "connecting";
  const hasData = ble.voltage != null;

  useEffect(() => {
    startAutoConnect();
    syncSpeedRunning();
  }, []);

  const toggleSpeed = async (on: boolean) => {
    setSpeedBusy(true);
    if (on) {
      const r = await startSpeed();
      if (!r.ok) show(r.msg ?? "Gagal mengaktifkan GPS", "err");
      else show("Kecepatan GPS aktif", "ok");
    } else {
      await stopSpeed();
    }
    setSpeedBusy(false);
  };

  // isi form dari nilai yang tersimpan di ESP32 (sekali, selama user belum mengetik)
  useEffect(() => {
    if (!dirty.current && ble.idleName != null && ble.idleLine2 != null) {
      setName(ble.idleName);
      setLine2(ble.idleLine2);
    }
  }, [ble.idleName, ble.idleLine2]);

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
    const sub = AppState.addEventListener("change", (st) => {
      if (st === "active") refreshPerm();
    });
    return () => sub.remove();
  }, [refreshPerm]);

  /** kirim + beri umpan balik; menolak halus jika belum terhubung */
  const send = useCallback(
    async (obj: object, okMsg?: string): Promise<boolean> => {
      if (!connected) {
        show("Belum terhubung ke ESP32", "err");
        return false;
      }
      const ok = await sendJson(obj);
      if (ok && okMsg) show(okMsg, "ok");
      if (!ok) show("Gagal mengirim, coba lagi", "err");
      return ok;
    },
    [connected, show],
  );

  const changeMode = (m: number) => {
    setMode(m);
    send({ cmd: "mode", val: m });
  };

  const writeIdle = async () => {
    setSaving(true);
    const n = toLabel(name).trim() || DEF_NAME;
    const l = toLabel(line2).trim() || DEF_LINE2;
    setName(n);
    setLine2(l);
    const ok = await send(
      { cmd: "idle_text", name: n, line2: l },
      "Tersimpan di display ✓",
    );
    if (ok) dirty.current = false;
    setSaving(false);
  };

  const resetIdle = () => {
    dirty.current = true;
    setName(DEF_NAME);
    setLine2(DEF_LINE2);
  };

  const sendNav = () => {
    send(
      { cmd: "nav", arrow, distance, street, eta, go: 1, auto: 0 },
      "Navigasi terkirim",
    );
    setMode(2);
  };

  const [confirmReset, setConfirmReset] = useState(false);
  useEffect(() => {
    if (!confirmReset) return;
    const t = setTimeout(() => setConfirmReset(false), 3500);
    return () => clearTimeout(t);
  }, [confirmReset]);
  const onReset = () => {
    if (!confirmReset) {
      setConfirmReset(true);
      show("Tekan sekali lagi untuk reset", "info");
      return;
    }
    setConfirmReset(false);
    send({ cmd: "reset_time" }, "Timer mesin direset");
  };

  const pill = connected
    ? { st: "ok" as const, tx: "Terhubung" }
    : busy
      ? {
          st: "busy" as const,
          tx: ble.link === "scanning" ? "Mencari..." : "Menyambung...",
        }
      : { st: "off" as const, tx: "Terputus" };

  const eng = fmtEngine(ble.engineSec);
  const vs = hasData ? voltStatus(ble.voltage!) : null;

  const raw = nav.lastRaw;
  const rawText = raw
    ? [raw.title, raw.text, raw.bigText, raw.subText].filter(Boolean).join("\n")
    : "(belum ada notifikasi Google Maps)";

  const prevName = toLabel(name).trim() || DEF_NAME;
  const prevLine2 = toLabel(line2).trim() || DEF_LINE2;

  return (
    <View style={{ flex: 1, backgroundColor: C.bg }}>
      <StatusBar barStyle="light-content" />
      <ScrollView
        contentContainerStyle={{
          padding: S.lg,
          paddingTop: insets.top + S.lg,
          paddingBottom: insets.bottom + 96,
        }}
        keyboardShouldPersistTaps="handled"
        refreshControl={
          <RefreshControl
            refreshing={false}
            tintColor={C.accent}
            onRefresh={() => {
              if (!connected && !busy) startAutoConnect();
              refreshPerm();
            }}
          />
        }
      >
        {/* HEADER */}
        <Animated.View entering={FadeIn.duration(400)} style={st.header}>
          <View style={{ flex: 1 }}>
            <Text style={T.title}>GL Pro Display</Text>
            <Text style={T.small}>Neotech · BLE</Text>
          </View>
          <StatusPill state={pill.st} text={pill.tx} />
        </Animated.View>

        {/* LIVE */}
        <Card title="Kondisi motor" index={0}>
          {!connected && !busy && !hasData ? (
            <View style={st.empty}>
              <Text style={T.body}>Belum terhubung ke ESP32</Text>
              <Text style={T.small}>
                {ble.error ?? "Nyalakan motor / display, lalu sambungkan."}
              </Text>
            </View>
          ) : (
            <View style={st.row}>
              <View style={st.tile}>
                <Text style={T.small}>Tegangan aki</Text>
                {hasData ? (
                  <Animated.Text
                    key="v"
                    entering={FadeIn.duration(250)}
                    style={[T.big, { color: vs!.c }]}
                  >
                    {ble.voltage!.toFixed(2)}
                    <Text style={st.unit}> V</Text>
                  </Animated.Text>
                ) : (
                  <Skeleton w={96} h={38} style={{ marginVertical: 4 }} />
                )}
                {hasData ? (
                  <Text style={[st.badge, { color: vs!.c }]}>{vs!.t}</Text>
                ) : (
                  <Skeleton w={60} h={12} />
                )}
              </View>
              <View style={st.tile}>
                <Text style={T.small}>Waktu mesin hidup</Text>
                {eng ? (
                  <Animated.Text
                    key="e"
                    entering={FadeIn.duration(250)}
                    style={[T.big, { color: C.accent }]}
                  >
                    {eng.h}
                    <Text style={st.unit}>j </Text>
                    {String(eng.m).padStart(2, "0")}
                    <Text style={st.unit}>m</Text>
                  </Animated.Text>
                ) : (
                  <Skeleton w={110} h={38} style={{ marginVertical: 4 }} />
                )}
                {eng ? (
                  <Text
                    style={[st.badge, { color: ble.engineOn ? C.ok : C.sub }]}
                  >
                    {ble.engineOn ? "Mesin ON" : "Mesin OFF"}
                  </Text>
                ) : (
                  <Skeleton w={60} h={12} />
                )}
              </View>
            </View>
          )}
          <Button
            label={
              connected
                ? "Putuskan"
                : busy
                  ? "Menghubungkan..."
                  : "Hubungkan ke ESP32"
            }
            variant={connected ? "ghost" : "primary"}
            loading={busy}
            onPress={() => (connected ? stopAutoConnect() : startAutoConnect())}
          />
        </Card>

        {/* MODE */}
        <Card title="Mode display" index={1}>
          <Segmented
            options={["Tegangan", "Waktu", "Navigasi"]}
            value={mode}
            onChange={changeMode}
          />
          <Button
            label="Tampilkan layar idle"
            variant="ghost"
            onPress={() => send({ cmd: "idle" })}
          />
        </Card>

        {/* IDLE EDITOR */}
        <Card title="Teks layar idle" index={2}>
          <View style={st.oled}>
            <Text style={st.oledName} numberOfLines={1} adjustsFontSizeToFit>
              Hai, {prevName}
            </Text>
            <Text style={st.oledLine} numberOfLines={1} adjustsFontSizeToFit>
              {prevLine2}
            </Text>
          </View>
          <View>
            <Text style={st.lbl}>Nama</Text>
            <TextInput
              style={st.input}
              value={name}
              maxLength={NAME_MAX}
              placeholder={DEF_NAME}
              placeholderTextColor={C.sub}
              onChangeText={(v) => {
                dirty.current = true;
                setName(v);
              }}
            />
            <Text style={st.count}>
              {name.length}/{NAME_MAX}
            </Text>
          </View>
          <View>
            <Text style={st.lbl}>Teks kedua</Text>
            <TextInput
              style={st.input}
              value={line2}
              maxLength={LINE2_MAX}
              placeholder={DEF_LINE2}
              placeholderTextColor={C.sub}
              onChangeText={(v) => {
                dirty.current = true;
                setLine2(v);
              }}
            />
            <Text style={st.count}>
              {line2.length}/{LINE2_MAX}
            </Text>
          </View>
          <View style={st.row}>
            <Button
              label="Default"
              variant="ghost"
              onPress={resetIdle}
              style={{ flex: 1 }}
            />
            <Button
              label="Tulis ke display"
              loading={saving}
              onPress={writeIdle}
              style={{ flex: 2 }}
            />
          </View>
          <Text style={T.small}>
            Hanya huruf, angka, dan tanda baca standar (aksen otomatis dibuang).
          </Text>
        </Card>

        {/* NAV OTOMATIS */}
        {Platform.OS === "android" && (
          <Card title="Navigasi otomatis" index={3}>
            <View style={st.between}>
              <Text style={T.body}>Kirim belokan Google Maps</Text>
              <Switch
                value={nav.enabled}
                onValueChange={setAutoNav}
                trackColor={{ true: C.accentDim, false: C.surface2 }}
                thumbColor={nav.enabled ? C.accent : C.sub}
              />
            </View>
            <View style={st.between}>
              <Text style={T.small}>Akses notifikasi</Text>
              <Text
                style={{
                  color: notifPerm === "authorized" ? C.ok : C.danger,
                  fontWeight: "700",
                  fontSize: 12,
                }}
              >
                {notifPerm === "authorized" ? "Aktif" : "Belum diberikan"}
              </Text>
            </View>
            {notifPerm !== "authorized" && (
              <Button
                label="Beri akses notifikasi"
                onPress={() => NotifListener?.requestPermission()}
              />
            )}
            <PressableScale
              onPress={() => setShowDebug((v) => !v)}
              style={st.link}
            >
              <Text style={st.linkTxt}>
                {showDebug ? "Sembunyikan debug" : "Lihat debug notifikasi"}
              </Text>
            </PressableScale>
            <Collapsible open={showDebug}>
              <Text style={st.lbl}>Diagnosa</Text>
              <Text style={st.mono}>
                {`notif diterima (semua app): ${nav.totalSeen}\nterakhir dari : ${nav.lastApp}\nnotif Maps lolos parse: ${nav.count}\nstatus terakhir: ${nav.lastResult}`}
              </Text>
              <Text style={[st.lbl, { marginTop: S.md }]}>
                Notifikasi Maps terakhir
              </Text>
              <Text style={st.mono}>{rawText}</Text>
              <Text style={[st.lbl, { marginTop: S.md }]}>Hasil parsing</Text>
              <Text style={st.mono}>
                {nav.lastInfo
                  ? `arrow : ${nav.lastInfo.arrow}\njarak : ${nav.lastInfo.distance}\njalan : ${nav.lastInfo.street}\neta   : ${nav.lastInfo.eta}`
                  : "-"}
              </Text>
            </Collapsible>
          </Card>
        )}

        {/* KECEPATAN */}
        {Platform.OS === "android" && (
          <Card title="Kecepatan GPS" index={4}>
            <View style={st.between}>
              <Text style={T.body}>Tampilkan di layar navigasi</Text>
              <Switch
                value={speed.running}
                disabled={speedBusy}
                onValueChange={toggleSpeed}
                trackColor={{ true: C.accentDim, false: C.surface2 }}
                thumbColor={speed.running ? C.accent : C.sub}
              />
            </View>
            {speed.running && (
              <View style={st.tile}>
                <Text style={T.small}>Kecepatan saat ini</Text>
                {speed.kmh != null ? (
                  <Animated.Text
                    key="spd"
                    entering={FadeIn.duration(200)}
                    style={[T.big, { color: C.accent }]}
                  >
                    {speed.kmh}
                    <Text style={st.unit}> km/h</Text>
                  </Animated.Text>
                ) : (
                  <Skeleton w={110} h={38} style={{ marginVertical: 4 }} />
                )}
                <Text style={T.small}>
                  {speed.accuracy != null
                    ? `Akurasi GPS ±${Math.round(speed.accuracy)} m`
                    : "Menunggu sinyal GPS..."}
                </Text>
              </View>
            )}
            {speed.error ? (
              <Text style={{ color: C.danger, fontSize: 12 }}>
                {speed.error}
              </Text>
            ) : null}
            <Text style={T.small}>
              Nyalakan saat app terbuka. Notifikasi "Kecepatan GPS aktif" akan
              muncul — itu yang menjaga GPS tetap jalan saat Google Maps di
              layar depan. Izin lokasi biasa sudah cukup.
            </Text>
          </Card>
        )}

        {/* MANUAL */}
        <Card title="Tes navigasi manual" index={5}>
          <PressableScale
            onPress={() => setShowManual((v) => !v)}
            style={st.link}
          >
            <Text style={st.linkTxt}>
              {showManual ? "Tutup" : "Buka form tes"}
            </Text>
          </PressableScale>
          <Collapsible open={showManual}>
            <View style={{ gap: S.md }}>
              <ScrollView horizontal showsHorizontalScrollIndicator={false}>
                {ARROWS.map((a) => (
                  <PressableScale
                    key={a}
                    onPress={() => setArrow(a)}
                    style={[st.chip, arrow === a && st.chipOn]}
                  >
                    <Text
                      style={[st.chipTxt, arrow === a && { color: "#04131f" }]}
                    >
                      {a}
                    </Text>
                  </PressableScale>
                ))}
              </ScrollView>
              <TextInput
                style={st.input}
                value={distance}
                onChangeText={setDistance}
                placeholder="150 m"
                placeholderTextColor={C.sub}
              />
              <TextInput
                style={st.input}
                value={street}
                onChangeText={setStreet}
                placeholder="Nama jalan"
                placeholderTextColor={C.sub}
              />
              <TextInput
                style={st.input}
                value={eta}
                onChangeText={setEta}
                placeholder="14:32"
                placeholderTextColor={C.sub}
              />
              <Button label="Kirim navigasi" onPress={sendNav} />
            </View>
          </Collapsible>
        </Card>

        {/* TIMER */}
        <Card title="Zona hati-hati" index={6}>
          <Button
            label={confirmReset ? "Yakin? Tekan lagi" : "Reset timer mesin"}
            variant="danger"
            onPress={onReset}
          />
        </Card>

        <Text style={[T.small, { textAlign: "center", lineHeight: 18 }]}>
          Tips: set baterai neotechApp ke "Tidak dibatasi" agar koneksi BLE
          tidak diputus Android saat layar mati.
        </Text>
      </ScrollView>
      {toast}
    </View>
  );
}

export default function App() {
  return (
    <ErrorBoundary>
      <Screen />
    </ErrorBoundary>
  );
}

const st = StyleSheet.create({
  header: { flexDirection: "row", alignItems: "center", marginBottom: S.xl },
  row: { flexDirection: "row", gap: S.md },
  between: {
    flexDirection: "row",
    justifyContent: "space-between",
    alignItems: "center",
  },
  tile: {
    flex: 1,
    backgroundColor: C.surface2,
    borderRadius: R.md,
    padding: S.md,
    gap: 2,
    minHeight: 104,
  },
  unit: { fontSize: 16, color: C.sub, fontWeight: "600" },
  badge: { fontSize: 12, fontWeight: "700" },
  empty: {
    backgroundColor: C.surface2,
    borderRadius: R.md,
    padding: S.lg,
    gap: S.xs,
  },
  oled: {
    backgroundColor: C.oled,
    borderRadius: R.md,
    borderWidth: 1,
    borderColor: C.border,
    paddingVertical: S.lg,
    paddingHorizontal: S.lg,
    alignItems: "center",
    gap: 6,
  },
  oledName: {
    color: C.oledText,
    fontSize: 24,
    fontWeight: "800",
    maxWidth: "100%",
  },
  oledLine: {
    color: C.oledText,
    fontSize: 14,
    opacity: 0.85,
    maxWidth: "100%",
  },
  lbl: { color: C.sub, fontSize: 12, marginBottom: 6, fontWeight: "600" },
  count: { color: C.sub, fontSize: 11, textAlign: "right", marginTop: 4 },
  input: {
    backgroundColor: C.surface2,
    color: C.text,
    borderRadius: R.md,
    paddingHorizontal: S.md,
    height: 48,
    fontSize: 15,
  },
  mono: {
    color: C.text,
    fontSize: 12,
    fontFamily: Platform.OS === "android" ? "monospace" : "Menlo",
  },
  link: { alignSelf: "flex-start", paddingVertical: 4 },
  linkTxt: { color: C.accent, fontWeight: "700", fontSize: 13 },
  chip: {
    paddingHorizontal: S.md,
    height: 36,
    justifyContent: "center",
    backgroundColor: C.surface2,
    borderRadius: R.pill,
    marginRight: S.sm,
  },
  chipOn: { backgroundColor: C.accent },
  chipTxt: { color: C.sub, fontSize: 12, fontWeight: "700" },
});
