/**
 * @file loader.cpp
 * Proxy d3d9.dll pour LonelyScreen.
 *
 * Placé à côté de LonelyScreen.exe, il est chargé à la place du d3d9.dll
 * système. Il :
 *  - charge le vrai d3d9.dll et intercepte quelques méthodes Direct3D 9
 *    (CreateDevice, Reset, Present, Clear, SetSamplerState, StretchRect) pour
 *    les transmettre aux plugins ;
 *  - charge les plugins livrés (install\plugins) et ceux installés par la
 *    boutique (%APPDATA%\LonelyScreenPlugins\plugins\<id>) ;
 *  - démarre le LonelyScreen Studio sur le thread d'interface (via
 *    CreateWindowExW) et ajoute "LonelyScreen Studio" au menu de la fenêtre
 *    (via TrackPopupMenuEx).
 */
#include <windows.h>
#include <d3d9.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "loader_internal.h"
#include "lsplugin.h"
#include "lsstub.h"
#include "store.h"

#ifndef LS_STUDIO_VERSION
#define LS_STUDIO_VERSION "0.0.0-dev"
#endif

// Exports du proxy, avec les noms décorés __stdcall attendus par x86.
#pragma comment(linker, "/EXPORT:Direct3DCreate9=_Proxy_Direct3DCreate9@4")
#pragma comment(linker, "/EXPORT:Direct3DCreate9Ex=_Proxy_Direct3DCreate9Ex@8")
#pragma comment(linker, "/EXPORT:D3DPERF_BeginEvent=_Proxy_D3DPERF_BeginEvent@8")
#pragma comment(linker, "/EXPORT:D3DPERF_EndEvent=_Proxy_D3DPERF_EndEvent@0")
#pragma comment(linker, "/EXPORT:D3DPERF_SetMarker=_Proxy_D3DPERF_SetMarker@8")
#pragma comment(linker, "/EXPORT:D3DPERF_SetRegion=_Proxy_D3DPERF_SetRegion@8")
#pragma comment(linker, "/EXPORT:D3DPERF_QueryRepeatFrame=_Proxy_D3DPERF_QueryRepeatFrame@0")
#pragma comment(linker, "/EXPORT:D3DPERF_SetOptions=_Proxy_D3DPERF_SetOptions@4")
#pragma comment(linker, "/EXPORT:D3DPERF_GetStatus=_Proxy_D3DPERF_GetStatus@0")

namespace {

// Indices de vtable, dans l'ordre de déclaration de d3d9.h.
constexpr UINT kD3d9CreateDevice = 16;
constexpr UINT kDeviceReset = 16;
constexpr UINT kDevicePresent = 17;
constexpr UINT kDeviceStretchRect = 34;
constexpr UINT kDeviceClear = 43;
constexpr UINT kDeviceSetSamplerState = 69;

constexpr size_t kLogLineSize = 512;
constexpr char kLoaderName[] = "loader";
constexpr wchar_t kConfigFolderName[] = L"LonelyScreenPlugins";
constexpr ULONGLONG kConfigPollIntervalMs = 1000;
constexpr ULONGLONG kRenderingWindowMs = 2000;
constexpr int kMinBootstrapWindowSize = 300;

// Entrée ajoutée au menu de la fenêtre LonelyScreen (identifiant < 0xF000, 4 bits bas à 0).
constexpr UINT kStudioMenuId = 0x7A50;
constexpr wchar_t kStudioMenuText[] = L"LonelyScreen Studio\tCtrl+Maj+P";

using TrackPopupMenuExFn = LsTrackPopupMenuExFn;
using CreateWindowExWFn = LsCreateWindowExWFn;

using Direct3DCreate9Fn = IDirect3D9 *(WINAPI *)(UINT);
using Direct3DCreate9ExFn = HRESULT(WINAPI *)(UINT, IDirect3D9Ex **);
using PerfColorStringIntFn = int(WINAPI *)(D3DCOLOR, LPCWSTR);
using PerfColorStringVoidFn = void(WINAPI *)(D3DCOLOR, LPCWSTR);
using PerfIntFn = int(WINAPI *)();
using PerfBoolFn = BOOL(WINAPI *)();
using PerfDwordFn = DWORD(WINAPI *)();
using PerfSetOptionsFn = void(WINAPI *)(DWORD);

using CreateDeviceFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3D9 *, UINT, D3DDEVTYPE, HWND, DWORD,
                                                    D3DPRESENT_PARAMETERS *, IDirect3DDevice9 **);
using ResetFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9 *, D3DPRESENT_PARAMETERS *);
using PresentFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9 *, const RECT *, const RECT *, HWND,
                                               const RGNDATA *);
using StretchRectFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9 *, IDirect3DSurface9 *, const RECT *,
                                                   IDirect3DSurface9 *, const RECT *, D3DTEXTUREFILTERTYPE);
using ClearFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9 *, DWORD, const D3DRECT *, DWORD, D3DCOLOR, float,
                                             DWORD);
using SetSamplerStateFn = HRESULT(STDMETHODCALLTYPE *)(IDirect3DDevice9 *, DWORD, D3DSAMPLERSTATETYPE, DWORD);

HMODULE g_self = nullptr;
HMODULE g_realD3d9 = nullptr;
INIT_ONCE g_initOnce = INIT_ONCE_STATIC_INIT;
std::wstring g_installDir;
std::wstring g_pluginsDir;
std::wstring g_configDir;
std::vector<const LsPlugin *> g_plugins;
std::vector<std::wstring> g_pluginIds;
bool g_hasOverlay = false;

std::atomic<bool> g_configDirty{false};
std::atomic<ULONGLONG> g_lastPresentTick{0};
std::atomic<bool> g_studioBootstrapped{false};
ULONGLONG g_lastConfigPoll = 0;
ULONGLONG g_lastConfigStamp = 0;

TrackPopupMenuExFn g_origTrackPopupMenuEx = nullptr;
CreateWindowExWFn g_origCreateWindowExW = nullptr;
int g_stubVersion = 0;

bool g_d3dHooked = false;
bool g_deviceHooked = false;
CreateDeviceFn g_origCreateDevice = nullptr;
ResetFn g_origReset = nullptr;
PresentFn g_origPresent = nullptr;
StretchRectFn g_origStretchRect = nullptr;
ClearFn g_origClear = nullptr;
SetSamplerStateFn g_origSetSamplerState = nullptr;

void __cdecl hostLog(const char *pluginName, const char *message) {
    if (g_configDir.empty()) {
        return;
    }
    const std::wstring path = g_configDir + L"\\lsplugins.log";
    FILE *file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"a") != 0 || file == nullptr) {
        return;
    }
    SYSTEMTIME now = {};
    GetLocalTime(&now);
    std::fprintf(file, "%02u:%02u:%02u [%s] %s\n", now.wHour, now.wMinute, now.wSecond,
                 pluginName != nullptr ? pluginName : "?", message != nullptr ? message : "");
    std::fclose(file);
}

const wchar_t *__cdecl hostPluginsDirectory() {
    return g_pluginsDir.c_str();
}

const wchar_t *__cdecl hostConfigDirectory() {
    return g_configDir.c_str();
}

const LsHostApi g_hostApi = {LS_PLUGIN_API_VERSION, hostLog, hostPluginsDirectory, hostConfigDirectory};

void vlog(const char *format, va_list args) {
    char line[kLogLineSize] = {};
    vsnprintf_s(line, sizeof(line), _TRUNCATE, format, args);
    hostLog(kLoaderName, line);
}

void loaderLog(const char *format, ...) {
    va_list args;
    va_start(args, format);
    vlog(format, args);
    va_end(args);
}

/**
 * Remplace une entrée de vtable COM.
 * @return l'ancienne fonction, ou nullptr en cas d'échec.
 */
