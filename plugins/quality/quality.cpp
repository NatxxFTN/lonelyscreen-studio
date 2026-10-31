/**
 * @file quality.cpp
 * Plugin "quality" : amélioration de l'image affichée.
 *  1. Filtrage : bilinéaire ou anisotrope au lieu de "plus proche voisin"
 *     quand la vidéo est agrandie ou réduite.
 *  2. Post-traitement GPU (pixel shader) : netteté (masque flou),
 *     contraste, saturation, luminosité.
 * Ça rend l'image plus nette et plus vive, mais ça n'ajoute pas de détails
 * absents de la source.
 * Réglages : %APPDATA%\LonelyScreenPlugins\quality.ini
 */
#include <algorithm>

#include <d3dcompiler.h>

#include "lsplugin_util.h"

namespace {

constexpr wchar_t kIniFile[] = L"quality.ini";
constexpr wchar_t kSection[] = L"quality";
constexpr char kLogName[] = "quality";
constexpr DWORD kSamplersToConfigure = 4;
constexpr int kDefaultMaxAnisotropy = 8;

// Réglages en pourcentage dans le .ini, convertis en facteurs pour le shader.
constexpr int kDefaultSharpen = 40;      // 0..100
constexpr int kDefaultContrast = 100;    // 50..150
constexpr int kDefaultSaturation = 100;  // 0..200
constexpr int kDefaultBrightness = 0;    // -50..50
constexpr int kMinContrast = 50;
constexpr int kMaxContrast = 150;
constexpr int kMaxSaturation = 200;
constexpr int kMaxBrightness = 50;
constexpr int kMaxPercent = 100;
constexpr float kPercent = 100.0f;
constexpr float kMaxSharpenStrength = 1.5f;
constexpr float kPixelCenterOffset = 0.5f;
constexpr DWORD kQuadFvf = D3DFVF_XYZRHW | D3DFVF_TEX1;
constexpr DWORD kAllColorChannels =
    D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE | D3DCOLORWRITEENABLE_ALPHA;

// c0 = (1/largeur, 1/hauteur, netteté, -) ; c1 = (contraste, saturation, luminosité, -)
constexpr char kShaderSource[] = R"(
sampler2D source : register(s0);
float4 texelAndSharpen : register(c0);
float4 grading : register(c1);

float4 main(float2 uv : TEXCOORD0) : COLOR0 {
    float3 center = tex2D(source, uv).rgb;
    float3 around = tex2D(source, uv + float2(texelAndSharpen.x, 0)).rgb
                  + tex2D(source, uv - float2(texelAndSharpen.x, 0)).rgb
                  + tex2D(source, uv + float2(0, texelAndSharpen.y)).rgb
                  + tex2D(source, uv - float2(0, texelAndSharpen.y)).rgb;
    float3 color = center + (center * 4.0 - around) * texelAndSharpen.z;
    float luma = dot(color, float3(0.299, 0.587, 0.114));
    color = lerp(luma.xxx, color, grading.y);
    color = (color - 0.5) * grading.x + 0.5 + grading.z;
    return float4(saturate(color), 1.0);
}
)";

struct QuadVertex {
    float x, y, z, rhw;
    float u, v;
};

const LsHostApi *g_host = nullptr;

// Réglages.
bool g_enabled = true;
bool g_wantAnisotropic = true;
int g_requestedAnisotropy = kDefaultMaxAnisotropy;
float g_sharpen = 0.0f;
float g_contrast = 1.0f;
float g_saturation = 1.0f;
float g_brightness = 0.0f;

// Valeurs effectives selon la carte graphique.
D3DTEXTUREFILTERTYPE g_minFilter = D3DTEXF_LINEAR;
D3DTEXTUREFILTERTYPE g_magFilter = D3DTEXF_LINEAR;
DWORD g_maxAnisotropy = 1;
bool g_linearStretch = false;

// Ressources GPU du post-traitement.
IDirect3DDevice9 *g_shaderDevice = nullptr;
IDirect3DPixelShader9 *g_shader = nullptr;
bool g_shaderFailed = false;
IDirect3DTexture9 *g_copy = nullptr;
UINT g_copyWidth = 0;
UINT g_copyHeight = 0;
D3DFORMAT g_copyFormat = D3DFMT_UNKNOWN;
bool g_inOwnDraw = false;

template <typename T> void releaseAndNull(T *&resource) {
    if (resource != nullptr) {
        resource->Release();
        resource = nullptr;
    }
}

