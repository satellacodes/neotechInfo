import React, { useEffect, useRef, useState } from "react";
import { StyleSheet, Switch, Text, View } from "react-native";

import { BleSnapshot, getBleSnapshot, sendJson, wakeAndConnect } from "../ble/ble";
import { Button, Card, Collapsible, PressableScale, Segmented, ToastKind } from "../ui/components";
import { C, R, S, T } from "../ui/theme";
import {
  disableFindNotif,
  enableFindNotif,
  findNotifAvailable,
  isFindNotifShown,
  listenFindNotif,
} from "./findNotif";

/** pesan galat dari firmware (frame "E|kode") untuk fitur keyless */
export const KL_ERRORS: Record<string, string> = {
  far: "HP terlalu jauh dari motor (coba dekat dulu, atau kalibrasi jarak)",
  mode: "Aktifkan mode Keyless dulu",
  busy: "Sedang sibuk, coba lagi sebentar",
  running: "Mesin sudah hidup",
  cool: "Starter istirahat 30 detik (terlalu banyak percobaan)",
  lowbat: "Aki terlalu lemah untuk starter",
  gear: "Masukkan gigi netral (N) dulu",
  moving: "Motor sedang melaju: mesin tidak dimatikan dari jauh",
  link: "Starter dihentikan: koneksi putus",
  nopair: "Firmware dibuild tanpa pairing: kendali relay dinonaktifkan",
};

function noticeText(code: string): { msg: string; kind: ToastKind } | null {
  if (code.startsWith("calib:")) return { msg: `Jarak 5 m tersimpan (ambang ${code.slice(6)} dBm)`, kind: "ok" };
  switch (code) {
    case "start_ok": return { msg: "Mesin menyala ✓", kind: "ok" };
    case "start_end": return { msg: "Starter selesai — cek apakah mesin hidup", kind: "info" };
    case "starting": return { msg: "Menyalakan kontak & starter…", kind: "info" };
    case "cranking": return { msg: "Starter…", kind: "info" };
    case "ign_on": return { msg: "Kontak ON", kind: "ok" };
    case "locked": return { msg: "Motor terkunci", kind: "ok" };
    case "locked_far": return { msg: "Terkunci otomatis: HP menjauh", kind: "info" };
    case "autooff": return { msg: "Kontak dimatikan otomatis (mesin tidak dihidupkan)", kind: "info" };
    case "killing": return { msg: "Mesin akan dimatikan 5 detik lagi — tekan BATAL untuk membatalkan", kind: "err" };
    case "killed": return { msg: "Mesin dimatikan & motor terkunci", kind: "ok" };
    case "cancel": return { msg: "Dibatalkan", kind: "info" };
    case "tamper": return { msg: "⚠ ALARM: kunci/kontak dipakai tanpa izin!", kind: "err" };
    case "paired": return { msg: "Pairing berhasil ✓", kind: "ok" };
    case "pair_open": return { msg: "Mode pairing dibuka 2 menit — lihat PIN di OLED", kind: "info" };
    case "calibrating": return { msg: "Kalibrasi: tetap di jarak 5 m selama 6 detik", kind: "info" };
    case "mode_kl": return { msg: "Mode Keyless aktif", kind: "ok" };
    case "mode_man": return { msg: "Mode Manual (kunci asli)", kind: "ok" };
    case "cool": return { msg: "Starter istirahat 30 detik", kind: "err" };
    default: return null;
  }
}

const WAKE_OPTS = ["Sigap", "Normal", "Hemat"] as const;
const WAKE_DESC = [
  "ESP32 mengintip HP tiap 2 dtk · tersambung ±3 dtk · boros (±10 mA tambahan)",
  "Tiap 4 dtk · tersambung ±5 dtk · seimbang (±4 mA tambahan)",
  "Tiap 8 dtk · tersambung ±9 dtk · paling hemat (±2 mA tambahan)",
];
const LOCK_OPTS = ["30 dtk", "2 mnt", "5 mnt", "Mati"] as const;
const LOCK_DESC = [
  "Kunci otomatis jika HP hilang ≥ 30 dtk dan mesin tidak sedang digas",
  "Kunci otomatis jika HP hilang ≥ 2 mnt dan mesin tidak sedang digas",
  "Kunci otomatis jika HP hilang ≥ 5 mnt dan mesin tidak sedang digas",
  "Tidak pernah mengunci otomatis (hanya peringatan bunyi)",
];