void *patchVtable(void *comObject, UINT index, void *hook) {
    void **vtable = *reinterpret_cast<void ***>(comObject);
    void *original = vtable[index];
    DWORD oldProtect = 0;
    if (!VirtualProtect(&vtable[index], sizeof(void *), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        loaderLog("VirtualProtect a échoué pour l'entrée %u (erreur %lu)", index, GetLastError());
        return nullptr;
    }
    vtable[index] = hook;
    VirtualProtect(&vtable[index], sizeof(void *), oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
    return original;
}

/** Copie dans le dossier de réglages les .ini par défaut d'un dossier, sans jamais écraser. */
void seedIniFiles(const std::wstring &sourceDirectory) {
    WIN32_FIND_DATAW entry = {};
    HANDLE search = FindFirstFileW((sourceDirectory + L"\\*.ini").c_str(), &entry);
    if (search == INVALID_HANDLE_VALUE) {
        return;
    }
    do {
        CopyFileW((sourceDirectory + L"\\" + entry.cFileName).c_str(), (g_configDir + L"\\" + entry.cFileName).c_str(),
                  TRUE);
    } while (FindNextFileW(search, &entry));
    FindClose(search);
}

/**
 * Choisit le dossier de réglages (%APPDATA%\LonelyScreenPlugins, modifiable
 * sans droits admin) et y copie les .ini par défaut qui n'y sont pas encore.
 */
void prepareConfigDirectory() {
    wchar_t appData[MAX_PATH] = {};
    const DWORD length = GetEnvironmentVariableW(L"APPDATA", appData, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        g_configDir = g_pluginsDir;
        return;
    }
    g_configDir = std::wstring(appData) + L"\\" + kConfigFolderName;
    CreateDirectoryW(g_configDir.c_str(), nullptr);
    seedIniFiles(g_pluginsDir);
}

/** Date de modification la plus récente parmi les .ini de réglages. */
ULONGLONG latestConfigStamp() {
    ULONGLONG latest = 0;
    WIN32_FIND_DATAW entry = {};
    HANDLE search = FindFirstFileW((g_configDir + L"\\*.ini").c_str(), &entry);
    if (search == INVALID_HANDLE_VALUE) {
        return 0;
    }
    do {
        ULARGE_INTEGER stamp = {};
        stamp.LowPart = entry.ftLastWriteTime.dwLowDateTime;
        stamp.HighPart = entry.ftLastWriteTime.dwHighDateTime;
        latest = stamp.QuadPart > latest ? stamp.QuadPart : latest;
    } while (FindNextFileW(search, &entry));
    FindClose(search);
    return latest;
}

/** Charge une DLL de plugin si elle exporte un LsPluginEntry compatible. */
void loadPlugin(const std::wstring &path, const std::wstring &id) {
    const std::string name = store::toUtf8(id);
    HMODULE module = LoadLibraryW(path.c_str());
    if (module == nullptr) {
        loaderLog("%s : chargement impossible (erreur %lu)", name.c_str(), GetLastError());
        return;
    }
    auto entryFn = reinterpret_cast<LsPluginEntryFn>(GetProcAddress(module, LS_PLUGIN_ENTRY_NAME));
    const LsPlugin *plugin = entryFn != nullptr ? entryFn(&g_hostApi) : nullptr;
    if (plugin == nullptr || plugin->apiVersion != LS_PLUGIN_API_VERSION) {
        loaderLog("%s : ignoré (pas de LsPluginEntry valide ou version d'API différente)", name.c_str());
        FreeLibrary(module);
        return;
    }
    g_plugins.push_back(plugin);
    g_pluginIds.push_back(id);
    g_hasOverlay = g_hasOverlay || plugin->onDrawOverlay != nullptr;
    loaderLog("plugin chargé : %s %s", plugin->name, plugin->version);
}

/** Charge les plugins livrés puis ceux installés depuis la boutique. */
void loadPlugins() {
    WIN32_FIND_DATAW entry = {};
    HANDLE search = FindFirstFileW((g_pluginsDir + L"\\*.dll").c_str(), &entry);
    if (search != INVALID_HANDLE_VALUE) {
        do {
            std::wstring id = entry.cFileName;
            id = id.substr(0, id.find_last_of(L'.'));
            loadPlugin(g_pluginsDir + L"\\" + entry.cFileName, id);
        } while (FindNextFileW(search, &entry));
        FindClose(search);
    }

    // Pendant l'initialisation : ne jamais rappeler loader::configDirectory() (InitOnce non réentrant).
    const std::wstring userDir = g_configDir + L"\\plugins";
    store::purgePendingRemovals(userDir);
    search = FindFirstFileW((userDir + L"\\*").c_str(), &entry);
    if (search == INVALID_HANDLE_VALUE) {
        return;
    }
    do {
        const std::wstring id = entry.cFileName;
        if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 || id == L"." || id == L"..") {
            continue;
        }
        const std::wstring folder = userDir + L"\\" + id;
        const std::wstring dll = folder + L"\\" + id + L".dll";
        if (GetFileAttributesW(dll.c_str()) != INVALID_FILE_ATTRIBUTES) {
            seedIniFiles(folder);
            loadPlugin(dll, id);
        }
    } while (FindNextFileW(search, &entry));
    FindClose(search);
}

BOOL CALLBACK initialize(PINIT_ONCE, PVOID, PVOID *) {
    wchar_t modulePath[MAX_PATH] = {};
    GetModuleFileNameW(g_self, modulePath, MAX_PATH);
    std::wstring directory(modulePath);
    g_installDir = directory.substr(0, directory.find_last_of(L"\\/"));
    g_pluginsDir = g_installDir + L"\\plugins";
    prepareConfigDirectory();
    g_lastConfigStamp = latestConfigStamp();

    wchar_t systemDir[MAX_PATH] = {};
    GetSystemDirectoryW(systemDir, MAX_PATH);
    g_realD3d9 = LoadLibraryW((std::wstring(systemDir) + L"\\d3d9.dll").c_str());
    if (g_realD3d9 == nullptr) {
        loaderLog("impossible de charger le d3d9.dll système (erreur %lu)", GetLastError());
        return TRUE;
    }
    loaderLog("chargeur démarré");
    loadPlugins();
    return TRUE;
}

void ensureInitialized() {
    InitOnceExecuteOnce(&g_initOnce, initialize, nullptr, nullptr);
}

template <typename Fn> Fn realProc(const char *name) {
    ensureInitialized();
    return g_realD3d9 != nullptr ? reinterpret_cast<Fn>(GetProcAddress(g_realD3d9, name)) : nullptr;
}

/** Sur le thread de rendu : applique les réglages modifiés (les plugins peuvent appeler l'appareil). */
void applyPendingConfig() {
    if (!g_configDirty.exchange(false)) {
        return;
    }
    loaderLog("réglages modifiés, rechargement");
    for (const LsPlugin *plugin : g_plugins) {
        if (plugin->onConfigChanged != nullptr) {
            plugin->onConfigChanged();
        }
    }
}

/** Dessine les overlays des plugins en préservant tout l'état de l'appareil. */
void drawOverlays(IDirect3DDevice9 *device) {
    IDirect3DSurface9 *backBuffer = nullptr;
    if (FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer)) || backBuffer == nullptr) {
        return;
    }
    D3DSURFACE_DESC desc = {};
    backBuffer->GetDesc(&desc);

    IDirect3DStateBlock9 *savedState = nullptr;
    IDirect3DSurface9 *previousTarget = nullptr;
    if (FAILED(device->CreateStateBlock(D3DSBT_ALL, &savedState))) {
        backBuffer->Release();
        return;
    }
    device->GetRenderTarget(0, &previousTarget);
    device->SetRenderTarget(0, backBuffer);

    if (SUCCEEDED(device->BeginScene())) {
        for (const LsPlugin *plugin : g_plugins) {
            if (plugin->onDrawOverlay != nullptr) {
                plugin->onDrawOverlay(device, desc.Width, desc.Height);
            }
        }
        device->EndScene();
    }

    if (previousTarget != nullptr) {
        device->SetRenderTarget(0, previousTarget);
        previousTarget->Release();
    }
    savedState->Apply();
    savedState->Release();
    backBuffer->Release();
}