/** Lit quality.ini. */
void loadConfig() {
    const std::wstring ini = ls::iniPath(g_host, kIniFile);
    g_enabled = ls::readIniInt(ini, kSection, L"enabled", 1) != 0;
    g_wantAnisotropic = ls::readIniString(ini, kSection, L"mode", L"anisotropic") != L"linear";
    g_requestedAnisotropy = ls::readIniInt(ini, kSection, L"max_anisotropy", kDefaultMaxAnisotropy);
    const int sharpen = std::clamp(ls::readIniInt(ini, kSection, L"sharpen", kDefaultSharpen), 0, kMaxPercent);
    const int contrast =
        std::clamp(ls::readIniInt(ini, kSection, L"contrast", kDefaultContrast), kMinContrast, kMaxContrast);
    const int saturation =
        std::clamp(ls::readIniInt(ini, kSection, L"saturation", kDefaultSaturation), 0, kMaxSaturation);
    const int brightness =
        std::clamp(ls::readIniInt(ini, kSection, L"brightness", kDefaultBrightness), -kMaxBrightness, kMaxBrightness);
    g_sharpen = static_cast<float>(sharpen) / kPercent * kMaxSharpenStrength;
    g_contrast = static_cast<float>(contrast) / kPercent;
    g_saturation = static_cast<float>(saturation) / kPercent;
    g_brightness = static_cast<float>(brightness) / kPercent;
}

bool effectIsNeutral() {
    return g_sharpen == 0.0f && g_contrast == 1.0f && g_saturation == 1.0f && g_brightness == 0.0f;
}

/** Choisit les filtres selon ce que la carte supporte. */
void detectCapabilities(IDirect3DDevice9 *device) {
    D3DCAPS9 caps = {};
    if (FAILED(device->GetDeviceCaps(&caps))) {
        return;
    }
    const bool minAniso = (caps.TextureFilterCaps & D3DPTFILTERCAPS_MINFANISOTROPIC) != 0;
    const bool magAniso = (caps.TextureFilterCaps & D3DPTFILTERCAPS_MAGFANISOTROPIC) != 0;
    g_minFilter = (g_wantAnisotropic && minAniso) ? D3DTEXF_ANISOTROPIC : D3DTEXF_LINEAR;
    g_magFilter = (g_wantAnisotropic && magAniso) ? D3DTEXF_ANISOTROPIC : D3DTEXF_LINEAR;
    const DWORD requested = static_cast<DWORD>(std::max(1, g_requestedAnisotropy));
    g_maxAnisotropy = std::min(requested, std::max<DWORD>(1, caps.MaxAnisotropy));
    g_linearStretch = (caps.StretchRectFilterCaps & D3DPTFILTERCAPS_MAGFLINEAR) != 0;
}

/** Applique les filtres aux premiers échantillonneurs (l'app peut ne jamais les régler). */
void applyDefaults(IDirect3DDevice9 *device) {
    for (DWORD sampler = 0; sampler < kSamplersToConfigure; ++sampler) {
        device->SetSamplerState(sampler, D3DSAMP_MINFILTER, g_minFilter);
        device->SetSamplerState(sampler, D3DSAMP_MAGFILTER, g_magFilter);
        device->SetSamplerState(sampler, D3DSAMP_MAXANISOTROPY, g_maxAnisotropy);
    }
}

/** Compile le pixel shader avec d3dcompiler_47.dll (présent sur Windows 10/11). */
bool ensureShader(IDirect3DDevice9 *device) {
    if (g_shader != nullptr && g_shaderDevice == device) {
        return true;
    }
    if (g_shaderFailed) {
        return false;
    }
    releaseAndNull(g_shader);
    g_shaderDevice = device;

    HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
    const auto compile =
        compiler != nullptr ? reinterpret_cast<pD3DCompile>(GetProcAddress(compiler, "D3DCompile")) : nullptr;
    if (compile == nullptr) {
        g_host->log(kLogName, "d3dcompiler_47.dll introuvable : post-traitement désactivé");
        g_shaderFailed = true;
        return false;
    }
    ID3DBlob *code = nullptr;
    ID3DBlob *errors = nullptr;
    const HRESULT result = compile(kShaderSource, sizeof(kShaderSource) - 1, "lsquality", nullptr, nullptr, "main",
                                   "ps_2_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (FAILED(result) || code == nullptr) {
        g_host->log(kLogName, errors != nullptr ? static_cast<const char *>(errors->GetBufferPointer())
                                                : "compilation du shader échouée");
        releaseAndNull(errors);
        releaseAndNull(code);
        g_shaderFailed = true;
        return false;
    }
    releaseAndNull(errors);
    const HRESULT created =
        device->CreatePixelShader(static_cast<const DWORD *>(code->GetBufferPointer()), &g_shader);
    releaseAndNull(code);
    if (FAILED(created)) {
        g_host->log(kLogName, "la carte graphique refuse le pixel shader : post-traitement désactivé");
        g_shaderFailed = true;
        return false;
    }
    g_host->log(kLogName, "post-traitement GPU prêt");
    return true;
}

/** Texture de copie de l'image, recréée si la taille ou le format change. */
bool ensureCopy(IDirect3DDevice9 *device, UINT width, UINT height, D3DFORMAT format) {
    if (g_copy != nullptr && g_copyWidth == width && g_copyHeight == height && g_copyFormat == format) {
        return true;
    }
    releaseAndNull(g_copy);
    if (FAILED(device->CreateTexture(width, height, 1, D3DUSAGE_RENDERTARGET, format, D3DPOOL_DEFAULT, &g_copy,
                                     nullptr))) {
        g_copy = nullptr;
        return false;
    }
    g_copyWidth = width;
    g_copyHeight = height;
    g_copyFormat = format;
    return true;
}

