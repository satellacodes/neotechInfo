import React, { useRef, useState } from "react";
import {
  LayoutChangeEvent,
  PanResponder,
  StyleSheet,
  View,
} from "react-native";

import { C } from "./theme";

/**
 * Slider kecil tanpa dependensi tambahan (PanResponder).
 * - onChange dipanggil terus saat digeser (pemanggil sebaiknya men-throttle kirim BLE)
 * - onCommit dipanggil sekali saat jari dilepas (simpan ke flash ESP32 di sini)
 */
export function AppSlider({
  value,
  min,
  max,
  onChange,
  onCommit,
  disabled,
}: {
  value: number;
  min: number;
  max: number;
  onChange: (v: number) => void;
  onCommit: (v: number) => void;
  disabled?: boolean;
}) {
  const [w, setW] = useState(0);
  const wRef = useRef(0);
  const startX = useRef(0);
  const last = useRef(value);
  const cb = useRef({ onChange, onCommit, min, max, disabled });
  cb.current = { onChange, onCommit, min, max, disabled };

  const toValue = (x: number) => {
    const f = Math.max(0, Math.min(1, x / Math.max(1, wRef.current)));
    return Math.round(cb.current.min + f * (cb.current.max - cb.current.min));
  };

  const pan = useRef(
    PanResponder.create({
      onStartShouldSetPanResponder: () => !cb.current.disabled,
      onMoveShouldSetPanResponder: () => !cb.current.disabled,
      onPanResponderTerminationRequest: () => false,
      onPanResponderGrant: (e) => {
        startX.current = e.nativeEvent.locationX;
        const v = toValue(startX.current);
        last.current = v;
        cb.current.onChange(v);
      },
      onPanResponderMove: (_e, g) => {
        const v = toValue(startX.current + g.dx);
        if (v !== last.current) {
          last.current = v;
          cb.current.onChange(v);
        }
      },
      onPanResponderRelease: () => cb.current.onCommit(last.current),
      onPanResponderTerminate: () => cb.current.onCommit(last.current),
    }),
  ).current;

  const f = Math.max(0, Math.min(1, (value - min) / Math.max(1, max - min)));
  const onLayout = (e: LayoutChangeEvent) => {
    wRef.current = e.nativeEvent.layout.width;
    setW(e.nativeEvent.layout.width);
  };

  return (
    <View
      style={[st.hit, disabled && { opacity: 0.5 }]}
      onLayout={onLayout}
      {...pan.panHandlers}
    >
      <View style={st.track} pointerEvents="none">
        <View style={[st.fill, { width: f * w }]} />
      </View>
      <View style={[st.thumb, { left: f * w - 12 }]} pointerEvents="none" />
    </View>
  );
}

const st = StyleSheet.create({
  hit: { height: 40, justifyContent: "center" },
  track: {
    height: 8,
    borderRadius: 4,
    backgroundColor: C.surface2,
    overflow: "hidden",
  },
  fill: { height: 8, backgroundColor: C.accent },
  thumb: {
    position: "absolute",
    top: 8,
    width: 24,
    height: 24,
    borderRadius: 12,
    backgroundColor: C.text,
    borderWidth: 3,
    borderColor: C.accent,
  },
});
