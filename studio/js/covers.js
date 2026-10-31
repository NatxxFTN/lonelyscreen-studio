/**
 * Pochettes générées : chaque extension est représentée par un téléphone en
 * recopie posé sur un fond à ses couleurs, et l'écran montre l'effet.
 * Le rendu est déterministe (même identifiant, même pochette).
 * @module covers
 */

const SIZE = 200;
const PHONE_W = 82;
const PHONE_H = 150;
const SCREEN_INSET = 5;

/** Hachage FNV-1a 32 bits, pour varier les compositions d'un élément à l'autre. */
function hash(text) {
  let h = 0x811c9dc5;
  for (let i = 0; i < text.length; i += 1) {
    h ^= text.charCodeAt(i);
    h = Math.imul(h, 0x01000193);
  }
  return h >>> 0;
}

/** Générateur pseudo-aléatoire déterministe (mulberry32). */
function random(seed) {
  let a = seed;
  return () => {
    a = (a + 0x6d2b79f5) | 0;
    let t = Math.imul(a ^ (a >>> 15), 1 | a);
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

/** Convertit #RRGGBB ou #AARRGGBB en {color, opacity}. */
export function splitColor(value, fallback = '#000000') {
  const hex = String(value ?? '').replace('#', '');
  if (hex.length === 8) {
    return { color: `#${hex.slice(2)}`, opacity: parseInt(hex.slice(0, 2), 16) / 255 };
  }
  if (hex.length === 6) return { color: `#${hex}`, opacity: 1 };
  return { color: fallback, opacity: 1 };
}

function backdrop(motif, [c0, c1, c2], rnd) {
  switch (motif) {
    case 'orbit': {
      const cx = 40 + rnd() * 120;
      return `<circle cx="${cx}" cy="${30 + rnd() * 40}" r="92" fill="none" stroke="${c1}" stroke-width="14" opacity=".55"/>
              <circle cx="${SIZE - cx}" cy="${160 + rnd() * 20}" r="46" fill="${c2}" opacity=".5"/>`;
    }
    case 'waves': {
      const y = 120 + rnd() * 20;
      return `<path d="M0 ${y} C60 ${y - 40} 120 ${y + 30} 200 ${y - 20} V200 H0Z" fill="${c1}" opacity=".7"/>
              <path d="M0 ${y + 34} C70 ${y + 4} 130 ${y + 60} 200 ${y + 20} V200 H0Z" fill="${c2}" opacity=".55"/>`;
    }
    case 'grid': {
      let lines = '';
      for (let i = 1; i < 10; i += 1) {
        lines += `<path d="M${i * 20} 0V200M0 ${i * 20}H200" stroke="${c1}" stroke-width="1" opacity=".16"/>`;
      }
      return `${lines}<rect x="${20 + Math.floor(rnd() * 6) * 20}" y="20" width="40" height="40" fill="${c2}" opacity=".85"/>`;
    }
    case 'sharpen':
      return [0, 1, 2, 3].map((i) => `<rect x="${18 + i * 14}" y="${18 + i * 14}" width="${164 - i * 28}" height="${164 - i * 28}"
              rx="4" fill="none" stroke="${i % 2 ? c2 : c1}" stroke-width="2" opacity="${0.75 - i * 0.12}"/>`).join('');
    case 'frame':
      return `<path d="M0 0H200L0 200Z" fill="${c1}" opacity=".35"/><circle cx="168" cy="40" r="22" fill="${c2}" opacity=".8"/>`;
    case 'field':
      return `<rect x="0" y="0" width="200" height="200" fill="${c1}" opacity=".5"/>
              <rect x="0" y="0" width="200" height="200" fill="url(#vignette)"/>`;
    case 'dial':
      return `<circle cx="100" cy="100" r="84" fill="none" stroke="${c1}" stroke-width="10" opacity=".25"/>
              <path d="M100 16 A84 84 0 0 1 184 100" fill="none" stroke="${c2}" stroke-width="10" stroke-linecap="round"/>`;
    case 'swatches':
      return [c1, c2, c0, c1].map((c, i) => `<rect x="${i * 50}" y="0" width="50" height="200" fill="${c}" opacity="${0.3 + (i % 2) * 0.35}"/>`).join('');
    default:
      return '';
  }
}

/**
 * Construit la pochette SVG d'un élément.
 * @param {{id: string, cover?: {colors?: string[], motif?: string}}} item
 * @param {{border?: string, borderPx?: number, tint?: string, field?: string, filter?: string}} [preview]
 *   Valeurs de réglages à montrer sur l'écran (aperçu en direct).
 * @returns {string}
 */
export function coverSvg(item, preview = {}) {
  const colors = item.cover?.colors?.length >= 3 ? item.cover.colors : ['#1E2027', '#8B6CFF', '#F2F3F5'];
  const [c0, c1, c2] = colors;
  const rnd = random(hash(item.id ?? 'x'));
  const uid = `c${hash(`${item.id}-${JSON.stringify(preview)}`).toString(36)}`;
  const tilt = -10 + rnd() * 8;
  const px = (SIZE - PHONE_W) / 2 + (rnd() - 0.5) * 14;
  const py = (SIZE - PHONE_H) / 2 + 10;
  const sx = px + SCREEN_INSET;
  const sy = py + SCREEN_INSET;
  const sw = PHONE_W - SCREEN_INSET * 2;
  const sh = PHONE_H - SCREEN_INSET * 2;

  const field = preview.field ?? c0;
  const border = splitColor(preview.border);
  // L'épaisseur réelle (en pixels écran) est ramenée à l'échelle du mini-écran, mais reste lisible.
  const rawBorder = Number(preview.borderPx ?? 0);
  const borderPx = rawBorder > 0 ? Math.min(10, Math.max(4, rawBorder * 0.8)) : 0;
  const tint = preview.tint ? splitColor(preview.tint) : { color: '#000000', opacity: 0 };
  const screenFilter = preview.filter ? `style="filter:${preview.filter}"` : '';

  return `<svg viewBox="0 0 ${SIZE} ${SIZE}" xmlns="http://www.w3.org/2000/svg" aria-hidden="true">
  <defs>
    <radialGradient id="vignette" cx="50%" cy="40%" r="75%"><stop offset="60%" stop-color="#000" stop-opacity="0"/><stop offset="100%" stop-color="#000" stop-opacity=".45"/></radialGradient>
    <clipPath id="${uid}-screen"><rect x="${sx}" y="${sy}" width="${sw}" height="${sh}" rx="9"/></clipPath>
    <linearGradient id="${uid}-glass" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#fff" stop-opacity=".18"/><stop offset=".5" stop-color="#fff" stop-opacity="0"/></linearGradient>
  </defs>
  <rect width="${SIZE}" height="${SIZE}" fill="${field}"/>
  ${backdrop(item.cover?.motif, colors, rnd)}
  <g transform="rotate(${tilt.toFixed(2)} ${px + PHONE_W / 2} ${py + PHONE_H / 2})">
    <rect x="${px + 3}" y="${py + 8}" width="${PHONE_W}" height="${PHONE_H}" rx="14" fill="#000" opacity=".35"/>
    <rect x="${px}" y="${py}" width="${PHONE_W}" height="${PHONE_H}" rx="14" fill="#0b0b0e"/>
    <g clip-path="url(#${uid}-screen)" ${screenFilter}>
      <rect x="${sx}" y="${sy}" width="${sw}" height="${sh}" fill="${c1}"/>
      <circle cx="${sx + sw * (0.3 + rnd() * 0.4)}" cy="${sy + sh * 0.62}" r="${sw * 0.55}" fill="${c2}" opacity=".9"/>
      <circle cx="${sx + sw * 0.85}" cy="${sy + sh * 0.2}" r="${sw * 0.28}" fill="${c0}" opacity=".55"/>
      <rect x="${sx + 8}" y="${sy + 12}" width="${sw * 0.45}" height="5" rx="2.5" fill="#fff" opacity=".85"/>
      <rect x="${sx + 8}" y="${sy + 21}" width="${sw * 0.3}" height="4" rx="2" fill="#fff" opacity=".5"/>
      <rect x="${sx}" y="${sy}" width="${sw}" height="${sh}" fill="${tint.color}" opacity="${tint.opacity}"/>
      <rect x="${sx}" y="${sy}" width="${sw}" height="${sh}" fill="url(#${uid}-glass)"/>
    </g>
    ${borderPx > 0 && border.opacity > 0 ? `<rect x="${sx + borderPx / 2}" y="${sy + borderPx / 2}" width="${sw - borderPx}" height="${sh - borderPx}" rx="8" fill="none" stroke="${border.color}" stroke-opacity="${border.opacity}" stroke-width="${borderPx}"/>` : ''}
    <rect x="${px + PHONE_W / 2 - 11}" y="${py + 9}" width="22" height="6" rx="3" fill="#0b0b0e"/>
  </g>
</svg>`;
}

/**
 * Couleur de fond du héros dérivée d'une pochette : la couleur principale assombrie.
 * @param {{cover?: {colors?: string[]}}} item
 * @returns {string}
 */
export function heroColor(item) {
  const base = item?.cover?.colors?.[1] ?? '#8B6CFF';
  return `color-mix(in oklab, ${base} 38%, #0e0f13)`;
}
