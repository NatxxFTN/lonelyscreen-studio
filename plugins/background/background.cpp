/**
 * @file background.cpp
 * Plugin "background" : remplace la couleur de fond (zones sans vidéo,
 * bandes noires autour de l'image) par une couleur personnalisée.
 * Réglages : %APPDATA%\LonelyScreenPlugins\background.ini
 */
#include "lsplugin_util.h"

namespace {

constexpr wchar_t kIniFile[] = L"background.ini";
constexpr wchar_t kSection[] = L"background";
constexpr wchar_t kDefaultColorText[] = L"#1E1E2E";
constexpr D3DCOLOR kDefaultColor = D3DCOLOR_XRGB(0x1E, 0x1E, 0x2E);

const LsHostApi *g_host = nullptr;
bool g_enabled = true;
D3DCOLOR g_color = kDefaultColor;

/** Lit background.ini. */
void loadConfig() {
    const std::wstring ini = ls::iniPath(g_host, kIniFile);
    g_enabled = ls::readIniInt(ini, kSection, L"enabled", 1) != 0;
    if (!ls::parseColor(ls::readIniString(ini, kSection, L"color", kDefaultColorText), &g_color)) {
        g_host->log("background", "couleur invalide dans background.ini, couleur par défaut utilisée");
        g_color = kDefaultColor;
    }
}

void __cdecl onClear(IDirect3DDevice9 *, D3DCOLOR *color) {
    if (g_enabled) {
        *color = g_color;
    }
}

const LsPlugin g_plugin = {
    .apiVersion = LS_PLUGIN_API_VERSION,
    .name = "background",
    .version = "1.1.0",
    .onClear = onClear,
    .onConfigChanged = loadConfig,
};

} // namespace

/** Point d'entrée : lit la configuration et renvoie la description du plugin. */
LS_PLUGIN_EXPORT const LsPlugin *__cdecl LsPluginEntry(const LsHostApi *host) {
    if (host == nullptr || host->apiVersion != LS_PLUGIN_API_VERSION) {
        return nullptr;
    }
    g_host = host;
    loadConfig();
    return &g_plugin;
}
