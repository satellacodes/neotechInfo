export type NavArrow =
  | "straight"
  | "left"
  | "right"
  | "slight-left"
  | "slight-right"
  | "sharp-left"
  | "sharp-right"
  | "uturn"
  | "roundabout"
  | "arrive";

export interface NavInfo {
  arrow: NavArrow;
  distance: string;
  street: string;
  eta: string;
}

export interface RawNotification {
  app?: string;
  title?: string;
  titleBig?: string;
  text?: string;
  bigText?: string;
  subText?: string;
  summaryText?: string;
  extraInfoText?: string;
}

export const MAPS_PKG = "com.google.android.apps.maps";

const DIST = /(\d+(?:[.,]\d+)?)\s*(km|m|mi|ft|yd)\b/i;
const DIST_G = /(\d+(?:[.,]\d+)?)\s*(km|m|mi|ft|yd)\b/gi;
const TRIP = /\b(mnt|min|mins|menit|jam|hr|hrs|hour|hours)\b|\d\s*j\b/i;

function clean(s: unknown): string {
  return String(s ?? "")
    .replace(/[   ]/g, " ")
    .replace(/[‎‏‪-‮]/g, "")
    .replace(/\s+/g, " ")
    .trim();
}

function toAscii(s: string, max: number): string {
  return s
    .normalize("NFD")
    .replace(/[̀-ͯ]/g, "")
    .replace(/[^\x20-\x7e]/g, "?")
    .slice(0, max);
}

function sideOf(t: string): "left" | "right" | null {
  const m = t.match(/\b(kiri|left|kanan|right)\b/i);
  if (!m) return null;
  return /kiri|left/i.test(m[1]) ? "left" : "right";
}

export function detectArrow(text: string): NavArrow | null {
  const t = text.toLowerCase();
  if (/putar balik|u-?turn|balik arah/.test(t)) return "uturn";
  if (/bundaran|bunderan|roundabout|traffic circle|rotary/.test(t))
    return "roundabout";
  if (
    /tujuan anda|tujuan\b|destination|you have arrived|telah tiba|sudah sampai/.test(
      t,
    )
  )
    return "arrive";

  const side = sideOf(t);
  if (side) {
    if (/tajam|sharp/.test(t))
      return side === "left" ? "sharp-left" : "sharp-right";
    if (/sedikit|serong|slight|\bbear\b|tetap|\bkeep\b/.test(t))
      return side === "left" ? "slight-left" : "slight-right";
    if (/\b(belok|turn)\b/.test(t)) return side;
    if (/ambil|\btake\b|keluar|\bexit\b|merge|gabung|menyatu/.test(t))
      return side === "left" ? "slight-left" : "slight-right";
    return side;
  }
  if (
    /lurus|terus|lanjut|continue|straight|\bhead\b|menuju|ikuti|follow/.test(t)
  )
    return "straight";
  return null;
}

function collectLines(n: RawNotification): string[] {
  const src = [
    n.title,
    n.titleBig,
    n.text,
    n.bigText,
    n.subText,
    n.summaryText,
    n.extraInfoText,
  ];
  const out: string[] = [];
  for (const f of src) {
    if (!f) continue;
    for (const part of String(f).split(/\r?\n/)) {
      const c = clean(part);
      if (c && !out.includes(c)) out.push(c);
    }
  }
  return out;
}

function findDistance(lines: string[]): string {
  for (const l of lines) {
    if (TRIP.test(l)) continue; // baris ringkasan perjalanan (mis. "12 mnt · 5,2 km")
    const m = l.match(DIST);
    if (m) return `${m[1]} ${m[2].toLowerCase()}`;
  }
  for (const l of lines) {
    const m = l.match(DIST);
    if (m) return `${m[1]} ${m[2].toLowerCase()}`;
  }
  return "";
}

function stripDistance(s: string): string {
  return s
    .replace(DIST_G, " ")
    .replace(/[|·•–—]/g, " ")
    .replace(/\s{2,}/g, " ")
    .replace(/^[\s,.\-]+|[\s,.\-]+$/g, "")
    .trim();
}

function findStreet(lines: string[], instr: string | undefined): string {
  if (instr) {
    const s = stripDistance(instr);
    const m = s.match(
      /\b(?:ke arah|ke|onto|toward|towards|menuju|via)\s+(?!(?:kiri|kanan|left|right)\b)(.+)$/i,
    );
    if (m && m[1].length >= 2) return m[1].trim();
  }
  // fallback: baris lain yang bukan jarak murni / ringkasan perjalanan / kata perintah
  for (const l of lines) {
    if (l === instr) continue;
    if (TRIP.test(l)) continue;
    const s = stripDistance(l);
    if (s.length < 3) continue;
    if (detectArrow(s)) continue;
    if (/^(eta|tiba|arrive|arrival)\b/i.test(s)) continue;
    if (/^\d{1,2}[:.]\d{2}/.test(s)) continue;
    return s;
  }
  return "";
}

function findEta(lines: string[]): string {
  const re =
    /(?<![\d.,])(\d{1,2})[:.](\d{2})(?![\d.,])\s*(am|pm)?(?!\s*(?:km|m|mi)\b)/i;
  for (const l of lines) {
    const m = l.match(re);
    if (!m) continue;
    let h = parseInt(m[1], 10);
    const mi = parseInt(m[2], 10);
    if (mi > 59) continue;
    const ap = m[3]?.toLowerCase();
    if (ap === "pm" && h < 12) h += 12;
    if (ap === "am" && h === 12) h = 0;
    if (h > 23) continue;
    return `${String(h).padStart(2, "0")}:${String(mi).padStart(2, "0")}`;
  }
  return "";
}

export function parseMapsNotification(n: RawNotification): NavInfo | null {
  const lines = collectLines(n);
  if (lines.length === 0) return null;

  const instr = lines.find((l) => !TRIP.test(l) && detectArrow(l) !== null);
  const arrow = (
    instr ? detectArrow(instr) : detectArrow(lines.join(" | "))
  ) as NavArrow | null;
  const distance = findDistance(lines);
  if (!arrow && !distance) return null;

  return {
    arrow: arrow ?? "straight",
    distance: toAscii(distance, 12),
    street: toAscii(findStreet(lines, instr), 40),
    eta: findEta(lines),
  };
}
