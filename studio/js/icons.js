/**
 * Jeu d'icônes du Studio : SVG dessinés sur une grille 24, trait 1,75, bouts arrondis.
 * @module icons
 */

const PATHS = {
  home: '<path d="M4 10.5 12 4l8 6.5V19a1 1 0 0 1-1 1h-4.5v-5.5h-5V20H5a1 1 0 0 1-1-1z"/>',
  store: '<path d="M5 8h14l-1.2 11.1a1 1 0 0 1-1 .9H7.2a1 1 0 0 1-1-.9z"/><path d="M9 10V7a3 3 0 0 1 6 0v3"/>',
  library: '<rect x="4" y="4" width="4" height="16" rx="1"/><rect x="10" y="4" width="4" height="16" rx="1"/><path d="m16.5 4.8 3.6 14.4"/>',
  themes: '<path d="M12 4a8 8 0 1 0 0 16c1.1 0 1.6-.9 1.2-1.8-.5-1-.2-2.2 1.1-2.2H17a3 3 0 0 0 3-3c0-5-3.6-9-8-9z"/><circle cx="8" cy="11" r="1.2"/><circle cx="11" cy="7.8" r="1.2"/><circle cx="15.3" cy="9" r="1.2"/>',
  settings: '<path d="M4 7h9M17 7h3M4 17h3M11 17h9"/><circle cx="15" cy="7" r="2"/><circle cx="9" cy="17" r="2"/>',
  search: '<circle cx="11" cy="11" r="6.5"/><path d="m20 20-4.2-4.2"/>',
  close: '<path d="M6 6l12 12M18 6 6 18"/>',
  back: '<path d="M14.5 5.5 8 12l6.5 6.5"/>',
  next: '<path d="M9.5 5.5 16 12l-6.5 6.5"/>',
  check: '<path d="m5 12.5 4.5 4.5L19 7.5"/>',
  download: '<path d="M12 4v11M7.5 10.5 12 15l4.5-4.5M5 19.5h14"/>',
  trash: '<path d="M5 7h14M10 7V5h4v2M7 7l.8 12.1a1 1 0 0 0 1 .9h6.4a1 1 0 0 0 1-.9L17 7"/>',
  refresh: '<path d="M19.5 12a7.5 7.5 0 1 1-2.2-5.3"/><path d="M19.5 4.5v4h-4"/>',
  folder: '<path d="M4 7a1 1 0 0 1 1-1h4.5l2 2H19a1 1 0 0 1 1 1v9a1 1 0 0 1-1 1H5a1 1 0 0 1-1-1z"/>',
  info: '<circle cx="12" cy="12" r="8.5"/><path d="M12 11v5M12 8h.01"/>',
  alert: '<path d="M12 4.5 20.5 19h-17z"/><path d="M12 10v4M12 16.8h.01"/>',
  sparkle: '<path d="M12 4l1.8 5.2L19 11l-5.2 1.8L12 18l-1.8-5.2L5 11l5.2-1.8z"/>',
  app: '<rect x="4.5" y="4.5" width="6" height="6" rx="1.5"/><rect x="13.5" y="4.5" width="6" height="6" rx="1.5"/><rect x="4.5" y="13.5" width="6" height="6" rx="1.5"/><rect x="13.5" y="13.5" width="6" height="6" rx="1.5"/>',
  link: '<path d="M10 14a4 4 0 0 0 5.7 0l3-3a4 4 0 0 0-5.7-5.7l-1 1"/><path d="M14 10a4 4 0 0 0-5.7 0l-3 3A4 4 0 0 0 11 18.7l1-1"/>',
  phone: '<rect x="7" y="3.5" width="10" height="17" rx="2.2"/><path d="M11 17.5h2"/>',
  keyboard: '<rect x="3.5" y="6.5" width="17" height="11" rx="2"/><path d="M7 10h.01M10 10h.01M13 10h.01M16 10h.01M8 14h8"/>',
};

/**
 * Renvoie le balisage SVG d'une icône.
 * @param {keyof typeof PATHS} name Nom de l'icône.
 * @param {string} [label] Libellé accessible ; sans libellé, l'icône est décorative.
 * @returns {string}
 */
export function icon(name, label) {
  const body = PATHS[name] ?? PATHS.info;
  const a11y = label ? `role="img" aria-label="${label}"` : 'aria-hidden="true"';
  return `<svg class="icon" viewBox="0 0 24 24" ${a11y}>${body}</svg>`;
}

/** Marque du Studio : un téléphone dont l'écran se reflète dans un second écran. */
export const BRAND_MARK = `
<svg class="brand-mark" viewBox="0 0 32 32" aria-hidden="true">
  <rect x="1" y="1" width="30" height="30" rx="8" fill="var(--accent)"/>
  <rect x="7.5" y="6.5" width="9" height="17" rx="2.2" fill="none" stroke="var(--accent-ink)" stroke-width="2"/>
  <path d="M19.5 10.5h4a1.5 1.5 0 0 1 1.5 1.5v8a1.5 1.5 0 0 1-1.5 1.5h-4" fill="none" stroke="var(--accent-ink)" stroke-width="2" stroke-linecap="round" opacity=".55"/>
  <path d="M12 20h0" stroke="var(--accent-ink)" stroke-width="2" stroke-linecap="round"/>
</svg>`;
