/**
 * @file lsstub.h
 * Contrat entre le relais d3d9.dll (posé à côté de LonelyScreen.exe, rarement
 * mis à jour) et le moteur lsstudio.dll (dans %LOCALAPPDATA%, mis à jour par
 * le Studio sans droits administrateur).
 *
 * Le relais intercepte CreateWindowExW et TrackPopupMenuEx dans l'exe, charge
 * le moteur de la version courante, puis lui délègue ces appels et les exports
 * Direct3D 9. Sans moteur valide, il se contente de relayer le d3d9 système.
 */
#pragma once

#include <windows.h>

#define LS_STUB_VERSION 1
#define LS_CORE_ATTACH_NAME "LsCoreAttach"
#define LS_APP_FOLDER L"LonelyScreenStudio"
#define LS_CORE_FILE L"lsstudio.dll"

typedef HWND(WINAPI *LsCreateWindowExWFn)(DWORD, LPCWSTR, LPCWSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE,
                                          LPVOID);
typedef BOOL(WINAPI *LsTrackPopupMenuExFn)(HMENU, UINT, int, int, HWND, LPTPMPARAMS);

/** Ce que le relais fournit au moteur. */
typedef struct LsStubInfo {
    UINT size;
    int stubVersion;
    LsCreateWindowExWFn originalCreateWindowExW;
    LsTrackPopupMenuExFn originalTrackPopupMenuEx;
} LsStubInfo;

/** Ce que le moteur rend au relais. */
typedef struct LsCoreHooks {
    UINT size;
    LsCreateWindowExWFn createWindowExW;
    LsTrackPopupMenuExFn trackPopupMenuEx;
} LsCoreHooks;

typedef BOOL(__cdecl *LsCoreAttachFn)(const LsStubInfo *info, LsCoreHooks *hooks);