HRESULT STDMETHODCALLTYPE hookReset(IDirect3DDevice9 *device, D3DPRESENT_PARAMETERS *params) {
    for (const LsPlugin *plugin : g_plugins) {
        if (plugin->onBeforeReset != nullptr) {
            plugin->onBeforeReset(device);
        }
    }
    const HRESULT result = g_origReset(device, params);
    if (SUCCEEDED(result)) {
        for (const LsPlugin *plugin : g_plugins) {
            if (plugin->onAfterReset != nullptr) {
                plugin->onAfterReset(device, params);
            }
        }
    }
    return result;
}

HRESULT STDMETHODCALLTYPE hookPresent(IDirect3DDevice9 *device, const RECT *source, const RECT *dest, HWND window,
                                      const RGNDATA *dirty) {
    g_lastPresentTick.store(GetTickCount64());
    applyPendingConfig();
    if (g_hasOverlay) {
        drawOverlays(device);
    }
    return g_origPresent(device, source, dest, window, dirty);
}

HRESULT STDMETHODCALLTYPE hookStretchRect(IDirect3DDevice9 *device, IDirect3DSurface9 *source, const RECT *sourceRect,
                                          IDirect3DSurface9 *dest, const RECT *destRect,
                                          D3DTEXTUREFILTERTYPE filter) {
    for (const LsPlugin *plugin : g_plugins) {
        if (plugin->onStretchRect != nullptr) {
            plugin->onStretchRect(device, &filter);
        }
    }
    return g_origStretchRect(device, source, sourceRect, dest, destRect, filter);
}

