/**
 * @file lsplugin.h
 * API publique des plugins LonelyScreen (ABI C, x86 32 bits).
 *
 * Un plugin est une DLL placée dans le dossier `plugins\` à côté de
 * LonelyScreen.exe. Elle exporte une fonction `LsPluginEntry` qui reçoit
 * l'API de l'hôte et renvoie la description du plugin (ou NULL pour refuser
 * de se charger). Chaque callback est optionnel : laisser NULL si inutile.
 */
#pragma once

#include <windows.h>
#include <d3d9.h>
#include <stdint.h>

#define LS_PLUGIN_API_VERSION 1u
#define LS_PLUGIN_ENTRY_NAME "LsPluginEntry"

#ifdef __cplusplus
extern "C" {
#endif

/** Services fournis par le chargeur aux plugins. */
typedef struct LsHostApi {
    uint32_t apiVersion;
    /** Écrit une ligne dans plugins\lsplugins.log. */
    void(__cdecl *log)(const char *pluginName, const char *message);
    /** Chemin absolu du dossier plugins\ (sans barre finale). */
    const wchar_t *(__cdecl *pluginsDirectory)(void);
    /** Dossier des réglages modifiables : %APPDATA%\LonelyScreenPlugins. */
    const wchar_t *(__cdecl *configDirectory)(void);
} LsHostApi;

/** Description et callbacks d'un plugin. */
typedef struct LsPlugin {
    uint32_t apiVersion;
    const char *name;
    const char *version;

    /** Appareil Direct3D créé par l'application. */
    void(__cdecl *onDeviceCreated)(IDirect3DDevice9 *device, const D3DPRESENT_PARAMETERS *params);
    /** Juste avant un Reset (libérer ici les ressources D3DPOOL_DEFAULT). */
    void(__cdecl *onBeforeReset)(IDirect3DDevice9 *device);
    /** Après un Reset réussi (recréer ici les ressources). */
    void(__cdecl *onAfterReset)(IDirect3DDevice9 *device, const D3DPRESENT_PARAMETERS *params);
    /** Effacement de la cible de rendu : la couleur peut être modifiée. */
    void(__cdecl *onClear)(IDirect3DDevice9 *device, D3DCOLOR *color);
    /** Changement d'état d'un échantillonneur : la valeur peut être modifiée. */
    void(__cdecl *onSamplerState)(IDirect3DDevice9 *device, DWORD sampler, D3DSAMPLERSTATETYPE type, DWORD *value);
    /** Copie mise à l'échelle (StretchRect) : le filtre peut être modifié. */
    void(__cdecl *onStretchRect)(IDirect3DDevice9 *device, D3DTEXTUREFILTERTYPE *filter);
    /** Dessin par-dessus l'image, juste avant l'affichage. Les états sont restaurés après. */
    void(__cdecl *onDrawOverlay)(IDirect3DDevice9 *device, UINT width, UINT height);
    /** Déchargement du plugin. */
    void(__cdecl *onShutdown)(void);
    /** Un fichier .ini du dossier de réglages a changé : relire la configuration. */
    void(__cdecl *onConfigChanged)(void);
} LsPlugin;

typedef const LsPlugin *(__cdecl *LsPluginEntryFn)(const LsHostApi *host);

#ifdef __cplusplus
}
#define LS_PLUGIN_EXPORT extern "C" __declspec(dllexport)
#else
#define LS_PLUGIN_EXPORT __declspec(dllexport)
#endif
