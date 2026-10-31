/**
 * @file stub.cpp
 * Relais d3d9.dll : le seul fichier posé dans le dossier de LonelyScreen.
 *
 * Volontairement minimal et stable, pour ne presque jamais demander de droits
 * administrateur. Il :
 *  - remplace CreateWindowExW et TrackPopupMenuEx dans la table d'imports de
 *    LonelyScreen.exe (simple réécriture mémoire, faite dans DllMain) ;
 *  - à la création de la fenêtre principale (hors verrou du chargeur), charge
 *    %LOCALAPPDATA%\LonelyScreenStudio\versions\<current.txt>\lsstudio.dll ;
 *  - relaie les exports Direct3D 9 vers le moteur, ou vers le d3d9 système
 *    si le moteur est absent ou invalide (LonelyScreen fonctionne alors normalement).
 */
#include <windows.h>
#include <d3d9.h>

#include <cstring>
#include <string>

#include "lsstub.h"

#pragma comment(linker, "/EXPORT:Direct3DCreate9=_Stub_Direct3DCreate9@4")
#pragma comment(linker, "/EXPORT:Direct3DCreate9Ex=_Stub_Direct3DCreate9Ex@8")
#pragma comment(linker, "/EXPORT:D3DPERF_BeginEvent=_Stub_D3DPERF_BeginEvent@8")
#pragma comment(linker, "/EXPORT:D3DPERF_EndEvent=_Stub_D3DPERF_EndEvent@0")
#pragma comment(linker, "/EXPORT:D3DPERF_SetMarker=_Stub_D3DPERF_SetMarker@8")
#pragma comment(linker, "/EXPORT:D3DPERF_SetRegion=_Stub_D3DPERF_SetRegion@8")
#pragma comment(linker, "/EXPORT:D3DPERF_QueryRepeatFrame=_Stub_D3DPERF_QueryRepeatFrame@0")
#pragma comment(linker, "/EXPORT:D3DPERF_SetOptions=_Stub_D3DPERF_SetOptions@4")
#pragma comment(linker, "/EXPORT:D3DPERF_GetStatus=_Stub_D3DPERF_GetStatus@0")

namespace {

constexpr size_t kMaxVersionLength = 32;
constexpr int kMinBootstrapWindowSize = 300;

using Direct3DCreate9Fn = IDirect3D9 *(WINAPI *)(UINT);
using Direct3DCreate9ExFn = HRESULT(WINAPI *)(UINT, IDirect3D9Ex **);
using PerfColorStringIntFn = int(WINAPI *)(D3DCOLOR, LPCWSTR);
using PerfColorStringVoidFn = void(WINAPI *)(D3DCOLOR, LPCWSTR);
using PerfIntFn = int(WINAPI *)();
using PerfBoolFn = BOOL(WINAPI *)();
using PerfDwordFn = DWORD(WINAPI *)();
using PerfSetOptionsFn = void(WINAPI *)(DWORD);

INIT_ONCE g_loadOnce = INIT_ONCE_STATIC_INIT;
HMODULE g_core = nullptr;
HMODULE g_system = nullptr;
LsStubInfo g_info = {sizeof(LsStubInfo), LS_STUB_VERSION, nullptr, nullptr};
LsCoreHooks g_hooks = {sizeof(LsCoreHooks), nullptr, nullptr};

/** Version courante lue dans current.txt (chiffres, lettres, points, tirets uniquement). */
std::wstring currentVersion(const std::wstring &base) {
    HANDLE file = CreateFileW((base + L"\\current.txt").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return {};
    }
    char buffer[kMaxVersionLength + 1] = {};
    DWORD read = 0;
    ReadFile(file, buffer, kMaxVersionLength, &read, nullptr);
    CloseHandle(file);
    std::wstring version;
    for (DWORD i = 0; i < read; ++i) {
        const char c = buffer[i];
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.' || c == '-') {
            version += static_cast<wchar_t>(c);
        } else if (c != '\r' && c != '\n' && c != ' ') {
            return {};
        }
    }
    return version;
}

