/**
 * LonelyScreen Studio : interface du panneau d'extension.
 * Vues : Accueil, Boutique, Bibliothèque, Thèmes, détail d'un plugin, app hébergée, Réglages.
 * @module app
 */
import { call, isNative, onEvent } from './bridge.js';
import { coverSvg, heroColor } from './covers.js';
import { BRAND_MARK, icon } from './icons.js';

const DEFAULT_ACCENT = '#8B6CFF';
const ACCENT_PRESETS = ['#8B6CFF', '#3DDC97', '#2BB3FF', '#FF4FD8', '#FFA94D', '#F2F3F5'];
const HOVER_OPEN_DELAY_MS = 320;
const SCROLL_HOVER_PAUSE_MS = 450;
const DRAG_THRESHOLD_PX = 6;
const RAIL_PAGE_RATIO = 0.85;
const RAIL_EDGE_TOLERANCE_PX = 4;
const RAIL_REVEAL_MARGIN_PX = 22;
const TILE_REVEAL_DELAY_MS = 260;
const WRITE_DEBOUNCE_MS = 90;
const TOAST_MS = 3200;
const CONFIRM_WINDOW_MS = 4000;
const STATE_POLL_MS = 4000;
const LUMINANCE_THRESHOLD = 0.5;

const TYPE_LABEL = { plugin: 'Effet', theme: 'Thème', app: 'App' };

const state = {
  data: null,
  values: {},
  route: { name: 'home' },
  history: [],
  query: '',
  storeFilter: 'all',
  libraryTab: 'plugin',
  openTile: null,
  busy: new Set(),
  restart: new Set(),
  remote: { status: 'idle', items: [], error: '' },
  confirmReset: false,
  // idle | checking | uptodate | available | applying | ready | error | unavailable
  update: { status: 'idle', info: null, error: '' },
};

const INSTALL_COMMAND = 'iwr -useb https://raw.githubusercontent.com/NatxxFTN/lonelyscreen-studio/main/install.ps1 | iex';

const $ = (selector, root = document) => root.querySelector(selector);

/** Échappe le texte injecté dans le HTML. */
function esc(value) {
  return String(value ?? '').replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' })[c]);
}

const keyOf = (item) => `${item.type}:${item.id}`;
const cssId = (text) => String(text).replace(/[^a-z0-9-]/gi, '-');

// ---------------------------------------------------------------------------
// Données
// ---------------------------------------------------------------------------

function pluginManifest(plugin) {
  return { type: 'plugin', id: plugin.id, name: plugin.id, ...(plugin.manifest ?? {}) };
}

function installedPlugins() {
  return (state.data?.plugins ?? []).map((p) => ({ ...p, item: pluginManifest(p) }));
}

/** Toutes les entrées de catalogue (officiel + en ligne), dédoublonnées. */
function catalogItems() {
  const local = (state.data?.localCatalog?.items ?? []).map((item) => ({ ...item, origin: 'Officiel' }));
  const merged = new Map(local.map((item) => [keyOf(item), item]));
  state.remote.items.forEach((item) => merged.set(keyOf(item), { ...item, origin: 'En ligne' }));
  return [...merged.values()];
}

function isInstalled(item) {
  if (item.type === 'plugin') return (state.data?.plugins ?? []).some((p) => p.id === item.id);
  if (item.type === 'theme') return (state.data?.themes ?? []).some((t) => t.id === item.id);
  return (state.data?.apps ?? []).some((a) => a.id === item.id);
}

function themeById(id) {
  const installed = (state.data?.themes ?? []).find((t) => t.id === id);
  if (installed) return { type: 'theme', id, ...installed.manifest };
  return catalogItems().find((item) => item.type === 'theme' && item.id === id) ?? null;
}

function activeTheme() {
  const id = state.data?.config?.activeTheme;
  return id ? themeById(id) : null;
}

function allThemes() {
  const installed = (state.data?.themes ?? []).map((t) => ({ type: 'theme', id: t.id, ...t.manifest }));
  const ids = new Set(installed.map((t) => t.id));
  return [...installed, ...catalogItems().filter((i) => i.type === 'theme' && !ids.has(i.id))];
}

function fieldDefault(field) {
  return String(field.default ?? '');
}

async function loadPluginValues() {
  const plugins = installedPlugins().filter((p) => p.item.settings?.fields);
  const entries = await Promise.all(plugins.map(async ({ id, item }) => {
    const { file, section, fields } = item.settings;
    const stored = await call('readIni', { file, section, keys: fields.map((f) => f.key) }).catch(() => ({}));
    const values = Object.fromEntries(fields.map((f) => [f.key, stored[f.key] ?? fieldDefault(f)]));
    return [id, values];
  }));
  state.values = Object.fromEntries(entries);
}

async function loadRemoteCatalog() {
  const url = state.data?.config?.catalogUrl;
  if (!url) {
    state.remote = { status: 'idle', items: [], error: '' };
    return;
  }
  state.remote = { status: 'loading', items: [], error: '' };
  render();
  try {
    const { text } = await call('fetch', { url });
    const catalog = JSON.parse(text);
    const items = (catalog.items ?? []).map((item) => ({
      ...item,
      files: (item.files ?? []).map((file) => ({ ...file, src: new URL(file.src, url).href })),
    }));
    state.remote = { status: 'ready', items, error: '', name: catalog.name };
  } catch (error) {
    state.remote = { status: 'error', items: [], error: error.message };
  }
  render();
}

async function refresh() {
  state.data = await call('state');
  await loadPluginValues();
  applyAccent();
  render();
}

// ---------------------------------------------------------------------------
// Accent
// ---------------------------------------------------------------------------

function luminance(hex) {
  const value = hex.replace('#', '');
  const [r, g, b] = [0, 2, 4].map((i) => {
    const c = parseInt(value.slice(i, i + 2), 16) / 255;
    return c <= 0.03928 ? c / 12.92 : ((c + 0.055) / 1.055) ** 2.4;
  });
  return 0.2126 * r + 0.7152 * g + 0.0722 * b;
}

