export const C = {
  bg: "#0a0e13",
  surface: "#121922",
  surface2: "#1a2430",
  border: "#243040",
  text: "#e8eef5",
  sub: "#8a97a6",
  accent: "#2ea8ff",
  accentDim: "#143a57",
  ok: "#34d399",
  warn: "#fbbf24",
  danger: "#f87171",
  oled: "#05080b",
  oledText: "#7dd3fc",
} as const;

export const S = { xs: 4, sm: 8, md: 12, lg: 16, xl: 24, xxl: 32 } as const;
export const R = { sm: 8, md: 12, lg: 16, pill: 999 } as const;

export const T = {
  title: { fontSize: 24, fontWeight: "800" as const, color: C.text },
  h: {
    fontSize: 13,
    fontWeight: "700" as const,
    color: C.sub,
    letterSpacing: 0.8,
  },
  body: { fontSize: 14, color: C.text },
  small: { fontSize: 12, color: C.sub },
  big: { fontSize: 34, fontWeight: "800" as const },
};
