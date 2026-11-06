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
import { KL_ERRORS, KeylessPanel } from "../keyless/KeylessPanel";
import { AppSlider } from "../ui/slider";
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

/** profil beacon Find Hub saat parkir (harus sama dengan FIND_PROF di firmware) */
const FIND_PROFILES = [
  { name: "Ketat", desc: "Beacon 8 dtk tiap ±1 mnt · tambahan arus ±7 mA" },
  { name: "Seimbang", desc: "Beacon 10 dtk tiap ±5 mnt · tambahan arus ±2 mA" },
  { name: "Hemat", desc: "Beacon 10 dtk tiap ±15 mnt · tambahan arus ±0,6 mA" },
] as const;
const PARK_OPTIONS = [1, 3, 10] as const; // menit
const BRIGHT_PRESETS = [
  { name: "Redup", v: 25 },
  { name: "Sedang", v: 60 },
  { name: "Terang", v: 100 },
] as const;
const FW_ERRORS: Record<string, string> = {
  pin: "PIN salah",
  pinfmt: "PIN baru harus 4–8 digit angka",
  key: "Kunci tidak valid (harus 40 karakter hex)",
  nokey: "Pasang kunci dulu sebelum mengaktifkan",
};

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

  // kecerahan layar
  const [bright, setBright] = useState(100);
  const brightDirty = useRef(false);
  const lastBrightSend = useRef(0);

  // pelacak Find Hub
  const [pin, setPin] = useState("");
  const [keyText, setKeyText] = useState("");
  const [profSel, setProfSel] = useState(1);
  const [findBusy, setFindBusy] = useState(false);
  const [showFindHelp, setShowFindHelp] = useState(false);
  const [confirmClear, setConfirmClear] = useState(false);

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

  // sinkronkan pengaturan yang tersimpan di ESP32 (sekali, selama user belum mengubahnya)
  useEffect(() => {
    if (!brightDirty.current && ble.bright != null) setBright(ble.bright);
  }, [ble.bright]);
  useEffect(() => {
    if (ble.findProf != null) setProfSel(ble.findProf);
  }, [ble.findProf]);
  useEffect(() => {
    const e = ble.fwError;
    if (e)
      show(
        FW_ERRORS[e.code] ?? KL_ERRORS[e.code] ?? `Galat dari ESP32: ${e.code}`,
        "err",
      );
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [ble.fwError?.at]);
  useEffect(() => {
    if (!confirmClear) return;
    const t = setTimeout(() => setConfirmClear(false), 3500);
    return () => clearTimeout(t);
  }, [confirmClear]);

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

  // kecerahan: kirim pelan-pelan saat digeser (tanpa menyimpan), simpan ke flash saat dilepas
  const onBrightDrag = (v: number) => {
    brightDirty.current = true;
    setBright(v);
    const now = Date.now();
    if (now - lastBrightSend.current >= 90) {
      lastBrightSend.current = now;
      void sendJson({ cmd: "bright", val: v });
    }
  };
  const onBrightCommit = async (v: number) => {
    setBright(v);
    await send({ cmd: "bright", val: v, save: 1 }, "Kecerahan tersimpan ✓");
    brightDirty.current = false;
  };

  const setParkMin = (i: number) =>
    send({ cmd: "park", min: PARK_OPTIONS[i] }, "Waktu tidur diatur");

  // ---- Find Hub ----
  const cleanKey = keyText.replace(/[^0-9a-fA-F]/g, "");
  const keyValid = cleanKey.length === 40;
  const pinValid = /^\d{4,8}$/.test(pin);

  const installKey = async () => {
    if (!keyValid) return show("Kunci harus 40 karakter hex (0-9, a-f)", "err");
    if (!pinValid)
      return show(
        ble.pinSet ? "Masukkan PIN pelacak" : "Buat PIN 4–8 digit angka",
        "err",
      );
    setFindBusy(true);
    const ok = await send(
      { cmd: "find_key", key: cleanKey.toLowerCase(), pin },
      "Kunci dipasang ✓ — sekarang aktifkan pelacak",
    );
    if (ok) setKeyText("");
    setFindBusy(false);
  };
  const toggleFind = async (on: boolean) => {
    if (!pinValid) return show("Masukkan PIN pelacak dulu", "err");
    setFindBusy(true);
    await send(
      { cmd: "find", en: on ? 1 : 0, prof: profSel, pin },
      on ? "Pelacak diaktifkan" : "Pelacak dimatikan",
    );
    setFindBusy(false);
  };
  const changeProfile = (i: number) => {
    setProfSel(i);
    if (ble.findKey && pinValid)
      send(
        { cmd: "find", en: ble.findOn ? 1 : 0, prof: i, pin },
        "Profil diubah",
      );
    else if (ble.findKey)
      show("Masukkan PIN agar profil tersimpan di ESP32", "info");
  };
  const clearFind = () => {
    if (!pinValid) return show("Masukkan PIN pelacak dulu", "err");
    if (!confirmClear) {
      setConfirmClear(true);
      show("Tekan sekali lagi untuk menghapus kunci", "info");
      return;
    }
    setConfirmClear(false);
    send({ cmd: "find_clear", pin }, "Kunci pelacak dihapus");
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

        {/* KEYLESS */}
        <KeylessPanel ble={ble} show={show} connected={connected} />

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

        {/* KECERAHAN */}
        <Card
          title="Kecerahan layar"
          index={2}
          right={
            <Text style={[T.small, { fontWeight: "700", color: C.accent }]}>
              {bright}%
            </Text>
          }
        >
          <AppSlider
            value={bright}
            min={5}
            max={100}
            disabled={!connected}
            onChange={onBrightDrag}
            onCommit={onBrightCommit}
          />
          <View style={st.row}>
            {BRIGHT_PRESETS.map((p) => (
              <Button
                key={p.name}
                label={p.name}
                variant={bright === p.v ? "primary" : "ghost"}
                disabled={!connected}
                onPress={() => onBrightCommit(p.v)}
                style={{ flex: 1 }}
              />
            ))}
          </View>
          <Text style={T.small}>
            Tersimpan di ESP32, jadi tetap berlaku setiap motor dinyalakan.
            Layar lebih redup = lebih hemat daya dan nyaman di malam hari.
          </Text>
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

        {/* FIND HUB */}
        <Card
          title="Pelacak Find Hub"
          index={5}
          right={
            <Text
              style={[
                T.small,
                { fontWeight: "700", color: ble.findOn ? C.ok : C.sub },
              ]}
            >
              {ble.findKey == null
                ? "—"
                : ble.findOn
                  ? "AKTIF"
                  : ble.findKey
                    ? "MATI"
                    : "BELUM ADA KUNCI"}
            </Text>
          }
        >
          <Text style={T.small}>
            ESP32 memancarkan beacon BLE yang dikenali jaringan Find Hub. HP
            Android di sekitar motor yang sedang lewat akan melaporkan lokasinya
            (terenkripsi) ke akun Google Anda. Beacon hanya dipancarkan saat HP
            Anda tidak terhubung ke display.
          </Text>

          <View>
            <Text style={st.lbl}>
              PIN pelacak {ble.pinSet === false ? "(buat baru, 4–8 angka)" : ""}
            </Text>
            <TextInput
              style={st.input}
              value={pin}
              onChangeText={(v) => setPin(v.replace(/\D/g, "").slice(0, 8))}
              keyboardType="number-pad"
              secureTextEntry
              placeholder="••••"
              placeholderTextColor={C.sub}
            />
          </View>

          <View style={st.between}>
            <Text style={T.body}>Aktifkan pelacak</Text>
            <Switch
              value={!!ble.findOn}
              disabled={!connected || findBusy || !ble.findKey}
              onValueChange={toggleFind}
              trackColor={{ true: C.accentDim, false: C.surface2 }}
              thumbColor={ble.findOn ? C.accent : C.sub}
            />
          </View>

          <View>
            <Text style={st.lbl}>
              Seberapa sering beacon dipancarkan saat parkir
            </Text>
            <Segmented
              options={FIND_PROFILES.map((p) => p.name)}
              value={profSel}
              onChange={changeProfile}
            />
            <Text style={[T.small, { marginTop: S.sm }]}>
              {FIND_PROFILES[profSel].desc}
            </Text>
          </View>

          <View>
            <Text style={st.lbl}>Kunci (advertisement key, 40 hex)</Text>
            <TextInput
              style={st.input}
              value={keyText}
              onChangeText={setKeyText}
              autoCapitalize="none"
              autoCorrect={false}
              placeholder={
                ble.findKey
                  ? "Kunci sudah terpasang — tempel untuk mengganti"
                  : "tempel dari skrip main.py"
              }
              placeholderTextColor={C.sub}
            />
            <Text style={st.count}>{cleanKey.length}/40</Text>
          </View>
          <View style={st.row}>
            <Button
              label={confirmClear ? "Yakin? Tekan lagi" : "Hapus kunci"}
              variant="danger"
              disabled={!connected || !ble.findKey}
              onPress={clearFind}
              style={{ flex: 1 }}
            />
            <Button
              label="Pasang kunci"
              loading={findBusy}
              disabled={!connected || !keyValid}
              onPress={installKey}
              style={{ flex: 2 }}
            />
          </View>

          <PressableScale
            onPress={() => setShowFindHelp((v) => !v)}
            style={st.link}
          >
            <Text style={st.linkTxt}>
              {showFindHelp ? "Tutup panduan" : "Cara mendapatkan kunci"}
            </Text>
          </PressableScale>
          <Collapsible open={showFindHelp}>
            <Text style={st.mono}>
              {`1. Di laptop: pasang Python + Chrome terbaru, lalu jalankan skrip main.py dari proyek GoogleFindMyTools.\n2. Login Google saat Chrome terbuka, tekan "r" (register tracker), salin advertisement key (40 karakter hex).\n3. Tempel di kolom kunci, isi PIN, tekan "Pasang kunci", lalu nyalakan saklar.\n4. Ulangi main.py secara berkala agar pendaftaran tidak kedaluwarsa. Lokasi dilihat lewat skrip itu.\n\nEksperimental & tidak resmi dari Google — lihat docs/find-hub.md.`}
            </Text>
          </Collapsible>
        </Card>

        {/* HEMAT DAYA */}
        <Card title="Hemat daya" index={6}>
          <Text style={st.lbl}>Display tidur setelah mesin mati</Text>
          <Segmented
            options={PARK_OPTIONS.map((m) => `${m} mnt`)}
            value={Math.max(
              0,
              PARK_OPTIONS.indexOf((ble.parkMin ?? 3) as 1 | 3 | 10),
            )}
            onChange={setParkMin}
          />
          <Text style={T.small}>
            Saat tidur, layar mati dan koneksi app terputus; ESP32 bangun tiap
            ±10 detik untuk memeriksa mesin. Nyalakan mesin (atau tekan tombol
            BOOT) untuk membangunkannya. Pelacak Find Hub tetap bekerja selama
            tegangan aki di atas 11,8 V.
          </Text>
        </Card>

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
