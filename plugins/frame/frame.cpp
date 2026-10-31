/**
 * @file frame.cpp
 * Plugin "frame" : personnalisation visuelle par-dessus l'image.
 *  - bordure colorée autour de la fenêtre ;
 *  - filtre de teinte semi-transparent (ambiance, mode nuit…).
 * Réglages : plugins\frame.ini
 */
#include <algorithm>

#include "lsplugin_util.h"

namespace {

constexpr wchar_t kIniFile[] = L"frame.ini";
constexpr wchar_t kSection[] = L"frame";
constexpr wchar_t kDefaultBorderText[] = L"#FF7C3AED";
constexpr wchar_t kDefaultTintText[] = L"#00000000";
constexpr D3DCOLOR kDefaultBorder = D3DCOLOR_ARGB(0xFF, 0x7C, 0x3A, 0xED);
constexpr D3DCOLOR kNoTint = D3DCOLOR_ARGB(0, 0, 0, 0);
constexpr int kDefaultBorderPx = 6;
constexpr int kMaxBorderPx = 64;
constexpr float kPixelCenterOffset = 0.5f;
constexpr DWORD kFvf = D3DFVF_XYZRHW | D3DFVF_DIFFUSE;
constexpr DWORD kAllColorChannels =
    D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE | D3DCOLORWRITEENABLE_ALPHA;

struct Vertex {
    float x, y, z, rhw;
    D3DCOLOR color;
};

const LsHostApi *g_host = nullptr;
bool g_enabled = true;
D3DCOLOR g_borderColor = kDefaultBorder;
int g_borderPx = kDefaultBorderPx;
D3DCOLOR g_tintColor = kNoTint;

/** Lit frame.ini. */
void loadConfig() {
    const std::wstring ini = ls::iniPath(g_host, kIniFile);
    g_enabled = ls::readIniInt(ini, kSection, L"enabled", 1) != 0;
    g_borderPx = std::clamp(ls::readIniInt(ini, kSection, L"border_px", kDefaultBorderPx), 0, kMaxBorderPx);
    if (!ls::parseColor(ls::readIniString(ini, kSection, L"border_color", kDefaultBorderText), &g_borderColor)) {
        g_host->log("frame", "border_color invalide, couleur par défaut utilisée");
        g_borderColor = kDefaultBorder;
    }
    if (!ls::parseColor(ls::readIniString(ini, kSection, L"tint_color", kDefaultTintText), &g_tintColor)) {
        g_host->log("frame", "tint_color invalide, teinte désactivée");
        g_tintColor = kNoTint;
    }
}

/** Prépare un pipeline fixe minimal : couleurs par sommet, transparence. */
void setupStates(IDirect3DDevice9 *device, UINT width, UINT height) {
    D3DVIEWPORT9 viewport = {0, 0, width, height, 0.0f, 1.0f};
    device->SetViewport(&viewport);
    device->SetVertexShader(nullptr);
    device->SetPixelShader(nullptr);
    device->SetTexture(0, nullptr);
    device->SetFVF(kFvf);
    device->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    device->SetRenderState(D3DRS_LIGHTING, FALSE);
    device->SetRenderState(D3DRS_FOGENABLE, FALSE);
    device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    device->SetRenderState(D3DRS_COLORWRITEENABLE, kAllColorChannels);
    device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
}

void drawRect(IDirect3DDevice9 *device, float left, float top, float right, float bottom, D3DCOLOR color) {
    const float l = left - kPixelCenterOffset;
    const float t = top - kPixelCenterOffset;
    const float r = right - kPixelCenterOffset;
    const float b = bottom - kPixelCenterOffset;
    const Vertex quad[4] = {
        {l, t, 0.0f, 1.0f, color},
        {r, t, 0.0f, 1.0f, color},
        {l, b, 0.0f, 1.0f, color},
        {r, b, 0.0f, 1.0f, color},
    };
    device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(Vertex));
}

void __cdecl onDrawOverlay(IDirect3DDevice9 *device, UINT width, UINT height) {
    if (!g_enabled) {
        return;
    }
    setupStates(device, width, height);
    const float w = static_cast<float>(width);
    const float h = static_cast<float>(height);

    if (ls::alphaOf(g_tintColor) > 0) {
        drawRect(device, 0.0f, 0.0f, w, h, g_tintColor);
    }
    if (g_borderPx > 0 && ls::alphaOf(g_borderColor) > 0) {
        const float px = static_cast<float>(g_borderPx);
        drawRect(device, 0.0f, 0.0f, w, px, g_borderColor);         // haut
        drawRect(device, 0.0f, h - px, w, h, g_borderColor);        // bas
        drawRect(device, 0.0f, px, px, h - px, g_borderColor);      // gauche
        drawRect(device, w - px, px, w, h - px, g_borderColor);     // droite
    }
}

const LsPlugin g_plugin = {
    .apiVersion = LS_PLUGIN_API_VERSION,
    .name = "frame",
    .version = "1.1.0",
    .onDrawOverlay = onDrawOverlay,
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