function currentAccent() {
  const config = state.data?.config ?? {};
  if (config.accentMode === 'custom' && /^#[0-9a-f]{6}$/i.test(config.accent ?? '')) return config.accent;
  return activeTheme()?.accent ?? DEFAULT_ACCENT;
}

function applyAccent() {
  const accent = currentAccent();
  const root = document.documentElement.style;
  root.setProperty('--accent', accent);
  root.setProperty('--accent-ink', luminance(accent) > LUMINANCE_THRESHOLD * 0.36 ? '#0e0f13' : '#ffffff');
}

async function saveConfig(patch) {
  const config = { ...(state.data?.config ?? {}), ...patch };
  await call('saveConfig', { config });
  state.data.config = config;
}

// ---------------------------------------------------------------------------
// Aperçus (la pochette montre les réglages courants)
// ---------------------------------------------------------------------------

function previewFor(pluginId) {
  const v = state.values[pluginId];
  if (!v || v.enabled === '0') return {};
  if (pluginId === 'frame') return { border: v.border_color, borderPx: Number(v.border_px), tint: v.tint_color };
  if (pluginId === 'background') return { field: v.color };
  if (pluginId === 'quality') {
    return { filter: `saturate(${Number(v.saturation) / 100}) contrast(${Number(v.contrast) / 100}) brightness(${1 + Number(v.brightness) / 100})` };
  }
  return {};
}

function combinedPreview() {
  return { ...previewFor('background'), ...previewFor('quality'), ...previewFor('frame') };
}

function cover(item, preview, transitionName) {
  const style = transitionName ? ` style="view-transition-name:${transitionName}"` : '';
  return `<div class="cover"${style}>${coverSvg(item, preview)}</div>`;
}

// ---------------------------------------------------------------------------
// Composants
// ---------------------------------------------------------------------------

function actionButtons(item) {
  const key = keyOf(item);
  if (state.busy.has(key)) {
    return `<button class="btn" disabled><span class="spinner"></span>${item.type === 'theme' ? 'Application…' : 'Installation…'}</button>`;
  }
  if (item.type === 'theme') {
    if (state.data?.config?.activeTheme === item.id) {
      return `<span class="btn btn-done">${icon('check')}Appliqué</span>`;
    }
    return `<button class="btn btn-primary" data-action="apply-theme" data-id="${esc(item.id)}">Appliquer</button>`;
  }
  if (item.type === 'app') {
    return isInstalled(item)
      ? `<button class="btn btn-primary" data-action="open-app" data-id="${esc(item.id)}">Ouvrir</button>`
      : `<button class="btn btn-primary" data-action="install" data-key="${esc(key)}">${icon('download')}Installer</button>`;
  }
  const plugin = (state.data?.plugins ?? []).find((p) => p.id === item.id);
  if (plugin && !plugin.loaded) return '<span class="chip chip-warn">Redémarrage requis</span>';
  if (plugin) return `<button class="btn btn-primary" data-action="open-plugin" data-id="${esc(item.id)}">Régler</button>`;
  return `<button class="btn btn-primary" data-action="install" data-key="${esc(key)}">${icon('download')}Installer</button>`;
}

function chips(item) {
  const list = [`<span class="chip">${TYPE_LABEL[item.type] ?? 'Extension'}</span>`];
  if (item.version) list.push(`<span class="chip">v${esc(item.version)}</span>`);
  if (item.builtin) list.push('<span class="chip chip-ok">Inclus</span>');
  else if (isInstalled(item)) list.push('<span class="chip chip-ok">Installé</span>');
  if (item.origin === 'En ligne') list.push('<span class="chip">En ligne</span>');
  return `<div class="chip-row">${list.join('')}</div>`;
}

function tile(item) {
  const key = keyOf(item);
  const open = state.openTile === key;
  const preview = item.type === 'plugin' ? previewFor(item.id) : {};
  return `<article class="tile${open ? ' is-open' : ''}" data-key="${esc(key)}">
    <button class="tile-hit" data-action="open-tile" data-key="${esc(key)}" aria-expanded="${open}" aria-label="${esc(item.name)} : afficher les détails"></button>
    ${cover(item, preview)}
    <div class="tile-meta"><h3>${esc(item.name)}</h3><p>${esc(item.summary)}</p></div>
    <div class="tile-detail" aria-hidden="${!open}">
      ${chips(item)}
      <h3>${esc(item.name)}</h3>
      <p>${esc(item.description || item.summary)}</p>
      <div class="tile-actions">${actionButtons(item)}</div>
    </div>
  </article>`;
}

function row(id, title, items, more) {
  if (!items.length) return '';
  const moreButton = more ? `<button class="more" data-action="nav" data-route="${more}">Tout afficher</button>` : '';
  const arrows = `<div class="row-arrows">
      <button class="icon-button" data-action="rail-scroll" data-row="${id}" data-direction="-1" aria-label="Faire défiler ${esc(title)} vers la gauche">${icon('back')}</button>
      <button class="icon-button" data-action="rail-scroll" data-row="${id}" data-direction="1" aria-label="Faire défiler ${esc(title)} vers la droite">${icon('next')}</button>
    </div>`;
  return `<section class="row" aria-labelledby="row-${id}">
    <div class="section-head"><h2 id="row-${id}">${title}</h2>${moreButton}${arrows}</div>
    <div class="rail-scroll" data-row="${id}">${items.map(tile).join('')}</div>
  </section>`;
}

function switchButton(pluginId, label) {
  const on = state.values[pluginId]?.enabled !== '0';
  return `<button class="switch" role="switch" aria-checked="${on}" aria-label="${esc(label)}" data-action="toggle-plugin" data-id="${esc(pluginId)}"></button>`;
}

function effectCards() {
  const plugins = installedPlugins().filter((p) => p.item.settings);
  if (!plugins.length) {
    return `<div class="empty"><strong>Aucun effet installé</strong><p>Les effets modifient l'image de la recopie : netteté, cadre, couleurs.</p>
      <button class="btn btn-primary" data-action="nav" data-route="store">Ouvrir la boutique</button></div>`;
  }
  return `<div class="effects">${plugins.map(({ id, item, loaded }) => `
    <div class="effect">
      ${cover(item, previewFor(id))}
      <button class="effect-name" data-action="open-plugin" data-id="${esc(id)}">
        <strong>${esc(item.name)}</strong>
        <span>${loaded ? (state.values[id]?.enabled === '0' ? 'Désactivé' : 'Actif') : 'Redémarrage requis'}</span>
      </button>
      ${loaded ? switchButton(id, `Activer ${item.name}`) : ''}
    </div>`).join('')}</div>`;
}

async function checkForUpdate({ silent = false } = {}) {
  if (!isNative) {
    state.update = { status: 'unavailable', info: null, error: 'Mises à jour disponibles uniquement dans LonelyScreen.' };
    render();
    return;
  }
  state.update = { status: 'checking', info: null, error: '' };
  render();
  try {
    const info = await call('updateCheck');
    state.update = { status: info.available ? 'available' : 'uptodate', info, error: '' };
  } catch (error) {
    state.update = { status: silent ? 'idle' : 'error', info: null, error: error.message };
  }
  render();
}

async function applyUpdate() {
  state.update = { ...state.update, status: 'applying' };
  render();
  try {
    const result = await call('updateApply');
    state.update = { status: 'ready', info: { ...state.update.info, latest: result.version }, error: '' };
    toast(`Version ${result.version} prête : redémarre LonelyScreen pour l'utiliser.`);
  } catch (error) {
    state.update = { ...state.update, status: 'error', error: error.message };
    toast(`Mise à jour impossible : ${error.message}`, 'error');
  }
  render();
}

function updateBanner() {
  const { status, info } = state.update;
  if (status === 'available') {
    return `<div class="banner banner-accent" role="status">${icon('download')}<p>LonelyScreen Studio ${esc(info.latest)} est disponible.</p>
      <button class="btn btn-primary" data-action="update-apply">Mettre à jour</button></div>`;
  }
  if (status === 'applying') {
    return `<div class="banner banner-accent" role="status"><span class="spinner"></span><p>Téléchargement et vérification de la mise à jour…</p></div>`;
  }
  if (status === 'ready') {
    return `<div class="banner" role="status">${icon('refresh')}<p>Mise à jour ${esc(info.latest)} installée : redémarre LonelyScreen pour l'utiliser.</p></div>`;
  }
  return '';
}

function restartBanner() {
  const pending = [...state.restart, ...installedPlugins().filter((p) => p.pendingRemoval || !p.loaded).map((p) => p.item.name)];
  const names = [...new Set(pending)];
  if (!names.length) return '';
  return `<div class="banner" role="status">${icon('refresh')}<p>Redémarre LonelyScreen pour finir d'appliquer : ${esc(names.join(', '))}.</p></div>`;
}

// ---------------------------------------------------------------------------
// Vues
// ---------------------------------------------------------------------------

function viewHome() {
  const theme = activeTheme();
  const heroItem = theme ?? { id: 'studio-default', cover: { colors: ['#151226', currentAccent(), '#F2F3F5'], motif: 'orbit' } };
  document.documentElement.style.setProperty('--hero', heroColor(heroItem));
  const hero = theme
    ? `<section class="hero">
        ${cover(heroItem, combinedPreview(), `cover-theme-${cssId(theme.id)}`)}
        <div>
          <p class="hero-label">Thème actif</p>
          <h1>${esc(theme.name)}</h1>
          <p class="summary">${esc(theme.summary)}</p>
          <div class="actions">
            <button class="btn btn-primary" data-action="nav" data-route="themes">Changer de thème</button>
            <button class="btn" data-action="clear-theme">Retirer le thème</button>
          </div>
        </div>
      </section>`
    : `<section class="hero">
        ${cover(heroItem, combinedPreview())}
        <div>
          <p class="hero-label">Aucun thème appliqué</p>
          <h1>Ta recopie, à ton image</h1>
          <p class="summary">Applique un thème en un clic, règle chaque effet en direct ou ajoute des apps au Studio.</p>
          <div class="actions">
            <button class="btn btn-primary" data-action="nav" data-route="themes">Choisir un thème</button>
            <button class="btn" data-action="nav" data-route="store">Ouvrir la boutique</button>
          </div>
        </div>
      </section>`;

  const themes = allThemes().filter((t) => t.id !== theme?.id);
  const apps = catalogItems().filter((i) => i.type === 'app');
  return `<div class="view">
    ${updateBanner()}
    ${restartBanner()}
    ${hero}
    <section aria-labelledby="effects-title">
      <div class="section-head"><h2 id="effects-title">Effets</h2><button class="more" data-action="nav" data-route="library">Gérer</button></div>
      ${effectCards()}
    </section>
    ${row('home-themes', 'Thèmes à essayer', themes, 'themes')}
    ${row('home-apps', 'Apps pour le Studio', apps, 'store')}
  </div>`;
}

function matchesQuery(item) {
  const q = state.query.trim().toLowerCase();
  if (!q) return true;
  return [item.name, item.summary, item.description, item.author].some((text) => String(text ?? '').toLowerCase().includes(q));
}

function storeGrid(items) {
  if (!items.length) {
    return `<div class="empty"><strong>Aucun résultat</strong><p>Rien ne correspond à « ${esc(state.query)} ». Essaie un autre mot, ou retire le filtre.</p>
      <button class="btn" data-action="clear-search">Effacer la recherche</button></div>`;
  }
  return `<div class="rail-scroll" data-row="store-results" style="flex-wrap:wrap;overflow:visible">${items.map(tile).join('')}</div>`;
}

function viewStore() {
  const items = catalogItems().filter(matchesQuery);
  const filters = [['all', 'Tout'], ['theme', 'Thèmes'], ['plugin', 'Effets'], ['app', 'Apps']];
  const filtered = state.storeFilter === 'all' ? items : items.filter((i) => i.type === state.storeFilter);
  const remote = state.remote;
  let source = `${icon('check')}Catalogue officiel`;
  if (remote.status === 'loading') source = '<span class="spinner"></span>Chargement du catalogue en ligne…';
  if (remote.status === 'ready') source = `${icon('link')}Officiel + ${esc(remote.name ?? 'catalogue en ligne')}`;
  const remoteError = remote.status === 'error'
    ? `<div class="note note-warn">${icon('alert')}<span>Catalogue en ligne indisponible : ${esc(remote.error)} Les extensions officielles restent disponibles.</span></div>`
    : '';
  const body = state.query || state.storeFilter !== 'all'
    ? storeGrid(filtered)
    : [row('store-themes', 'Thèmes', items.filter((i) => i.type === 'theme')),
      row('store-effects', 'Effets vidéo', items.filter((i) => i.type === 'plugin')),
      row('store-apps', 'Apps', items.filter((i) => i.type === 'app'))].join('');
  return `<div class="view">
    ${restartBanner()}
    <header class="store-head">
      <div><h1>Boutique</h1><p class="lede">Thèmes, effets et apps pour LonelyScreen. Tout s'installe sans droits administrateur, et chaque fichier est vérifié avant installation.</p></div>
      <span class="source">${source}</span>
    </header>
    <div class="filters" role="group" aria-label="Type d'extension">
      ${filters.map(([value, label]) => `<button class="filter" data-action="store-filter" data-value="${value}" aria-pressed="${state.storeFilter === value}">${label}</button>`).join('')}
    </div>
    ${remoteError}
    ${body}
  </div>`;
}

function viewLibrary() {
  const tabs = [['plugin', 'Effets'], ['theme', 'Thèmes'], ['app', 'Apps']];
  let list = '';
  if (state.libraryTab === 'plugin') {
    list = installedPlugins().map(({ id, item, loaded, source, pendingRemoval }) => `
      <div class="list-row">
        ${cover(item, previewFor(id))}
        <button class="row-main" data-action="open-plugin" data-id="${esc(id)}">
          <strong>${esc(item.name)}</strong>
          <span class="sub">${source === 'builtin' ? 'Inclus' : 'Installé'} · v${esc(item.version ?? '?')}${pendingRemoval ? ' · suppression au redémarrage' : ''}</span>
        </button>
        ${loaded && item.settings ? switchButton(id, `Activer ${item.name}`) : '<span class="chip chip-warn">Redémarrage requis</span>'}
        <button class="icon-button" data-action="open-plugin" data-id="${esc(id)}" aria-label="Régler ${esc(item.name)}">${icon('next')}</button>
      </div>`).join('');
  } else if (state.libraryTab === 'theme') {
    const themes = (state.data?.themes ?? []).map((t) => ({ type: 'theme', id: t.id, ...t.manifest }));
    list = themes.length ? themes.map((theme) => `
      <div class="list-row">
        ${cover(theme)}
        <div class="row-main"><strong>${esc(theme.name)}</strong><span class="sub">${esc(theme.summary)}</span></div>
        ${actionButtons(theme)}
        <button class="icon-button" data-action="uninstall" data-type="theme" data-id="${esc(theme.id)}" aria-label="Supprimer ${esc(theme.name)}">${icon('trash')}</button>
      </div>`).join('') : '';
  } else {
    const apps = (state.data?.apps ?? []).map((a) => ({ type: 'app', id: a.id, ...a.manifest }));
    list = apps.length ? apps.map((app) => `
      <div class="list-row">
        ${cover(app)}
        <div class="row-main"><strong>${esc(app.name)}</strong><span class="sub">${esc(app.summary)}</span></div>
        <button class="btn" data-action="open-app" data-id="${esc(app.id)}">Ouvrir</button>
        <button class="icon-button" data-action="uninstall" data-type="app" data-id="${esc(app.id)}" aria-label="Supprimer ${esc(app.name)}">${icon('trash')}</button>
      </div>`).join('') : '';
  }
  const emptyCopy = {
    plugin: ['Aucun effet', 'Les effets s\'installent depuis la boutique.'],
    theme: ['Aucun thème installé', 'Un thème change d\'un coup le fond, le cadre, les couleurs et l\'accent du Studio.'],
    app: ['Aucune app', 'Une app ajoute une page au Studio : minuteur, atelier de couleurs…'],
  }[state.libraryTab];
  const empty = `<div class="empty"><strong>${emptyCopy[0]}</strong><p>${emptyCopy[1]}</p>
    <button class="btn btn-primary" data-action="nav" data-route="store">Ouvrir la boutique</button></div>`;
  return `<div class="view">
    ${restartBanner()}
    <header><h1>Bibliothèque</h1><p class="lede">Tout ce qui est installé sur ce PC.</p></header>
    <div class="segmented" role="group" aria-label="Type">
      ${tabs.map(([value, label]) => `<button data-action="library-tab" data-value="${value}" aria-pressed="${state.libraryTab === value}">${label}</button>`).join('')}
    </div>
    ${list ? `<div class="list">${list}</div>` : empty}
  </div>`;
}

function viewThemes() {
  const active = state.data?.config?.activeTheme;
  const cards = allThemes().map((theme) => `
    <article class="theme-card" data-active="${active === theme.id}">
      <div class="cover" style="view-transition-name:cover-theme-${cssId(theme.id)}">${coverSvg(theme)}
        ${active === theme.id ? `<span class="applied-badge">${icon('check', 'Appliqué')}</span>` : ''}</div>
      <div><h3>${esc(theme.name)}</h3><p>${esc(theme.summary)}</p></div>
      ${actionButtons(theme)}
    </article>`).join('');
  return `<div class="view">
    <header class="store-head">
      <div><h1>Thèmes</h1><p class="lede">Un thème règle d'un coup le fond, le cadre, l'image et la couleur d'accent du Studio. Tu peux ensuite ajuster chaque effet.</p></div>
      ${active ? '<button class="btn" data-action="clear-theme">Retirer le thème</button>' : ''}
    </header>
    <div class="themes">${cards}</div>
  </div>`;
}

function settingControl(pluginId, field) {
  const value = state.values[pluginId]?.[field.key] ?? fieldDefault(field);
  const id = `f-${cssId(pluginId)}-${cssId(field.key)}`;
  if (field.type === 'toggle') {
    const on = value !== '0';
    return `<div class="setting"><span class="label" id="${id}-l">${esc(field.label)}</span>
      <div class="control"><button class="switch" role="switch" aria-checked="${on}" aria-labelledby="${id}-l"
        data-action="toggle-field" data-plugin="${esc(pluginId)}" data-key="${esc(field.key)}"></button></div><span></span></div>`;
  }
  if (field.type === 'slider') {
    const pct = ((Number(value) - field.min) / (field.max - field.min)) * 100;
    return `<div class="setting"><label for="${id}">${esc(field.label)}</label>
      <div class="control"><input type="range" id="${id}" min="${field.min}" max="${field.max}" step="${field.step ?? 1}" value="${esc(value)}"
        style="--fill:${pct}%" data-input="slider" data-plugin="${esc(pluginId)}" data-key="${esc(field.key)}" data-unit="${esc(field.unit ?? '')}"></div>
      <output for="${id}">${esc(value)}${esc(field.unit ?? '')}</output></div>`;
  }
  if (field.type === 'choice') {
    return `<div class="setting"><span class="label">${esc(field.label)}</span>
      <div class="control"><div class="segmented" role="group" aria-label="${esc(field.label)}">
        ${field.options.map((o) => `<button data-action="choice" data-plugin="${esc(pluginId)}" data-key="${esc(field.key)}" data-value="${esc(o.value)}" aria-pressed="${value === o.value}">${esc(o.label)}</button>`).join('')}
      </div></div><span></span></div>`;
  }
  if (field.type === 'color') {
    const hex = String(value).replace('#', '');
    const rgb = `#${hex.length === 8 ? hex.slice(2) : hex}`.slice(0, 7);
    const alpha = hex.length === 8 ? parseInt(hex.slice(0, 2), 16) : 255;
    const alphaControl = field.alpha
      ? `<input type="range" min="0" max="255" value="${alpha}" style="--fill:${(alpha / 255) * 100}%" aria-label="Opacité ${esc(field.label)}"
          data-input="alpha" data-plugin="${esc(pluginId)}" data-key="${esc(field.key)}">`
      : '';
    return `<div class="setting"><label for="${id}">${esc(field.label)}</label>
      <div class="control">
        <span class="swatch-button"><span style="background:${esc(rgb)};opacity:${alpha / 255}"></span>
          <input type="color" id="${id}" value="${esc(rgb.toLowerCase())}" data-input="color" data-plugin="${esc(pluginId)}" data-key="${esc(field.key)}" data-alpha="${field.alpha ? 1 : 0}"></span>
        <span class="hex">${esc(rgb.toUpperCase())}</span>
        ${alphaControl}
      </div><output>${field.alpha ? `${Math.round((alpha / 255) * 100)} %` : ''}</output></div>`;
  }
  return '';
}

function viewPlugin(id) {
  const plugin = installedPlugins().find((p) => p.id === id);
  if (!plugin) return `<div class="view"><div class="empty"><strong>Effet introuvable</strong><p>Il a peut-être été désinstallé.</p></div></div>`;
  const { item, loaded, source } = plugin;
  document.documentElement.style.setProperty('--hero', heroColor(item));
  const fields = item.settings?.fields ?? [];
  const live = state.data?.rendering
    ? `<div class="note">${icon('phone')}<span>Recopie en cours : chaque réglage s'applique sur l'image immédiatement.</span></div>`
    : `<div class="note">${icon('info')}<span>Aucune recopie en cours. Tes réglages sont enregistrés et s'appliqueront dès qu'un appareil se connectera.</span></div>`;
  return `<div class="view">
    <div>
      <section class="detail-head">
        <div id="detail-cover">${cover(item, previewFor(id), `cover-plugin-${cssId(id)}`)}</div>
        <div>
          <h1>${esc(item.name)}</h1>
          <p class="meta">${esc(item.author ?? 'Auteur inconnu')} · v${esc(item.version ?? '?')} · ${source === 'builtin' ? 'Inclus' : 'Installé depuis la boutique'}</p>
          <div class="actions">
            ${loaded && fields.some((f) => f.key === 'enabled') ? `${switchButton(id, `Activer ${item.name}`)}<span>${state.values[id]?.enabled === '0' ? 'Désactivé' : 'Activé'}</span>` : ''}
            ${fields.length ? `<button class="btn" data-action="reset-plugin" data-id="${esc(id)}">${icon('refresh')}Valeurs par défaut</button>` : ''}
            ${source === 'user' ? `<button class="btn btn-danger" data-action="uninstall" data-type="plugin" data-id="${esc(id)}">${icon('trash')}Désinstaller</button>` : ''}
          </div>
        </div>
      </section>
    </div>
    <p class="lede">${esc(item.description ?? '')}</p>
    ${loaded ? live : `<div class="note note-warn">${icon('refresh')}<span>Cet effet sera actif après le redémarrage de LonelyScreen.</span></div>`}
    ${fields.length ? `<div class="settings">${fields.filter((f) => f.key !== 'enabled').map((f) => settingControl(id, f)).join('')}</div>` : ''}
  </div>`;
}

function viewApp(id) {
  const app = (state.data?.apps ?? []).find((a) => a.id === id);
  if (!app) return `<div class="view"><div class="empty"><strong>App introuvable</strong><p>Elle a peut-être été désinstallée.</p></div></div>`;
  const item = { type: 'app', id, ...app.manifest };
  return `<div class="app-view">
    <div class="app-head">
      <h1>${esc(item.name ?? id)}</h1><span class="spacer"></span>
      <button class="btn btn-quiet" data-action="uninstall" data-type="app" data-id="${esc(id)}">${icon('trash')}Désinstaller</button>
    </div>
    <iframe class="app-frame" id="app-frame" src="${esc(app.url)}" title="${esc(item.name ?? id)}" sandbox="allow-scripts allow-forms"></iframe>
  </div>`;
}

function updatePanel() {
  const { status, info, error } = state.update;
  const check = `<button class="btn" data-action="update-check">${icon('refresh')}Rechercher une mise à jour</button>`;
  const lines = {
    idle: check,
    checking: '<button class="btn" disabled><span class="spinner"></span>Recherche…</button>',
    uptodate: `<span class="btn btn-done">${icon('check')}À jour</span>${check}`,
    available: `<button class="btn btn-primary" data-action="update-apply">${icon('download')}Installer la version ${esc(info?.latest)}</button>`,
    applying: '<button class="btn" disabled><span class="spinner"></span>Téléchargement et vérification…</button>',
    ready: `<span class="btn btn-done">${icon('check')}Prête : redémarre LonelyScreen</span>`,
    error: check,
    unavailable: '',
  }[status] ?? check;
  const notes = status === 'available' && info?.notes ? `<p class="lede">${esc(info.notes)}</p>` : '';
  const stub = info?.stubUpdateRequired
    ? `<div class="note note-warn">${icon('alert')}<span>Cette version demande de relancer la commande d'installation dans PowerShell :<br><kbd>${esc(INSTALL_COMMAND)}</kbd></span></div>`
    : '';
  const failure = (status === 'error' || status === 'unavailable') && error
    ? `<div class="note note-warn">${icon('alert')}<span>${esc(error)}</span></div>` : '';
  return `<div class="pref-actions">${lines}</div>${notes}${stub}${failure}`;
}

function viewSettings() {
  const config = state.data?.config ?? {};
  const mode = config.accentMode === 'custom' ? 'custom' : 'theme';
  const accent = currentAccent();
  return `<div class="view">
    <header><h1>Réglages du Studio</h1></header>
    <div class="prefs">
      <section class="pref-block">
        <h2>Couleur d'accent</h2>
        <p>Elle colore les boutons, la sélection et l'onglet du Studio. Par défaut, elle suit le thème actif.</p>
        <div class="accents" role="group" aria-label="Couleur d'accent">
          <button class="btn${mode === 'theme' ? ' btn-primary' : ''}" data-action="accent-theme" aria-pressed="${mode === 'theme'}">Suivre le thème</button>
          ${ACCENT_PRESETS.map((hex) => `<button class="accent-dot" style="background:${hex}" data-action="accent" data-value="${hex}"
            aria-pressed="${mode === 'custom' && accent.toUpperCase() === hex}" aria-label="Accent ${hex}"></button>`).join('')}
          <span class="swatch-button" title="Couleur personnalisée"><span style="background:${esc(accent)}"></span>
            <input type="color" value="${esc(accent.toLowerCase())}" data-input="accent" aria-label="Couleur d'accent personnalisée"></span>
        </div>
      </section>
      <section class="pref-block">
        <h2>Catalogue en ligne</h2>
        <p>Adresse HTTPS d'un fichier catalog.json, par exemple sur un dépôt GitHub. Ses extensions s'ajoutent à la boutique ; chaque fichier doit porter une empreinte SHA-256, vérifiée avant installation.</p>
        <form class="field" data-form="catalog">
          <input type="url" name="url" placeholder="https://raw.githubusercontent.com/…/catalog.json" value="${esc(config.catalogUrl ?? '')}" pattern="https://.*" aria-label="Adresse du catalogue">
          <button class="btn btn-primary" type="submit">Enregistrer</button>
        </form>
      </section>
      <section class="pref-block">
        <h2>Raccourci</h2>
        <p>Ouvre ou ferme le Studio avec <kbd>Ctrl</kbd> <kbd>Maj</kbd> <kbd>P</kbd> dans LonelyScreen, avec l'onglet collé au bord de la fenêtre, ou par le menu de la fenêtre.</p>
      </section>
      <section class="pref-block">
        <h2>Données</h2>
        <p class="path">${esc(state.data?.configDirectory ?? '')}</p>
        <div class="pref-actions">
          <button class="btn" data-action="open-folder">${icon('folder')}Ouvrir le dossier</button>
          <button class="btn btn-danger" data-action="reset-all">${icon('refresh')}${state.confirmReset ? 'Confirmer : tout remettre par défaut' : 'Remettre les effets par défaut'}</button>
        </div>
      </section>
      <section class="pref-block">
        <h2>Mises à jour</h2>
        <p>Version ${esc(state.data?.version ?? '?')}. Chaque mise à jour est signée par l'auteur et vérifiée avant installation ; elle s'applique au prochain démarrage de LonelyScreen, sans droits administrateur.</p>
        ${updatePanel()}
      </section>
      <section class="pref-block">
        <h2>À propos</h2>
        <p>LonelyScreen Studio ${esc(state.data?.version ?? '')} · API des plugins v1 · ${isNative ? 'intégré à LonelyScreen' : 'aperçu hors de LonelyScreen'}. Module communautaire, non affilié à IMTIGER Technologies, éditeur de LonelyScreen.</p>
      </section>
    </div>
  </div>`;
}

// ---------------------------------------------------------------------------
// Rendu
// ---------------------------------------------------------------------------

const NAV = [
  ['home', 'Accueil', 'home'],
  ['store', 'Boutique', 'store'],
  ['library', 'Bibliothèque', 'library'],
  ['themes', 'Thèmes', 'themes'],
];

function renderRail() {
  const current = state.route.name;
  const apps = state.data?.apps ?? [];
  const live = state.data?.rendering;
  $('#rail').innerHTML = `
    <div class="brand">${BRAND_MARK}<div class="brand-name"><strong>Studio</strong><span>LonelyScreen</span></div></div>
    <nav class="nav" aria-label="Navigation">
      ${NAV.map(([route, label, ico]) => `<button class="nav-item" data-action="nav" data-route="${route}" aria-current="${current === route ? 'page' : 'false'}" title="${label}">
        ${icon(ico)}<span>${label}</span>${route === 'library' ? `<span class="count">${(state.data?.plugins?.length ?? 0) + (state.data?.themes?.length ?? 0) + apps.length}</span>` : ''}</button>`).join('')}
    </nav>
    <div class="rail-group">
      <h2>Apps</h2>
      <div class="rail-apps">
        ${apps.length ? apps.map((app) => `<button class="nav-item app-link" data-action="open-app" data-id="${esc(app.id)}" title="${esc(app.manifest?.name ?? app.id)}"
            aria-current="${current === 'app' && state.route.id === app.id ? 'page' : 'false'}">
            <span class="mini-cover">${coverSvg({ id: app.id, ...app.manifest })}</span><span>${esc(app.manifest?.name ?? app.id)}</span></button>`).join('')
          : '<p class="rail-empty">Installe une app depuis la boutique pour l\'ajouter ici.</p>'}
      </div>
    </div>
    <div class="rail-foot">
      <button class="nav-item" data-action="nav" data-route="settings" aria-current="${current === 'settings' ? 'page' : 'false'}" title="Réglages">${icon('settings')}<span>Réglages</span></button>
      <div class="live" data-state="${live ? 'on' : 'off'}" role="status">
        <span class="live-dot"></span><strong>${live ? 'Recopie en cours' : 'En attente d\'un appareil'}</strong>
        <p>${live ? 'Les effets s\'appliquent en direct.' : 'Les effets s\'afficheront dès la connexion.'}</p>
      </div>
    </div>`;
}

function renderContent() {
  const { name, id } = state.route;
  const views = {
    home: viewHome,
    store: viewStore,
    library: viewLibrary,
    themes: viewThemes,
    settings: viewSettings,
    plugin: () => viewPlugin(id),
    app: () => viewApp(id),
  };
  if (!['home', 'plugin'].includes(name)) document.documentElement.style.setProperty('--hero', heroColor(activeTheme() ?? {}));
  const content = $('#content');
  const scrolls = [...content.querySelectorAll('.rail-scroll[data-row]')].map((el) => [el.dataset.row, el.scrollLeft]);
  content.innerHTML = (views[name] ?? viewHome)();
  scrolls.forEach(([rowId, left]) => {
    const el = railById(rowId);
    if (el) el.scrollLeft = left;
  });
  updateRailArrows();
}

function railById(rowId) {
  return document.querySelector(`.rail-scroll[data-row="${CSS.escape(rowId)}"]`);
}

/** Active ou désactive les flèches ‹ › selon la position de chaque rangée ; les masque si tout tient. */
function updateRailArrows(rail) {
  const rails = rail ? [rail] : [...document.querySelectorAll('.rail-scroll[data-row]')];
  rails.forEach((el) => {
    const arrows = el.closest('.row')?.querySelector('.row-arrows');
    if (!arrows) return;
    const max = el.scrollWidth - el.clientWidth;
    arrows.hidden = max <= RAIL_EDGE_TOLERANCE_PX;
    const [previous, next] = arrows.querySelectorAll('button');
    previous.disabled = el.scrollLeft <= RAIL_EDGE_TOLERANCE_PX;
    next.disabled = el.scrollLeft >= max - RAIL_EDGE_TOLERANCE_PX;
  });
}

function render() {
  if (!state.data) return;
  renderRail();
  renderContent();
  $('#back').disabled = state.history.length === 0;
}

function navigate(route, { transition = true } = {}) {
  const same = route.name === state.route.name && route.id === state.route.id;
  if (same) return;
  state.history.push(state.route);
  state.route = route;
  state.openTile = null;
  const update = () => {
    render();
    $('#content').scrollTop = 0;
  };
  if (transition && document.startViewTransition && !matchMedia('(prefers-reduced-motion: reduce)').matches) {
    document.startViewTransition(update);
  } else {
    update();
  }
}

function goBack() {
  const previous = state.history.pop();
  if (!previous) return;
  state.route = previous;
  const update = () => render();
  if (document.startViewTransition) document.startViewTransition(update);
  else update();
}

// ---------------------------------------------------------------------------
// Notifications
// ---------------------------------------------------------------------------

function toast(message, tone = 'info') {
  const element = document.createElement('div');
  element.className = 'toast';
  element.dataset.tone = tone;
  element.setAttribute('role', tone === 'error' ? 'alert' : 'status');
  element.innerHTML = `${icon(tone === 'error' ? 'alert' : 'check')}<span>${esc(message)}</span>`;
  $('#toasts').append(element);
  setTimeout(() => {
    element.classList.add('leaving');
    element.addEventListener('animationend', () => element.remove(), { once: true });
  }, TOAST_MS);
}

// ---------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------

/**
 * Ouvre (ou ferme avec null) une tuile.
 * @param {string|null} key
 * @param {{reveal?: boolean}} [options] reveal : faire défiler la rangée (et elle seule,
 *   jamais la page) pour montrer la tuile ouverte. Réservé aux ouvertures volontaires
 *   (clic, clavier) : au survol, déplacer le contenu sous la souris gênerait le défilement.
 */
function setOpenTile(key, { reveal = false } = {}) {
  state.openTile = key;
  document.querySelectorAll('.tile').forEach((el) => {
    const open = el.dataset.key === key;
    el.classList.toggle('is-open', open);
    el.querySelector('.tile-hit')?.setAttribute('aria-expanded', String(open));
    el.querySelector('.tile-detail')?.setAttribute('aria-hidden', String(!open));
  });
  if (key && reveal) {
    const tileEl = document.querySelector(`.tile[data-key="${CSS.escape(key)}"]`);
    const rail = tileEl?.closest('.rail-scroll');
    setTimeout(() => {
      if (!tileEl || !rail) return;
      const overflowRight = tileEl.offsetLeft + tileEl.offsetWidth - (rail.scrollLeft + rail.clientWidth);
      const overflowLeft = rail.scrollLeft - tileEl.offsetLeft;
      if (overflowRight > 0) rail.scrollBy({ left: overflowRight + RAIL_REVEAL_MARGIN_PX, behavior: 'smooth' });
      else if (overflowLeft > 0) rail.scrollBy({ left: -(overflowLeft + RAIL_REVEAL_MARGIN_PX), behavior: 'smooth' });
    }, TILE_REVEAL_DELAY_MS);
  }
  updateRailArrows();
}

async function install(item, { quiet = false } = {}) {
  const key = keyOf(item);
  state.busy.add(key);
  render();
  try {
    const result = await call('install', { item });
    if (result?.restartRequired) state.restart.add(item.name);
    await refresh();
    if (!quiet) {
      toast(result?.restartRequired ? `${item.name} installé : redémarre LonelyScreen pour l'activer.` : `${item.name} installé.`);
    }
    return true;
  } catch (error) {
    toast(`Installation impossible : ${error.message}`, 'error');
    return false;
  } finally {
    state.busy.delete(key);
    render();
  }
}

async function applyTheme(id) {
  const theme = themeById(id);
  if (!theme) return;
  const key = keyOf({ type: 'theme', id });
  if (!isInstalled({ type: 'theme', id }) && !(await install({ ...theme, type: 'theme' }, { quiet: true }))) return;
  state.busy.add(key);
  render();
  try {
    for (const entry of theme.settings ?? []) {
      await call('writeIni', { file: entry.file, section: entry.section, values: entry.values });
    }
    await saveConfig({ activeTheme: id });
    await loadPluginValues();
    applyAccent();
    toast(`Thème « ${theme.name} » appliqué.`);
  } catch (error) {
    toast(`Thème non appliqué : ${error.message}`, 'error');
  } finally {
    state.busy.delete(key);
    render();
  }
}

async function clearTheme() {
  try {
    await call('resetSettings');
    await saveConfig({ activeTheme: null });
    await loadPluginValues();
    applyAccent();
    render();
    toast('Thème retiré : effets remis par défaut.');
  } catch (error) {
    toast(error.message, 'error');
  }
}

const writeTimers = new Map();

/** Enregistre un réglage de plugin (regroupé pour les curseurs) et met l'aperçu à jour. */
function writeSetting(pluginId, key, value, { immediate = false } = {}) {
  const plugin = installedPlugins().find((p) => p.id === pluginId);
  const settings = plugin?.item.settings;
  if (!settings) return;
  state.values[pluginId] = { ...(state.values[pluginId] ?? {}), [key]: String(value) };
  refreshPreviews(pluginId);
  const timerKey = `${pluginId}/${key}`;
  clearTimeout(writeTimers.get(timerKey));
  const run = () => call('writeIni', { file: settings.file, section: settings.section, values: { [key]: String(value) } })
    .catch((error) => toast(`Réglage non enregistré : ${error.message}`, 'error'));
  if (immediate) run();
  else writeTimers.set(timerKey, setTimeout(run, WRITE_DEBOUNCE_MS));
}

/** Met à jour les pochettes d'aperçu sans re-rendre la page (les curseurs gardent le focus). */
function refreshPreviews(pluginId) {
  const holder = $('#detail-cover .cover');
  if (holder && state.route.name === 'plugin' && state.route.id === pluginId) {
    const plugin = installedPlugins().find((p) => p.id === pluginId);
    holder.innerHTML = coverSvg(plugin.item, previewFor(pluginId));
  }
}

function confirmReset() {
  if (!state.confirmReset) {
    state.confirmReset = true;
    render();
    setTimeout(() => { state.confirmReset = false; render(); }, CONFIRM_WINDOW_MS);
    return;
  }
  state.confirmReset = false;
  clearTheme();
}

const actions = {
  nav: (el) => navigate({ name: el.dataset.route }),
  back: () => goBack(),
  'open-tile': (el) => setOpenTile(state.openTile === el.dataset.key ? null : el.dataset.key, { reveal: true }),
  'rail-scroll': (el) => {
    const rail = railById(el.dataset.row);
    if (rail) rail.scrollBy({ left: Number(el.dataset.direction) * rail.clientWidth * RAIL_PAGE_RATIO, behavior: 'smooth' });
  },
  install: (el) => {
    const item = catalogItems().find((i) => keyOf(i) === el.dataset.key);
    if (item) install(item);
  },
  'apply-theme': (el) => applyTheme(el.dataset.id),
  'clear-theme': () => clearTheme(),
  'open-plugin': (el) => navigate({ name: 'plugin', id: el.dataset.id }),
  'open-app': (el) => navigate({ name: 'app', id: el.dataset.id }),
  'toggle-plugin': (el) => {
    const on = el.getAttribute('aria-checked') !== 'true';
    writeSetting(el.dataset.id, 'enabled', on ? '1' : '0', { immediate: true });
    render();
  },
  'toggle-field': (el) => {
    const on = el.getAttribute('aria-checked') !== 'true';
    writeSetting(el.dataset.plugin, el.dataset.key, on ? '1' : '0', { immediate: true });
    render();
  },
  choice: (el) => {
    writeSetting(el.dataset.plugin, el.dataset.key, el.dataset.value, { immediate: true });
    render();
  },
  'reset-plugin': async (el) => {
    const plugin = installedPlugins().find((p) => p.id === el.dataset.id);
    const { file, section, fields } = plugin.item.settings;
    const values = Object.fromEntries(fields.map((f) => [f.key, fieldDefault(f)]));
    await call('writeIni', { file, section, values });
    state.values[plugin.id] = values;
    render();
    toast(`${plugin.item.name} : valeurs par défaut restaurées.`);
  },
  uninstall: async (el) => {
    try {
      const result = await call('uninstall', { type: el.dataset.type, id: el.dataset.id });
      if (el.dataset.type === 'theme' && state.data.config.activeTheme === el.dataset.id) await saveConfig({ activeTheme: null });
      if (state.route.name === 'app' || state.route.name === 'plugin') state.route = { name: 'library' };
      await refresh();
      toast(result.restartRequired ? 'Suppression programmée au prochain démarrage de LonelyScreen.' : 'Supprimé.');
    } catch (error) {
      toast(error.message, 'error');
    }
  },
  'store-filter': (el) => { state.storeFilter = el.dataset.value; render(); },
  'library-tab': (el) => { state.libraryTab = el.dataset.value; render(); },
  'clear-search': () => { state.query = ''; $('#search').value = ''; render(); },
  'accent-theme': async () => { await saveConfig({ accentMode: 'theme' }); applyAccent(); render(); },
  accent: async (el) => { await saveConfig({ accentMode: 'custom', accent: el.dataset.value }); applyAccent(); render(); },
  'open-folder': () => call('openFolder', { which: 'config' }),
  'update-check': () => checkForUpdate(),
  'update-apply': () => applyUpdate(),
  'reset-all': () => confirmReset(),
  close: () => call('close'),
};

document.addEventListener('click', (event) => {
  const target = event.target.closest('[data-action]');
  if (!target || target.disabled) return;
  const handler = actions[target.dataset.action];
  if (handler) {
    event.preventDefault();
    handler(target);
  }
});

document.addEventListener('input', (event) => {
  const el = event.target;
  const kind = el.dataset?.input;
  if (!kind) return;
  if (kind === 'slider') {
    el.style.setProperty('--fill', `${((el.value - el.min) / (el.max - el.min)) * 100}%`);
    el.closest('.setting').querySelector('output').textContent = `${el.value}${el.dataset.unit}`;
    writeSetting(el.dataset.plugin, el.dataset.key, el.value);
  } else if (kind === 'alpha' || kind === 'color') {
    const setting = el.closest('.setting');
    const colorInput = setting.querySelector('input[type="color"]');
    const alphaInput = setting.querySelector('input[data-input="alpha"]');
    const rgb = colorInput.value.toUpperCase();
    const alpha = alphaInput ? Number(alphaInput.value) : 255;
    if (alphaInput) alphaInput.style.setProperty('--fill', `${(alpha / 255) * 100}%`);
    setting.querySelector('.swatch-button span').style.cssText = `background:${rgb};opacity:${alpha / 255}`;
    setting.querySelector('.hex').textContent = rgb;
    setting.querySelector('output').textContent = alphaInput ? `${Math.round((alpha / 255) * 100)} %` : '';
    const value = colorInput.dataset.alpha === '1' ? `#${alpha.toString(16).padStart(2, '0').toUpperCase()}${rgb.slice(1)}` : rgb;
    writeSetting(colorInput.dataset.plugin, colorInput.dataset.key, value);
  } else if (kind === 'accent') {
    document.documentElement.style.setProperty('--accent', el.value);
  }
});

document.addEventListener('change', async (event) => {
  if (event.target.dataset?.input === 'accent') {
    await saveConfig({ accentMode: 'custom', accent: event.target.value.toUpperCase() });
    applyAccent();
    render();
  }
});

document.addEventListener('submit', async (event) => {
  if (event.target.dataset.form !== 'catalog') return;
  event.preventDefault();
  const url = new FormData(event.target).get('url').trim();
  if (url && !url.startsWith('https://')) {
    toast('L\'adresse doit commencer par https://', 'error');
    return;
  }
  await saveConfig({ catalogUrl: url || null });
  toast(url ? 'Catalogue enregistré, chargement…' : 'Catalogue en ligne retiré.');
  loadRemoteCatalog();
});

// Défilement : pendant qu'on fait défiler, le contenu glisse sous la souris ; on suspend
// alors l'ouverture des tuiles au survol pour que rien ne bouge sous le pointeur.
let lastScrollAt = 0;
const noteScroll = () => {
  lastScrollAt = Date.now();
  clearTimeout(hoverTimer);
};
document.addEventListener('wheel', noteScroll, { passive: true, capture: true });
document.addEventListener('scroll', (event) => {
  noteScroll();
  if (event.target.classList?.contains('rail-scroll')) updateRailArrows(event.target);
}, { passive: true, capture: true });
window.addEventListener('resize', () => updateRailArrows());

// Survol prolongé d'une tuile (souris immobile) : elle s'ouvre comme sur un catalogue de streaming.
let hoverTimer = null;
document.addEventListener('pointerover', (event) => {
  const tileEl = event.target.closest('.tile');
  clearTimeout(hoverTimer);
  if (!tileEl || tileEl.classList.contains('is-open') || event.pointerType !== 'mouse' || dragState.moved) return;
  hoverTimer = setTimeout(() => {
    if (Date.now() - lastScrollAt >= SCROLL_HOVER_PAUSE_MS) setOpenTile(tileEl.dataset.key);
  }, HOVER_OPEN_DELAY_MS);
});

// Glisser une rangée à la souris pour la faire défiler (un petit déplacement reste un clic).
const dragState = { rail: null, startX: 0, startLeft: 0, moved: false };
document.addEventListener('pointerdown', (event) => {
  const rail = event.target.closest('.rail-scroll');
  if (!rail || event.pointerType !== 'mouse' || event.button !== 0 || event.target.closest('button:not(.tile-hit), a, input')) return;
  Object.assign(dragState, { rail, startX: event.clientX, startLeft: rail.scrollLeft, moved: false });
});
document.addEventListener('pointermove', (event) => {
  const { rail } = dragState;
  if (!rail) return;
  const dx = event.clientX - dragState.startX;
  if (!dragState.moved && Math.abs(dx) < DRAG_THRESHOLD_PX) return;
  if (!dragState.moved) {
    dragState.moved = true;
    clearTimeout(hoverTimer);
    rail.classList.add('is-dragging');
  }
  rail.scrollLeft = dragState.startLeft - dx;
});
document.addEventListener('pointerup', () => {
  if (!dragState.rail) return;
  dragState.rail.classList.remove('is-dragging');
  dragState.rail = null;
  // Le clic qui suit un glisser ne doit pas ouvrir de tuile.
  if (dragState.moved) setTimeout(() => { dragState.moved = false; }, 0);
});
document.addEventListener('click', (event) => {
  if (dragState.moved && event.target.closest('.rail-scroll')) {
    event.stopPropagation();
    event.preventDefault();
  }
}, true);
document.addEventListener('pointerout', (event) => {
  const row = event.target.closest('.rail-scroll');
  if (row && !row.contains(event.relatedTarget)) {
    clearTimeout(hoverTimer);
    if (state.openTile && row.querySelector('.tile.is-open')) setOpenTile(null);
  }
});

document.addEventListener('keydown', (event) => {
  if (event.key === 'Escape') {
    if (state.openTile) setOpenTile(null);
    else if (state.route.name !== 'home') goBack();
  }
  if ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === 'f') {
    event.preventDefault();
    $('#search').focus();
  }
});