HRESULT STDMETHODCALLTYPE hookClear(IDirect3DDevice9 *device, DWORD count, const D3DRECT *rects, DWORD flags,
                                    D3DCOLOR color, float z, DWORD stencil) {
    if ((flags & D3DCLEAR_TARGET) != 0) {
        for (const LsPlugin *plugin : g_plugins) {
            if (plugin->onClear != nullptr) {
                plugin->onClear(device, &color);
            }
        }
    }
    return g_origClear(device, count, rects, flags, color, z, stencil);
}

HRESULT STDMETHODCALLTYPE hookSetSamplerState(IDirect3DDevice9 *device, DWORD sampler, D3DSAMPLERSTATETYPE type,
                                              DWORD value) {
    for (const LsPlugin *plugin : g_plugins) {
        if (plugin->onSamplerState != nullptr) {
            plugin->onSamplerState(device, sampler, type, &value);
        }
    }
    return g_origSetSamplerState(device, sampler, type, value);
}

/** Installe les hooks sur la vtable de l'appareil (partagée par toutes ses instances). */
void hookDevice(IDirect3DDevice9 *device) {
    if (g_deviceHooked) {
        return;
    }
    g_origReset = reinterpret_cast<ResetFn>(patchVtable(device, kDeviceReset, reinterpret_cast<void *>(&hookReset)));
    g_origPresent =
        reinterpret_cast<PresentFn>(patchVtable(device, kDevicePresent, reinterpret_cast<void *>(&hookPresent)));
    g_origStretchRect = reinterpret_cast<StretchRectFn>(
        patchVtable(device, kDeviceStretchRect, reinterpret_cast<void *>(&hookStretchRect)));
    g_origClear = reinterpret_cast<ClearFn>(patchVtable(device, kDeviceClear, reinterpret_cast<void *>(&hookClear)));
    g_origSetSamplerState = reinterpret_cast<SetSamplerStateFn>(
        patchVtable(device, kDeviceSetSamplerState, reinterpret_cast<void *>(&hookSetSamplerState)));
    g_deviceHooked = g_origReset != nullptr && g_origPresent != nullptr && g_origStretchRect != nullptr &&
                     g_origClear != nullptr && g_origSetSamplerState != nullptr;
    loaderLog(g_deviceHooked ? "hooks de l'appareil installés" : "échec partiel des hooks de l'appareil");
}

HRESULT STDMETHODCALLTYPE hookCreateDevice(IDirect3D9 *d3d, UINT adapter, D3DDEVTYPE type, HWND focusWindow,
                                           DWORD flags, D3DPRESENT_PARAMETERS *params, IDirect3DDevice9 **device) {
    const HRESULT result = g_origCreateDevice(d3d, adapter, type, focusWindow, flags, params, device);
    if (SUCCEEDED(result) && device != nullptr && *device != nullptr) {
        hookDevice(*device);
        for (const LsPlugin *plugin : g_plugins) {
            if (plugin->onDeviceCreated != nullptr) {
                plugin->onDeviceCreated(*device, params);
            }
        }
    }
    return result;
}

void bootstrapStudio() {
    if (!g_studioBootstrapped.exchange(true)) {
        studio::bootstrapOnUiThread();
    }
}

/** Ajoute l'entrée du Studio (et un séparateur) avant "Fermer" dans le menu système, une seule fois. */
void ensureStudioMenuItem(HMENU menu) {
    if (GetMenuState(menu, kStudioMenuId, MF_BYCOMMAND) != static_cast<UINT>(-1)) {
        return;
    }
    if (GetMenuState(menu, SC_CLOSE, MF_BYCOMMAND) != static_cast<UINT>(-1)) {
        InsertMenuW(menu, SC_CLOSE, MF_BYCOMMAND | MF_STRING, kStudioMenuId, kStudioMenuText);
        InsertMenuW(menu, SC_CLOSE, MF_BYCOMMAND | MF_SEPARATOR, 0, nullptr);
    } else {
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kStudioMenuId, kStudioMenuText);
    }
}

