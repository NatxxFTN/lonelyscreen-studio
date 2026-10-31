/**
 * @file loader_internal.h
 * Fonctions partagées entre les fichiers du chargeur (non exposées aux plugins).
 */
#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace loader {

/** Dossier des réglages (%APPDATA%\LonelyScreenPlugins). Initialise le chargeur si besoin. */
const std::wstring &configDirectory();

/** Dossier d'installation (celui de d3d9.dll, à côté de LonelyScreen.exe). */
const std::wstring &installDirectory();

/** Dossier des plugins livrés (install\plugins). */
const std::wstring &builtinPluginsDirectory();

/** Supprime les .ini de réglages et recopie les valeurs par défaut. */
void restoreDefaultConfig();

/** Identifiants (nom de fichier sans .dll) des plugins chargés dans ce processus. */
std::vector<std::wstring> loadedPluginIds();

/** Vrai si une image vidéo a été affichée dans les 2 dernières secondes. */
bool isRendering();

/** Signale qu'un .ini a changé : les plugins rechargeront au prochain rendu. */
void markConfigDirty();

/** Au plus une fois par seconde : détecte un .ini modifié à la main (thread d'interface). */
void checkConfigChanges();

/** Écrit une ligne dans le journal au nom du chargeur. */
void log(const char *format, ...);

/** Version du moteur (fichier VERSION, injectée à la compilation). */
const char *coreVersion();

/** Version du relais d3d9.dll qui a chargé le moteur (0 si inconnu). */
int stubVersion();

} // namespace loader

namespace studio {

/** Démarre le Studio (onglet, raccourci, suivi de fenêtre) sur le thread d'interface. */
void bootstrapOnUiThread();

/** Ouvre ou ferme le panneau Studio. */
void toggle();

} // namespace studio