// Pont pour les apps hébergées (iframe) : écriture de réglages uniquement.
window.addEventListener('message', async (event) => {
  const frame = $('#app-frame');
  const message = event.data;
  if (!frame || event.source !== frame.contentWindow || message?.type !== 'ls:writeIni') return;
  try {
    await call('writeIni', { file: message.file, section: message.section, values: message.values });
    await loadPluginValues();
    event.source.postMessage({ type: 'ls:reply', requestId: message.requestId, ok: true }, '*');
  } catch (error) {
    event.source.postMessage({ type: 'ls:reply', requestId: message.requestId, ok: false, error: error.message }, '*');
  }
});

$('#search').addEventListener('input', (event) => {
  state.query = event.target.value;
  if (state.route.name !== 'store') {
    state.history.push(state.route);
    state.route = { name: 'store' };
  }
  render();
  $('#search').focus();
});

onEvent((event) => {
  if (event.event === 'opened') refresh();
});

// Rafraîchit l'état de la recopie pendant que le panneau est ouvert.
setInterval(async () => {
  if (document.hidden || !state.data) return;
  const next = await call('state').catch(() => null);
  if (next && next.rendering !== state.data.rendering) {
    state.data.rendering = next.rendering;
    renderRail();
  }
}, STATE_POLL_MS);

$('#close').innerHTML = icon('close');
$('#back').innerHTML = icon('back');
$('#search-icon').innerHTML = icon('search');

refresh()
  .then(() => {
    loadRemoteCatalog();
    if (isNative) checkForUpdate({ silent: true });
  })
  .catch((error) => {
    $('#content').innerHTML = `<div class="view"><div class="empty"><strong>Le Studio n'a pas pu démarrer</strong><p>${esc(error.message)}</p></div></div>`;
  });