const ST_LABEL = ["TERKUNCI", "KONTAK ON", "STARTER…", "MESIN HIDUP", "AKAN DIMATIKAN", "MENCARI…"];
const ST_COLOR = [C.sub, C.warn, C.warn, C.ok, C.danger, C.accent];

export function KeylessPanel({
  ble,
  show,
  connected,
}: {
  ble: BleSnapshot;
  show: (msg: string, kind?: ToastKind) => void;
  connected: boolean;
}) {
  const kl = ble.kl;
  const [busy, setBusy] = useState(false);
  const [showCfg, setShowCfg] = useState(false);
  const [confirmKill, setConfirmKill] = useState(false);
  const [notifOn, setNotifOn] = useState(false);

  // ---- umpan balik kejadian dari firmware ----
  useEffect(() => {
    const n = ble.fwNotice;
    if (!n) return;
    const t = noticeText(n.code);
    if (t) show(t.msg, t.kind);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [ble.fwNotice?.at]);

  useEffect(() => {
    if (!confirmKill) return;
    const t = setTimeout(() => setConfirmKill(false), 3500);
    return () => clearTimeout(t);
  }, [confirmKill]);

  // ---- kirim perintah; bangunkan ESP32 bila tidur ----
  const act = async (obj: object, okMsg?: string, needNear = false): Promise<boolean> => {
    setBusy(true);
    try {
      if (!getBleSnapshot().kl || getBleSnapshot().link !== "connected") {
        show("Membangunkan ESP32…", "info");
        if (!(await wakeAndConnect())) {
          show("ESP32 tidak ditemukan. Dekati motor / periksa Bluetooth", "err");
          return false;
        }
      }
      if (needNear) {
        // RSSI baru stabil ±1 dtk setelah tersambung
        const t0 = Date.now();
        while (Date.now() - t0 < 4000 && !getBleSnapshot().kl?.near) {
          await new Promise<void>((r) => setTimeout(r, 250));
        }
      }
      const ok = await sendJson(obj);
      if (!ok) show("Gagal mengirim. Jika baru pairing: ketik PIN dari layar OLED", "err");
      else if (okMsg) show(okMsg, "ok");
      return ok;
    } finally {
      setBusy(false);
    }
  };

  // ---- notifikasi "Cari motor" ----
  const findBike = () => act({ cmd: "kl_find" }, "Mencari motor…");
  const findRef = useRef(findBike);
  findRef.current = findBike;
  useEffect(() => {
    void isFindNotifShown().then(setNotifOn);
    return listenFindNotif(() => void findRef.current());
  }, []);
  const toggleNotif = async (on: boolean) => {
    if (on) {
      const ok = await enableFindNotif();
      setNotifOn(ok);
      show(ok ? "Notifikasi 'Cari motor' dipasang" : "Izin notifikasi ditolak / paket belum terpasang", ok ? "ok" : "err");
    } else {
      await disableFindNotif();
      setNotifOn(false);
    }
  };

  const keyless = kl?.mode === 1;
  const st = kl?.st ?? 0;
  const killing = st === 4;

  const onKill = () => {
    if (!confirmKill) {
      setConfirmKill(true);
      show("Tekan sekali lagi untuk mematikan mesin", "info");
      return;
    }
    setConfirmKill(false);
    void act({ cmd: "kl_kill" });
  };

  return (
    <>
      <Card
        title="Keyless"
        index={0}
        right={
          <Text style={[T.small, { fontWeight: "700", color: kl ? ST_COLOR[st] ?? C.sub : C.sub }]}>
            {kl ? (keyless ? ST_LABEL[st] ?? "—" : "MODE MANUAL") : ble.sleeping ? "ESP TIDUR" : "—"}
          </Text>
        }
      >
        {ble.needPin ? (
          <View style={st2.banner}>
            <Text style={st2.bannerTxt}>
              Dialog Bluetooth Android muncul? Ketik PIN 6 digit yang tampil di layar OLED motor.
            </Text>
          </View>
        ) : null}

        {kl && kl.tamper > 0 ? (
          <View style={[st2.banner, { borderColor: C.danger }]}>
            <Text style={[st2.bannerTxt, { color: C.danger }]}>
              ⚠ {kl.tamper}× kunci/kontak dipakai tanpa izin saat terkunci.
            </Text>
            <PressableScale onPress={() => void act({ cmd: "kl_ack" })} style={st2.link}>
              <Text style={st2.linkTxt}>Tandai sudah dibaca</Text>
            </PressableScale>
          </View>
        ) : null}

        {!kl ? (
          <Text style={T.small}>
            {ble.sleeping
              ? "ESP32 tidur untuk hemat daya. Tekan tombol apa pun di bawah untuk membangunkannya (butuh ±3–9 dtk)."
              : connected
                ? "Menunggu status dari ESP32…"
                : "Belum terhubung. Dekati motor lalu tekan tombol di bawah."}
          </Text>
        ) : (
          <View style={st2.chips}>
            <Chip label="Jarak" value={kl.near ? "Dekat" : connected ? "Jauh" : "—"} ok={kl.near} sub={kl.rssi ? `${kl.rssi} dBm` : ""} />
            <Chip
              label="Gigi"
              value={kl.neutral === 1 ? "N" : kl.neutral === 0 ? "Gigi" : "—"}
              ok={kl.neutral === 1}
              sub={kl.neutral === 2 ? "kontak mati" : ""}
            />
            <Chip label="Mesin" value={kl.run ? "Hidup" : "Mati"} ok={kl.run} sub={kl.cdi ? "CDI ON" : "CDI putus"} />
          </View>
        )}

        <View>
          <Text style={st2.lbl}>Mode kendaraan</Text>
          <Segmented
            options={["Manual (kunci)", "Keyless"]}
            value={kl?.mode === 1 ? 1 : 0}
            onChange={(i) => void act({ cmd: "kl_mode", val: i }, undefined)}
          />
          <Text style={[T.small, { marginTop: S.sm }]}>
            {keyless
              ? "CDI terputus saat terkunci. Kunci asli JANGAN diputar (akan memicu alarm). Mesin hanya bisa hidup dari app saat HP dekat."
              : "Kunci asli bekerja seperti biasa; ESP32 hanya menutup relay CDI selama kunci ON."}
          </Text>
        </View>

        {keyless ? (
          killing ? (
            <Button label="BATAL — jangan matikan mesin" variant="primary" onPress={() => void act({ cmd: "kl_cancel" })} />
          ) : (
            <>
              <Button
                label={st === 2 ? "Starter berjalan… (tekan untuk berhenti)" : "START MESIN"}
                loading={busy && st !== 2}
                disabled={st === 3}
                onPress={() => (st === 2 ? void act({ cmd: "kl_stop" }) : void act({ cmd: "kl_start" }, undefined, true))}
              />
              <View style={st2.row}>
                <Button
                  label={kl?.ign ? "Kunci motor" : "Kontak ON"}
                  variant="ghost"
                  disabled={busy || st === 2}
                  onPress={() =>
                    kl?.ign ? void act({ cmd: "kl_ign", en: 0 }) : void act({ cmd: "kl_ign", en: 1 }, undefined, true)
                  }
                  style={{ flex: 1 }}
                />
                <Button
                  label={confirmKill ? "Yakin? Tekan lagi" : "Matikan mesin"}
                  variant="danger"
                  disabled={busy || st === 2 || !kl?.ign}
                  onPress={onKill}
                  style={{ flex: 1 }}
                />
              </View>
            </>
          )
        ) : null}

        <View style={st2.row}>
          <Button label="Cari motor" variant="ghost" disabled={busy} onPress={() => void findBike()} style={{ flex: 1 }} />
          <Button
            label={kl?.calib ? "Mengukur…" : "Kalibrasi 5 m"}
            variant="ghost"
            disabled={busy || kl?.calib}
            onPress={() => void act({ cmd: "kl_calib" })}
            style={{ flex: 1 }}
          />
        </View>
        <Text style={T.small}>
          Kalibrasi: berdiri tepat 5 m dari motor (HP di saku seperti biasa), tekan, tunggu 6 detik. Ambang jarak
          tersimpan di ESP32. Sinyal Bluetooth berubah oleh tubuh, dinding & kendaraan — hasilnya perkiraan, bukan meteran.
        </Text>

        {findNotifAvailable ? (
          <View style={st2.between}>
            <Text style={T.body}>Notifikasi "Cari motor"</Text>
            <Switch
              value={notifOn}
              onValueChange={toggleNotif}
              trackColor={{ true: C.accentDim, false: C.surface2 }}
              thumbColor={notifOn ? C.accent : C.sub}
            />
          </View>
        ) : (
          <Text style={T.small}>
            Notifikasi "Cari motor" butuh paket expo-notifications (npx expo install expo-notifications), lalu build ulang.
          </Text>
        )}

        <PressableScale onPress={() => setShowCfg((v) => !v)} style={st2.link}>
          <Text style={st2.linkTxt}>{showCfg ? "Tutup pengaturan keyless" : "Pengaturan keyless & keamanan"}</Text>
        </PressableScale>
        <Collapsible open={showCfg}>
          <View style={{ gap: S.md }}>
            <View>
              <Text style={st2.lbl}>Kecepatan ESP32 menyahut HP saat parkir (hemat aki)</Text>
              <Segmented
                options={[...WAKE_OPTS]}
                value={kl?.wake ?? 1}
                onChange={(i) => void act({ cmd: "kl_cfg", wp: i })}
              />
              <Text style={[T.small, { marginTop: S.sm }]}>{WAKE_DESC[kl?.wake ?? 1]} · angka perkiraan</Text>
            </View>
            <View>
              <Text style={st2.lbl}>Kunci otomatis bila HP menjauh saat kontak ON</Text>
              <Segmented
                options={[...LOCK_OPTS]}
                value={kl?.lock ?? 1}
                onChange={(i) => void act({ cmd: "kl_cfg", lk: i })}
              />
              <Text style={[T.small, { marginTop: S.sm }]}>
                {LOCK_DESC[kl?.lock ?? 1]}. Mesin yang sedang jalan TIDAK dimatikan seketika saat HP menjauh. Saat terkunci, riting kedip ala pesawat + 1 bip.
              </Text>
            </View>
            <Row
              label="Starter wajib gigi netral"
              value={kl?.nr ?? true}
              onChange={(v) => void act({ cmd: "kl_cfg", nr: v ? 1 : 0 })}
            />
            <Row
              label="Kontak mati sendiri bila mesin tak dihidupkan (3 mnt)"
              value={kl?.ao ?? true}
              onChange={(v) => void act({ cmd: "kl_cfg", ao: v ? 1 : 0 })}
            />
            <Row
              label="Kedip riting pesawat + bip bila HP menjauh saat kontak ON"
              value={kl?.wf ?? true}
              onChange={(v) => void act({ cmd: "kl_cfg", wf: v ? 1 : 0 })}
            />
            <Button
              label={kl?.pair ? "Mode pairing terbuka (lihat OLED)" : "Pasangkan HP lain (buka pairing 2 mnt)"}
              variant="ghost"
              disabled={busy}
              onPress={() => void act({ cmd: "kl_pair" })}
            />
            <Text style={T.small}>
              Mencabut HP yang hilang: tahan tombol BOOT di ESP32 selama 3 detik — semua HP lama dihapus dan pairing
              dibuka. Lalu hapus ("forget") perangkat lama di pengaturan Bluetooth Android.
            </Text>
          </View>
        </Collapsible>
      </Card>
    </>
  );
}

function Chip({ label, value, ok, sub }: { label: string; value: string; ok: boolean; sub?: string }) {
  return (
    <View style={st2.chip}>
      <Text style={T.small}>{label}</Text>
      <Text style={[st2.chipVal, { color: ok ? C.ok : C.text }]}>{value}</Text>
      {sub ? <Text style={st2.chipSub}>{sub}</Text> : <Text style={st2.chipSub}> </Text>}
    </View>
  );
}

function Row({ label, value, onChange }: { label: string; value: boolean; onChange: (v: boolean) => void }) {
  return (
    <View style={st2.between}>
      <Text style={[T.body, { flex: 1, paddingRight: S.md }]}>{label}</Text>
      <Switch
        value={value}
        onValueChange={onChange}
        trackColor={{ true: C.accentDim, false: C.surface2 }}
        thumbColor={value ? C.accent : C.sub}
      />
    </View>
  );
}

const st2 = StyleSheet.create({
  row: { flexDirection: "row", gap: S.md },
  between: { flexDirection: "row", justifyContent: "space-between", alignItems: "center" },
  lbl: { color: C.sub, fontSize: 12, marginBottom: 6, fontWeight: "600" },
  chips: { flexDirection: "row", gap: S.sm },
  chip: { flex: 1, backgroundColor: C.surface2, borderRadius: R.md, padding: S.md, gap: 2 },
  chipVal: { fontSize: 18, fontWeight: "800" },
  chipSub: { color: C.sub, fontSize: 11 },
  banner: { borderWidth: 1, borderColor: C.warn, borderRadius: R.md, padding: S.md, gap: S.xs },
  bannerTxt: { color: C.text, fontSize: 13 },
  link: { alignSelf: "flex-start", paddingVertical: 4 },
  linkTxt: { color: C.accent, fontWeight: "700", fontSize: 13 },
});
