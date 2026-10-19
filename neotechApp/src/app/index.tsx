import React, { useCallback, useEffect, useRef, useState } from "react";
import {
  Alert,
  PermissionsAndroid,
  Platform,
  ScrollView,
  StyleSheet,
  Text,
  TextInput,
  TouchableOpacity,
  View,
  StatusBar,
} from "react-native";
import { BleManager, Device, State, Subscription } from "react-native-ble-plx";
import { Buffer } from "buffer";

const SVC = "6E400001-B5A3-F393-E0A9-E50E24DCCA9E";
const RX = "6E400002-B5A3-F393-E0A9-E50E24DCCA9E";
const DEVICE_NAME = "GLPro-Neotech";
const SCAN_TIMEOUT_MS = 12000;

async function requestBlePermissions(): Promise<boolean> {
  if (Platform.OS !== "android") return true;
  const res = await PermissionsAndroid.requestMultiple([
    PermissionsAndroid.PERMISSIONS.BLUETOOTH_SCAN,
    PermissionsAndroid.PERMISSIONS.BLUETOOTH_CONNECT,
    PermissionsAndroid.PERMISSIONS.ACCESS_FINE_LOCATION,
  ]);
  return Object.values(res).every(
    (r) => r === PermissionsAndroid.RESULTS.GRANTED,
  );
}

export default function App() {
  const mgrRef = useRef<BleManager | null>(null);
  if (!mgrRef.current) mgrRef.current = new BleManager();
  const manager = mgrRef.current;

  const scanTimer = useRef<ReturnType<typeof setTimeout> | null>(null);
  const discSub = useRef<Subscription | null>(null);

  const [dev, setDev] = useState<Device | null>(null);
  const [connected, setConnected] = useState(false);
  const [scanning, setScanning] = useState(false);
  const [mode, setMode] = useState(0);

  const [arrow, setArrow] = useState("straight");
  const [distance, setDistance] = useState("150m");
  const [street, setStreet] = useState("Jl. Sudirman");
  const [eta, setEta] = useState("14:32");

  const stopScan = useCallback(() => {
    manager.stopDeviceScan();
    if (scanTimer.current) clearTimeout(scanTimer.current);
    scanTimer.current = null;
    setScanning(false);
  }, [manager]);

  useEffect(() => {
    return () => {
      if (scanTimer.current) clearTimeout(scanTimer.current);
      discSub.current?.remove();
      manager.destroy();
    };
  }, [manager]);

  const connect = async () => {
    if (scanning || connected) return;

    if (!(await requestBlePermissions())) {
      Alert.alert("Izin ditolak", "Izin Bluetooth/Lokasi diperlukan.");
      return;
    }
    if ((await manager.state()) !== State.PoweredOn) {
      Alert.alert("Bluetooth mati", "Nyalakan Bluetooth dulu.");
      return;
    }

    setScanning(true);
    manager.startDeviceScan(null, null, async (err, d) => {
      if (err) {
        console.warn(err);
        stopScan();
        return;
      }
      if (d?.name !== DEVICE_NAME) return;

      stopScan();
      try {
        const c = await d.connect();
        await c.discoverAllServicesAndCharacteristics();
        discSub.current?.remove();
        discSub.current = c.onDisconnected(() => {
          setConnected(false);
          setDev(null);
        });
        setDev(c);
        setConnected(true);
      } catch (e) {
        Alert.alert("Gagal konek", String(e));
      }
    });

    scanTimer.current = setTimeout(stopScan, SCAN_TIMEOUT_MS);
  };

  const sendJson = async (obj: object) => {
    if (!dev) return;
    const b64 = Buffer.from(JSON.stringify(obj)).toString("base64");
    try {
      await dev.writeCharacteristicWithResponseForService(SVC, RX, b64);
    } catch (e) {
      console.warn("TX", e);
    }
  };

  const setModeAndSend = (m: number) => {
    setMode(m);
    sendJson({ cmd: "mode", val: m });
  };

  const sendNav = () => {
    sendJson({ cmd: "nav", arrow, distance, street, eta });
    setMode(2);
  };

  return (
    <ScrollView contentContainerStyle={s.root}>
      <StatusBar barStyle="light-content" />
      <Text style={s.title}>GL Pro Smart Display</Text>

      <View style={s.card}>
        <Text style={s.status}>
          {connected
            ? "✅ Terhubung"
            : scanning
              ? "🔍 Scan..."
              : "❌ Belum konek"}
        </Text>
      </View>

      <TouchableOpacity
        style={[s.btn, connected && s.btnDim]}
        onPress={connect}
        disabled={connected}
      >
        <Text style={s.btnTxt}>
          {connected ? "Sudah terhubung" : "Hubungkan ke ESP32"}
        </Text>
      </TouchableOpacity>

      <Text style={s.h}>Mode Display</Text>
      <View style={s.row}>
        {["Tegangan", "Waktu", "Navigasi"].map((l, i) => (
          <TouchableOpacity
            key={i}
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

      <Text style={s.h}>Navigasi Manual</Text>

      <Text style={s.lbl}>Arah:</Text>
      <ScrollView horizontal showsHorizontalScrollIndicator={false}>
        {[
          "straight",
          "left",
          "right",
          "slight-left",
          "slight-right",
          "uturn",
          "roundabout",
        ].map((a) => (
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
        placeholder="150m"
      />
      <TextInput
        style={s.input}
        value={street}
        onChangeText={setStreet}
        placeholder="Nama jalan"
      />
      <TextInput
        style={s.input}
        value={eta}
        onChangeText={setEta}
        placeholder="14:32"
      />

      <TouchableOpacity style={s.btn} onPress={sendNav}>
        <Text style={s.btnTxt}>Kirim Navigasi</Text>
      </TouchableOpacity>

      <TouchableOpacity
        style={[s.btn, s.btnRed]}
        onPress={() => sendJson({ cmd: "reset_time" })}
      >
        <Text style={s.btnTxt}>Reset Timer Mesin</Text>
      </TouchableOpacity>

      <Text style={s.hint}>
        Untuk integrasi Google Maps realtime otomatis, lihat bagian
        "NotificationListener" di dokumentasi — perlu bare workflow / dev
        client.
      </Text>
    </ScrollView>
  );
}

const s = StyleSheet.create({
  root: {
    padding: 20,
    paddingTop: 55,
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
  lbl: { color: "#8b949e", marginBottom: 4, fontSize: 12 },
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
