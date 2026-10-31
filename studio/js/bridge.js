/**
 * Pont entre l'interface et le chargeur natif (WebView2).
 * Hors de LonelyScreen (navigateur de développement), un mode simulé rejoue
 * le même contrat à partir du catalogue local, avec stockage dans localStorage.
 * @module bridge
 */

const webview = window.chrome?.webview;
const pending = new Map();
const listeners = new Set();
let sequence = 0;

/** Vrai quand le Studio tourne réellement dans LonelyScreen. */
export const isNative = Boolean(webview);

if (webview) {
  webview.addEventListener('message', (event) => {
    const message = event.data;
    if (message && message.id != null && pending.has(message.id)) {
      const { resolve, reject } = pending.get(message.id);
      pending.delete(message.id);
      if (message.ok) resolve(message.result);
      else reject(new Error(message.error || 'Erreur inconnue'));
    } else if (message?.event) {
      listeners.forEach((listener) => listener(message));
    }
  });
}

/**
 * Appelle une opération du chargeur.
 * @param {string} op Nom de l'opération (state, readIni, writeIni, install…).
 * @param {object} [args]
 * @returns {Promise<any>}
 */
export function call(op, args = {}) {
  if (!webview) return mock(op, args);
  return new Promise((resolve, reject) => {
    sequence += 1;
    pending.set(sequence, { resolve, reject });
    webview.postMessage({ id: sequence, op, args });
  });
}

/**
 * S'abonne aux événements poussés par le chargeur (ex. ouverture du panneau).
 * @param {(event: {event: string}) => void} listener
 * @returns {() => void} Fonction de désabonnement.
 */
export function onEvent(listener) {
  listeners.add(listener);
  return () => listeners.delete(listener);
}

// ---------------------------------------------------------------------------
// Mode simulé (développement hors LonelyScreen)
// ---------------------------------------------------------------------------

const MOCK_KEY = 'ls-studio-mock';
const MOCK_DELAY_MS = 450;

function mockStore() {
  try {
    return JSON.parse(localStorage.getItem(MOCK_KEY)) ?? { ini: {}, installed: { theme: [], app: [] }, config: {} };
  } catch {
    return { ini: {}, installed: { theme: [], app: [] }, config: {} };
  }
}

function saveMock(store) {
  localStorage.setItem(MOCK_KEY, JSON.stringify(store));
}

async function mock(op, args) {
  const store = mockStore();
  const catalog = await fetch('catalog/catalog.json').then((r) => r.json());
  const wait = () => new Promise((r) => setTimeout(r, MOCK_DELAY_MS));
  switch (op) {
    case 'state': {
      const byType = (type) => catalog.items.filter((item) => item.type === type);
      return {
        plugins: byType('plugin').map((item) => ({ id: item.id, source: 'builtin', loaded: true, pendingRemoval: false, manifest: item })),
        themes: byType('theme').filter((t) => store.installed.theme.includes(t.id)).map((t) => ({ id: t.id, manifest: t })),
        apps: byType('app').filter((a) => store.installed.app.includes(a.id))
          .map((a) => ({ id: a.id, manifest: a, url: `catalog/apps/${a.id}/index.html` })),
        config: store.config,
        localCatalog: catalog,
        rendering: false,
        configDirectory: 'C:\\Users\\vous\\AppData\\Roaming\\LonelyScreenPlugins',
      };
    }
    case 'readIni': {
      const section = store.ini[`${args.file}/${args.section}`] ?? {};
      return Object.fromEntries(args.keys.filter((k) => k in section).map((k) => [k, section[k]]));
    }
    case 'writeIni': {
      const key = `${args.file}/${args.section}`;
      store.ini[key] = { ...(store.ini[key] ?? {}), ...args.values };
      saveMock(store);
      return { saved: true };
    }
    case 'saveConfig':
      store.config = args.config;
      saveMock(store);
      return { saved: true };
    case 'install':
      await wait();
      if (!store.installed[args.item.type]?.includes(args.item.id)) store.installed[args.item.type]?.push(args.item.id);
      saveMock(store);
      return { installed: true, restartRequired: args.item.type === 'plugin' };
    case 'uninstall':
      store.installed[args.type] = (store.installed[args.type] ?? []).filter((id) => id !== args.id);
      saveMock(store);
      return { removed: true, restartRequired: false };
    case 'resetSettings':
      store.ini = {};
      saveMock(store);
      return { reset: true };
    case 'fetch':
      await wait();
      throw new Error('Mode aperçu : pas d\'accès réseau.');
    default:
      return {};
  }
}
