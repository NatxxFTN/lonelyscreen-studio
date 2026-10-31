/**
 * @file updater.h
 * Mises à jour du Studio, sans droits administrateur.
 *
 * Flux publié (GitHub Releases) : latest.json (manifeste) + latest.json.sig
 * (signature ECDSA P-256 / SHA-256, Base64, format r||s). Le manifeste liste
 * chaque fichier avec son URL et son SHA-256. Une version n'est installée que
 * si la signature correspond à la clé publique intégrée (release_key.h) ET
 * que chaque fichier correspond à son empreinte.
 *
 * Les versions vivent dans %LOCALAPPDATA%\LonelyScreenStudio\versions\<v>\ ;
 * current.txt désigne celle que le relais d3d9.dll charge au démarrage.
 */
#pragma once

#include "store.h"

namespace updater {

/** {version, stubVersion, signingConfigured, feed}. */
store::json status();

/** Vérifie le flux : {current, latest, available, notes, stubUpdateRequired}. */
void checkAsync(store::Completion done);

/** Télécharge, vérifie et active la dernière version (effective au prochain démarrage). */
void applyAsync(store::Completion done);

/** Supprime en arrière-plan les anciennes versions inutilisées. */
void cleanupOldVersionsAsync();

} // namespace updater
