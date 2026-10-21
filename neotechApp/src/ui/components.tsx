import React, { useCallback, useEffect, useRef, useState } from "react";
import {
  ActivityIndicator,
  LayoutChangeEvent,
  Pressable,
  StyleProp,
  StyleSheet,
  Text,
  View,
  ViewStyle,
} from "react-native";
import Animated, {
  Easing,
  FadeIn,
  FadeInDown,
  FadeOut,
  FadeOutDown,
  useAnimatedStyle,
  useSharedValue,
  withRepeat,
  withSequence,
  withSpring,
  withTiming,
} from "react-native-reanimated";

import { C, R, S, T } from "./theme";

export function PressableScale({
  onPress,
  disabled,
  style,
  children,
}: {
  onPress?: () => void;
  disabled?: boolean;
  style?: StyleProp<ViewStyle>;
  children: React.ReactNode;
}) {
  const sc = useSharedValue(1);
  const st = useAnimatedStyle(() => ({ transform: [{ scale: sc.value }] }));
  return (
    <Pressable
      disabled={disabled}
      onPress={onPress}
      onPressIn={() => {
        sc.value = withTiming(0.96, { duration: 90 });
      }}
      onPressOut={() => {
        sc.value = withSpring(1, { damping: 12, stiffness: 240 });
      }}
    >
      <Animated.View style={[style, st]}>{children}</Animated.View>
    </Pressable>
  );
}

type Variant = "primary" | "ghost" | "danger";
export function Button({
  label,
  onPress,
  variant = "primary",
  loading,
  disabled,
  style,
}: {
  label: string;
  onPress?: () => void;
  variant?: Variant;
  loading?: boolean;
  disabled?: boolean;
  style?: StyleProp<ViewStyle>;
}) {
  const bg =
    variant === "primary"
      ? C.accent
      : variant === "danger"
        ? "#7f1d1d"
        : C.surface2;
  const fg = variant === "primary" ? "#04131f" : C.text;
  const off = disabled || loading;
  return (
    <PressableScale
      onPress={onPress}
      disabled={off}
      style={[
        s.btn,
        { backgroundColor: bg, opacity: off ? 0.55 : 1 },
        variant === "ghost" && { borderWidth: 1, borderColor: C.border },
        style,
      ]}
    >
      {loading ? (
        <ActivityIndicator color={fg} size="small" />
      ) : (
        <Text style={[s.btnTxt, { color: fg }]}>{label}</Text>
      )}
    </PressableScale>
  );
}

export function Card({
  title,
  index = 0,
  right,
  children,
}: {
  title?: string;
  index?: number;
  right?: React.ReactNode;
  children: React.ReactNode;
}) {
  return (
    <Animated.View
      entering={FadeInDown.delay(index * 70).duration(380)}
      style={s.card}
    >
      {title ? (
        <View style={s.cardHead}>
          <Text style={T.h}>{title.toUpperCase()}</Text>
          {right}
        </View>
      ) : null}
      {children}
    </Animated.View>
  );
}

export function Skeleton({
  w,
  h = 16,
  style,
}: {
  w?: number | `${number}%`;
  h?: number;
  style?: StyleProp<ViewStyle>;
}) {
  const o = useSharedValue(0.35);
  useEffect(() => {
    o.value = withRepeat(
      withTiming(0.8, { duration: 750, easing: Easing.inOut(Easing.ease) }),
      -1,
      true,
    );
  }, [o]);
  const st = useAnimatedStyle(() => ({ opacity: o.value }));
  return (
    <Animated.View
      style={[
        {
          width: w ?? "100%",
          height: h,
          borderRadius: R.sm,
          backgroundColor: C.surface2,
        },
        st,
        style,
      ]}
    />
  );
}

export function StatusPill({
  state,
  text,
}: {
  state: "ok" | "busy" | "off";
  text: string;
}) {
  const o = useSharedValue(1);
  useEffect(() => {
    if (state === "busy") {
      o.value = withRepeat(
        withSequence(
          withTiming(0.25, { duration: 500 }),
          withTiming(1, { duration: 500 }),
        ),
        -1,
      );
    } else {
      o.value = withTiming(1, { duration: 200 });
    }
  }, [state, o]);
  const dot = useAnimatedStyle(() => ({ opacity: o.value }));
  const color = state === "ok" ? C.ok : state === "busy" ? C.warn : C.danger;
  return (
    <View style={[s.pill, { borderColor: color + "55" }]}>
      <Animated.View style={[s.dot, { backgroundColor: color }, dot]} />
      <Text style={[s.pillTxt, { color }]}>{text}</Text>
    </View>
  );
}