BOOL CALLBACK loadEngine(PINIT_ONCE, PVOID, PVOID *) {
    wchar_t localAppData[MAX_PATH] = {};
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH);
    if (length > 0 && length < MAX_PATH) {
        const std::wstring base = std::wstring(localAppData) + L"\\" + LS_APP_FOLDER;
        const std::wstring version = currentVersion(base);
        if (!version.empty()) {
            const std::wstring core = base + L"\\versions\\" + version + L"\\" + LS_CORE_FILE;
            g_core = LoadLibraryExW(core.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
            const auto attach =
                g_core != nullptr ? reinterpret_cast<LsCoreAttachFn>(GetProcAddress(g_core, LS_CORE_ATTACH_NAME)) : nullptr;
            if (attach == nullptr || !attach(&g_info, &g_hooks)) {
                g_hooks.createWindowExW = nullptr;
                g_hooks.trackPopupMenuEx = nullptr;
                if (g_core != nullptr) {
                    FreeLibrary(g_core);
                    g_core = nullptr;
                }
            }
        }
    }
    if (g_core == nullptr) {
        wchar_t systemDir[MAX_PATH] = {};
        GetSystemDirectoryW(systemDir, MAX_PATH);
        g_system = LoadLibraryW((std::wstring(systemDir) + L"\\d3d9.dll").c_str());
    }
    return TRUE;
}

void ensureEngine() {
    InitOnceExecuteOnce(&g_loadOnce, loadEngine, nullptr, nullptr);
}

template <typename Fn> Fn target(const char *name) {
    ensureEngine();
    HMODULE module = g_core != nullptr ? g_core : g_system;
    return module != nullptr ? reinterpret_cast<Fn>(GetProcAddress(module, name)) : nullptr;
}

HWND WINAPI stubCreateWindowExW(DWORD exStyle, LPCWSTR className, LPCWSTR windowName, DWORD style, int x, int y,
                                int width, int height, HWND parent, HMENU menu, HINSTANCE instance, LPVOID param) {
    // La fenêtre principale est créée sur le thread d'interface : c'est là que le moteur doit naître.
    const bool topLevel = parent == nullptr && (style & WS_CHILD) == 0;
    const bool captioned = (style & WS_CAPTION) == WS_CAPTION;
    const bool largePopup = (style & WS_POPUP) != 0 && width >= kMinBootstrapWindowSize && height >= kMinBootstrapWindowSize;
    if (topLevel && (captioned || largePopup)) {
        ensureEngine();
    }
    const LsCreateWindowExWFn next = g_hooks.createWindowExW != nullptr ? g_hooks.createWindowExW : g_info.originalCreateWindowExW;
    return next(exStyle, className, windowName, style, x, y, width, height, parent, menu, instance, param);
}

BOOL WINAPI stubTrackPopupMenuEx(HMENU menu, UINT flags, int x, int y, HWND window, LPTPMPARAMS params) {
    const LsTrackPopupMenuExFn next =
        g_hooks.trackPopupMenuEx != nullptr ? g_hooks.trackPopupMenuEx : g_info.originalTrackPopupMenuEx;
    return next(menu, flags, x, y, window, params);
}

/** Remplace une entrée de la table d'imports de l'exe. @return l'adresse d'origine, ou nullptr. */
void *hookImport(HMODULE module, const char *dllName, const char *functionName, void *hook) {
    auto *base = reinterpret_cast<BYTE *>(module);
    const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
    const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY &imports = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (imports.VirtualAddress == 0) {
        return nullptr;
    }
    for (auto *descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR *>(base + imports.VirtualAddress);
         descriptor->Name != 0; ++descriptor) {
        if (_stricmp(reinterpret_cast<const char *>(base + descriptor->Name), dllName) != 0 ||
            descriptor->OriginalFirstThunk == 0) {
            continue;
        }
        auto *names = reinterpret_cast<IMAGE_THUNK_DATA *>(base + descriptor->OriginalFirstThunk);
        auto *addresses = reinterpret_cast<IMAGE_THUNK_DATA *>(base + descriptor->FirstThunk);
        for (; names->u1.AddressOfData != 0; ++names, ++addresses) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) {
                continue;
            }
            const auto *byName = reinterpret_cast<const IMAGE_IMPORT_BY_NAME *>(base + names->u1.AddressOfData);
            if (std::strcmp(byName->Name, functionName) != 0) {
                continue;
            }
            DWORD oldProtect = 0;
            if (!VirtualProtect(&addresses->u1.Function, sizeof(void *), PAGE_READWRITE, &oldProtect)) {
                return nullptr;
            }
            void *original = reinterpret_cast<void *>(addresses->u1.Function);
            addresses->u1.Function = reinterpret_cast<ULONG_PTR>(hook);
            VirtualProtect(&addresses->u1.Function, sizeof(void *), oldProtect, &oldProtect);
            return original;
        }
    }
    return nullptr;
}