void setupEffectStates(IDirect3DDevice9 *device, UINT width, UINT height) {
    D3DVIEWPORT9 viewport = {0, 0, width, height, 0.0f, 1.0f};
    device->SetViewport(&viewport);
    device->SetVertexShader(nullptr);
    device->SetPixelShader(g_shader);
    device->SetFVF(kQuadFvf);
    device->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    device->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    device->SetRenderState(D3DRS_COLORWRITEENABLE, kAllColorChannels);
    device->SetTexture(0, g_copy);
    device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    device->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);

    const float texelAndSharpen[4] = {1.0f / static_cast<float>(width), 1.0f / static_cast<float>(height),
                                      g_sharpen, 0.0f};
    const float grading[4] = {g_contrast, g_saturation, g_brightness, 0.0f};
    device->SetPixelShaderConstantF(0, texelAndSharpen, 1);
    device->SetPixelShaderConstantF(1, grading, 1);
}

void drawFullScreenQuad(IDirect3DDevice9 *device, UINT width, UINT height) {
    const float r = static_cast<float>(width) - kPixelCenterOffset;
    const float b = static_cast<float>(height) - kPixelCenterOffset;
    const float o = -kPixelCenterOffset;
    const QuadVertex quad[4] = {
        {o, o, 0.0f, 1.0f, 0.0f, 0.0f},
        {r, o, 0.0f, 1.0f, 1.0f, 0.0f},
        {o, b, 0.0f, 1.0f, 0.0f, 1.0f},
        {r, b, 0.0f, 1.0f, 1.0f, 1.0f},
    };
    device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(QuadVertex));
}

/** Copie l'image finale, puis la redessine à travers le shader. */
void __cdecl onDrawOverlay(IDirect3DDevice9 *device, UINT width, UINT height) {
    if (!g_enabled || effectIsNeutral() || !ensureShader(device)) {
        return;
    }
    IDirect3DSurface9 *target = nullptr;
    if (FAILED(device->GetRenderTarget(0, &target)) || target == nullptr) {
        return;
    }
    D3DSURFACE_DESC desc = {};
    target->GetDesc(&desc);
    IDirect3DSurface9 *copySurface = nullptr;
    g_inOwnDraw = true;
    if (ensureCopy(device, width, height, desc.Format) && SUCCEEDED(g_copy->GetSurfaceLevel(0, &copySurface))) {
        const HRESULT copied = device->StretchRect(target, nullptr, copySurface, nullptr, D3DTEXF_NONE);
        copySurface->Release();
        if (SUCCEEDED(copied)) {
            setupEffectStates(device, width, height);
            drawFullScreenQuad(device, width, height);
            device->SetTexture(0, nullptr);
        }
    }
    g_inOwnDraw = false;
    target->Release();
}

void __cdecl onDeviceCreated(IDirect3DDevice9 *device, const D3DPRESENT_PARAMETERS *) {
    detectCapabilities(device);
    if (g_enabled) {
        applyDefaults(device);
    }
}

void __cdecl onBeforeReset(IDirect3DDevice9 *) {
    releaseAndNull(g_copy); // les ressources D3DPOOL_DEFAULT doivent être libérées avant Reset
}

void __cdecl onAfterReset(IDirect3DDevice9 *device, const D3DPRESENT_PARAMETERS *) {
    if (g_enabled) {
        applyDefaults(device);
    }
}

void __cdecl onSamplerState(IDirect3DDevice9 *, DWORD, D3DSAMPLERSTATETYPE type, DWORD *value) {
    if (!g_enabled || g_inOwnDraw) {
        return;
    }
    const bool upgradable = *value == D3DTEXF_POINT || *value == D3DTEXF_LINEAR;
    if (type == D3DSAMP_MINFILTER && upgradable) {
        *value = g_minFilter;
    } else if (type == D3DSAMP_MAGFILTER && upgradable) {
        *value = g_magFilter;
    } else if (type == D3DSAMP_MAXANISOTROPY) {
        *value = std::max(*value, g_maxAnisotropy);
    }
}

void __cdecl onStretchRect(IDirect3DDevice9 *, D3DTEXTUREFILTERTYPE *filter) {
    if (g_enabled && !g_inOwnDraw && g_linearStretch && (*filter == D3DTEXF_NONE || *filter == D3DTEXF_POINT)) {
        *filter = D3DTEXF_LINEAR;
    }
}

void __cdecl onShutdown() {
    releaseAndNull(g_copy);
    releaseAndNull(g_shader);
}

void __cdecl onConfigChanged() {
    loadConfig();
    if (g_shaderDevice != nullptr) {
        detectCapabilities(g_shaderDevice);
    }
}

const LsPlugin g_plugin = {
    .apiVersion = LS_PLUGIN_API_VERSION,
    .name = "quality",
    .version = "2.0.0",
    .onDeviceCreated = onDeviceCreated,
    .onBeforeReset = onBeforeReset,
    .onAfterReset = onAfterReset,
    .onSamplerState = onSamplerState,
    .onStretchRect = onStretchRect,
    .onDrawOverlay = onDrawOverlay,
    .onShutdown = onShutdown,
    .onConfigChanged = onConfigChanged,
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