/**
 * Remplace TrackPopupMenuEx pour LonelyScreen.exe : Qt affiche le menu de la
 * fenêtre avec TPM_RETURNCMD, ce qui permet d'intercepter notre commande.
 */
BOOL WINAPI hookTrackPopupMenuEx(HMENU menu, UINT flags, int x, int y, HWND window, LPTPMPARAMS params) {
    const bool isSystemMenu = window != nullptr && menu == GetSystemMenu(window, FALSE);
    if (isSystemMenu) {
        bootstrapStudio();
        ensureStudioMenuItem(menu);
    }
    const BOOL result = g_origTrackPopupMenuEx(menu, flags, x, y, window, params);
    if (isSystemMenu && (flags & TPM_RETURNCMD) != 0 && static_cast<UINT>(result) == kStudioMenuId) {
        studio::toggle();
        return 0;
    }
    return result;
}

/**
 * Remplace CreateWindowExW pour LonelyScreen.exe : la création de la première
 * vraie fenêtre de premier niveau a lieu sur le thread d'interface de Qt, où
 * le Studio doit vivre.
 */
HWND WINAPI hookCreateWindowExW(DWORD exStyle, LPCWSTR className, LPCWSTR windowName, DWORD style, int x, int y,
                                int width, int height, HWND parent, HMENU menu, HINSTANCE instance, LPVOID param) {
    HWND window = g_origCreateWindowExW(exStyle, className, windowName, style, x, y, width, height, parent, menu,
                                        instance, param);
    const bool topLevel = window != nullptr && parent == nullptr && (style & WS_CHILD) == 0;
    const bool captioned = (style & WS_CAPTION) == WS_CAPTION;
    const bool largePopup = (style & WS_POPUP) != 0 && width >= kMinBootstrapWindowSize &&
                            height >= kMinBootstrapWindowSize;
    if (topLevel && (captioned || largePopup) && !g_studioBootstrapped.load()) {
        bootstrapStudio();
    }
    return window;
}

} // namespace

/**
 * Point d'entrée appelé par le relais d3d9.dll (stub/stub.cpp) juste après le
 * chargement du moteur, hors verrou du chargeur Windows.
 * @return FALSE si la structure du relais est incompatible.
 */
extern "C" __declspec(dllexport) BOOL __cdecl LsCoreAttach(const LsStubInfo *info, LsCoreHooks *hooks) {
    if (info == nullptr || hooks == nullptr || info->size < sizeof(LsStubInfo) || hooks->size < sizeof(LsCoreHooks) ||
        info->originalCreateWindowExW == nullptr || info->originalTrackPopupMenuEx == nullptr) {
        return FALSE;
    }
    g_stubVersion = info->stubVersion;
    g_origCreateWindowExW = info->originalCreateWindowExW;
    g_origTrackPopupMenuEx = info->originalTrackPopupMenuEx;
    hooks->createWindowExW = hookCreateWindowExW;
    hooks->trackPopupMenuEx = hookTrackPopupMenuEx;
    return TRUE;
}

namespace loader {

const char *coreVersion() {
    return LS_STUDIO_VERSION;
}

int stubVersion() {
    return g_stubVersion;
}

const std::wstring &configDirectory() {
    ensureInitialized();
    return g_configDir;
}

const std::wstring &installDirectory() {
    ensureInitialized();
    return g_installDir;
}

const std::wstring &builtinPluginsDirectory() {
    ensureInitialized();
    return g_pluginsDir;
}

void restoreDefaultConfig() {
    ensureInitialized();
    WIN32_FIND_DATAW entry = {};
    HANDLE search = FindFirstFileW((g_configDir + L"\\*.ini").c_str(), &entry);
    if (search != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring name = entry.cFileName;
            if (name != L"studio-window.ini") {
                DeleteFileW((g_configDir + L"\\" + name).c_str());
            }
        } while (FindNextFileW(search, &entry));
        FindClose(search);
    }
    seedIniFiles(g_pluginsDir);
    loaderLog("réglages remis par défaut");
}

