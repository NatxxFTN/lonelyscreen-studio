/**
 * @file store.h
 * Boutique et état du Studio : lecture/écriture des réglages, installation
 * et désinstallation des plugins, thèmes et apps, accès réseau vérifié.
 */
#pragma once

#include <functional>
#include <string>

#include "nlohmann/json.hpp"

namespace store {

using json = nlohmann::json;
using Completion = std::function<void(const json &result, const std::string &error)>;

/** Inventaire complet : plugins, thèmes, apps, configuration, catalogue local, état du rendu. */
json buildState();

/** {file, section, keys[]} -> {clé: valeur} depuis le .ini de réglages. */
json readIni(const json &args);

/** {file, section, values{clé: valeur}} : écrit plusieurs réglages d'un coup. */
json writeIni(const json &args);

/** {config} : enregistre studio.json. */
json saveConfig(const json &args);

/** {type, id} : désinstalle (différé au prochain démarrage pour un plugin chargé). */
json uninstall(const json &args);

/** Installe une entrée de catalogue sur un thread de travail ; `done` est appelé sur ce thread. */
void installAsync(const json &item, Completion done);

/** Télécharge un texte (catalogue distant) en HTTPS sur un thread de travail. */
void fetchTextAsync(const std::string &url, Completion done);

/**
 * Supprime les plugins marqués pour désinstallation (appelé avant leur chargement).
 * Le dossier est passé explicitement : cette fonction s'exécute pendant
 * l'initialisation du chargeur et ne doit pas la redemander.
 */
void purgePendingRemovals(const std::wstring &userPluginsDir);

/** Dossiers. */
std::wstring userPluginsDirectory();
std::wstring studioDirectory();

/** Conversions UTF-8 <-> UTF-16. */
std::wstring toWide(const std::string &text);
std::string toUtf8(const std::wstring &text);

} // namespace store