void installImportHooks() {
    const HMODULE exe = GetModuleHandleW(nullptr);
    void *createWindow = hookImport(exe, "USER32.dll", "CreateWindowExW", reinterpret_cast<void *>(&stubCreateWindowExW));
    void *trackMenu = hookImport(exe, "USER32.dll", "TrackPopupMenuEx", reinterpret_cast<void *>(&stubTrackPopupMenuEx));
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    g_info.originalCreateWindowExW = reinterpret_cast<LsCreateWindowExWFn>(
        createWindow != nullptr ? createWindow : GetProcAddress(user32, "CreateWindowExW"));
    g_info.originalTrackPopupMenuEx = reinterpret_cast<LsTrackPopupMenuExFn>(
        trackMenu != nullptr ? trackMenu : GetProcAddress(user32, "TrackPopupMenuEx"));
}

} // namespace

extern "C" IDirect3D9 *WINAPI Stub_Direct3DCreate9(UINT sdkVersion) {
    const auto fn = target<Direct3DCreate9Fn>("Direct3DCreate9");
    return fn != nullptr ? fn(sdkVersion) : nullptr;
}

extern "C" HRESULT WINAPI Stub_Direct3DCreate9Ex(UINT sdkVersion, IDirect3D9Ex **out) {
    const auto fn = target<Direct3DCreate9ExFn>("Direct3DCreate9Ex");
    return fn != nullptr ? fn(sdkVersion, out) : E_NOTIMPL;
}

extern "C" int WINAPI Stub_D3DPERF_BeginEvent(D3DCOLOR color, LPCWSTR name) {
    const auto fn = target<PerfColorStringIntFn>("D3DPERF_BeginEvent");
    return fn != nullptr ? fn(color, name) : 0;
}

extern "C" int WINAPI Stub_D3DPERF_EndEvent() {
    const auto fn = target<PerfIntFn>("D3DPERF_EndEvent");
    return fn != nullptr ? fn() : 0;
}

extern "C" void WINAPI Stub_D3DPERF_SetMarker(D3DCOLOR color, LPCWSTR name) {
    if (const auto fn = target<PerfColorStringVoidFn>("D3DPERF_SetMarker")) {
        fn(color, name);
    }
}

extern "C" void WINAPI Stub_D3DPERF_SetRegion(D3DCOLOR color, LPCWSTR name) {
    if (const auto fn = target<PerfColorStringVoidFn>("D3DPERF_SetRegion")) {
        fn(color, name);
    }
}

extern "C" BOOL WINAPI Stub_D3DPERF_QueryRepeatFrame() {
    const auto fn = target<PerfBoolFn>("D3DPERF_QueryRepeatFrame");
    return fn != nullptr ? fn() : FALSE;
}

extern "C" void WINAPI Stub_D3DPERF_SetOptions(DWORD options) {
    if (const auto fn = target<PerfSetOptionsFn>("D3DPERF_SetOptions")) {
        fn(options);
    }
}

extern "C" DWORD WINAPI Stub_D3DPERF_GetStatus() {
    const auto fn = target<PerfDwordFn>("D3DPERF_GetStatus");
    return fn != nullptr ? fn() : 0;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        installImportHooks(); // aucune DLL chargée ici : uniquement une réécriture mémoire
    }
    return TRUE;
}