std::vector<std::wstring> loadedPluginIds() {
    ensureInitialized();
    return g_pluginIds;
}

bool isRendering() {
    const ULONGLONG last = g_lastPresentTick.load();
    return last != 0 && GetTickCount64() - last < kRenderingWindowMs;
}

void markConfigDirty() {
    g_configDirty.store(true);
    g_lastConfigStamp = latestConfigStamp();
}

void checkConfigChanges() {
    const ULONGLONG now = GetTickCount64();
    if (now - g_lastConfigPoll < kConfigPollIntervalMs) {
        return;
    }
    g_lastConfigPoll = now;
    const ULONGLONG stamp = latestConfigStamp();
    if (stamp != g_lastConfigStamp) {
        g_lastConfigStamp = stamp;
        g_configDirty.store(true);
    }
}

void log(const char *format, ...) {
    va_list args;
    va_start(args, format);
    vlog(format, args);
    va_end(args);
}

} // namespace loader

extern "C" IDirect3D9 *WINAPI Proxy_Direct3DCreate9(UINT sdkVersion) {
    const auto create = realProc<Direct3DCreate9Fn>("Direct3DCreate9");
    if (create == nullptr) {
        return nullptr;
    }
    IDirect3D9 *d3d = create(sdkVersion);
    if (d3d != nullptr && !g_d3dHooked) {
        g_origCreateDevice = reinterpret_cast<CreateDeviceFn>(
            patchVtable(d3d, kD3d9CreateDevice, reinterpret_cast<void *>(&hookCreateDevice)));
        g_d3dHooked = g_origCreateDevice != nullptr;
    }
    return d3d;
}

extern "C" HRESULT WINAPI Proxy_Direct3DCreate9Ex(UINT sdkVersion, IDirect3D9Ex **out) {
    const auto create = realProc<Direct3DCreate9ExFn>("Direct3DCreate9Ex");
    return create != nullptr ? create(sdkVersion, out) : E_NOTIMPL;
}

extern "C" int WINAPI Proxy_D3DPERF_BeginEvent(D3DCOLOR color, LPCWSTR name) {
    const auto fn = realProc<PerfColorStringIntFn>("D3DPERF_BeginEvent");
    return fn != nullptr ? fn(color, name) : 0;
}

extern "C" int WINAPI Proxy_D3DPERF_EndEvent() {
    const auto fn = realProc<PerfIntFn>("D3DPERF_EndEvent");
    return fn != nullptr ? fn() : 0;
}

extern "C" void WINAPI Proxy_D3DPERF_SetMarker(D3DCOLOR color, LPCWSTR name) {
    if (const auto fn = realProc<PerfColorStringVoidFn>("D3DPERF_SetMarker")) {
        fn(color, name);
    }
}

extern "C" void WINAPI Proxy_D3DPERF_SetRegion(D3DCOLOR color, LPCWSTR name) {
    if (const auto fn = realProc<PerfColorStringVoidFn>("D3DPERF_SetRegion")) {
        fn(color, name);
    }
}

extern "C" BOOL WINAPI Proxy_D3DPERF_QueryRepeatFrame() {
    const auto fn = realProc<PerfBoolFn>("D3DPERF_QueryRepeatFrame");
    return fn != nullptr ? fn() : FALSE;
}

extern "C" void WINAPI Proxy_D3DPERF_SetOptions(DWORD options) {
    if (const auto fn = realProc<PerfSetOptionsFn>("D3DPERF_SetOptions")) {
        fn(options);
    }
}

extern "C" DWORD WINAPI Proxy_D3DPERF_GetStatus() {
    const auto fn = realProc<PerfDwordFn>("D3DPERF_GetStatus");
    return fn != nullptr ? fn() : 0;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = instance;
        DisableThreadLibraryCalls(instance);
    } else if (reason == DLL_PROCESS_DETACH && reserved == nullptr) {
        // Déchargement explicite uniquement : à la fin du processus, ne rien appeler.
        for (const LsPlugin *plugin : g_plugins) {
            if (plugin->onShutdown != nullptr) {
                plugin->onShutdown();
            }
        }
    }
    return TRUE;
}