export function Segmented({
  options,
  value,
  onChange,
}: {
  options: string[];
  value: number;
  onChange: (i: number) => void;
}) {
  const [w, setW] = useState(0);
  const x = useSharedValue(0);
  const segW = w > 0 ? (w - 8) / options.length : 0;
  useEffect(() => {
    x.value = withSpring(value * segW, { damping: 18, stiffness: 220 });
  }, [value, segW, x]);
  const ind = useAnimatedStyle(() => ({
    transform: [{ translateX: x.value }],
  }));
  const onLayout = useCallback(
    (e: LayoutChangeEvent) => setW(e.nativeEvent.layout.width),
    [],
  );
  return (
    <View style={s.seg} onLayout={onLayout}>
      {segW > 0 && <Animated.View style={[s.segInd, { width: segW }, ind]} />}
      {options.map((o, i) => (
        <Pressable key={o} style={s.segItem} onPress={() => onChange(i)}>
          <Text style={[s.segTxt, value === i && { color: "#04131f" }]}>
            {o}
          </Text>
        </Pressable>
      ))}
    </View>
  );
}

export type ToastKind = "ok" | "err" | "info";
export function useToast() {
  const [t, setT] = useState<{
    id: number;
    msg: string;
    kind: ToastKind;
  } | null>(null);
  const timer = useRef<ReturnType<typeof setTimeout> | null>(null);
  const show = useCallback((msg: string, kind: ToastKind = "info") => {
    if (timer.current) clearTimeout(timer.current);
    setT({ id: Date.now(), msg, kind });
    timer.current = setTimeout(() => setT(null), 2400);
  }, []);
  useEffect(
    () => () => {
      if (timer.current) clearTimeout(timer.current);
    },
    [],
  );
  const node = t ? (
    <Animated.View
      key={t.id}
      entering={FadeInDown.duration(220)}
      exiting={FadeOutDown.duration(180)}
      style={[
        s.toast,
        {
          borderColor:
            t.kind === "ok" ? C.ok : t.kind === "err" ? C.danger : C.border,
        },
      ]}
      pointerEvents="none"
    >
      <Text style={s.toastTxt}>{t.msg}</Text>
    </Animated.View>
  ) : null;
  return { toast: node, show };
}

export function Collapsible({
  open,
  children,
}: {
  open: boolean;
  children: React.ReactNode;
}) {
  if (!open) return null;
  return (
    <Animated.View
      entering={FadeIn.duration(220)}
      exiting={FadeOut.duration(140)}
    >
      {children}
    </Animated.View>
  );
}

export class ErrorBoundary extends React.Component<
  { children: React.ReactNode },
  { err: Error | null }
> {
  state = { err: null as Error | null };
  static getDerivedStateFromError(err: Error) {
    return { err };
  }
  render() {
    if (!this.state.err) return this.props.children;
    return (
      <View style={[s.center, { backgroundColor: C.bg }]}>
        <Text style={T.title}>Ups, ada error</Text>
        <Text style={[T.small, { marginVertical: S.md, textAlign: "center" }]}>
          {String(this.state.err.message)}
        </Text>
        <Button
          label="Coba lagi"
          onPress={() => this.setState({ err: null })}
        />
      </View>
    );
  }
}

const s = StyleSheet.create({
  btn: {
    minHeight: 48,
    paddingHorizontal: S.lg,
    borderRadius: R.md,
    alignItems: "center",
    justifyContent: "center",
  },
  btnTxt: { fontSize: 15, fontWeight: "800" },
  card: {
    backgroundColor: C.surface,
    borderRadius: R.lg,
    borderWidth: 1,
    borderColor: C.border,
    padding: S.lg,
    marginBottom: S.lg,
    gap: S.md,
  },
  cardHead: {
    flexDirection: "row",
    justifyContent: "space-between",
    alignItems: "center",
  },
  pill: {
    flexDirection: "row",
    alignItems: "center",
    gap: S.sm,
    paddingHorizontal: S.md,
    paddingVertical: 6,
    borderRadius: R.pill,
    borderWidth: 1,
    backgroundColor: C.surface,
  },
  dot: { width: 8, height: 8, borderRadius: 4 },
  pillTxt: { fontSize: 12, fontWeight: "700" },
  seg: {
    flexDirection: "row",
    backgroundColor: C.surface2,
    borderRadius: R.md,
    padding: 4,
    height: 48,
  },
  segInd: {
    position: "absolute",
    top: 4,
    left: 4,
    bottom: 4,
    borderRadius: R.sm,
    backgroundColor: C.accent,
  },
  segItem: { flex: 1, alignItems: "center", justifyContent: "center" },
  segTxt: { color: C.sub, fontWeight: "700", fontSize: 13 },
  toast: {
    position: "absolute",
    left: S.lg,
    right: S.lg,
    bottom: S.xl,
    backgroundColor: C.surface2,
    borderWidth: 1,
    borderRadius: R.md,
    paddingVertical: S.md,
    paddingHorizontal: S.lg,
  },
  toastTxt: {
    color: C.text,
    fontSize: 14,
    fontWeight: "600",
    textAlign: "center",
  },
  center: {
    flex: 1,
    alignItems: "center",
    justifyContent: "center",
    padding: S.xl,
  },
});
