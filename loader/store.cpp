/**
 * @file store.cpp
 * Implémentation de la boutique du Studio.
 *
 * Organisation sur disque :
 *   <moteur>\plugins\<id>.dll + <id>.json      plugins livrés (version courante)
 *   <moteur>\studio\                           interface web + catalogue local
 *   %APPDATA%\LonelyScreenPlugins\
 *       *.ini                                  réglages des plugins
 *       studio.json                            configuration du Studio
 *       plugins\<id>\<id>.dll + plugin.json    plugins installés par la boutique
 *       themes\<id>.json                       thèmes installés
 *       apps\<id>\index.html + app.json        apps installées
 *       cache\                                 téléchargements en cours
 *
 * Sécurité : identifiants et chemins relatifs sont validés (pas de "..", pas de
 * chemin absolu) ; tout fichier téléchargé doit porter une empreinte SHA-256,
 * vérifiée avant installation. Seul HTTPS est accepté.
 */
#include "store.h"

#include <windows.h>

#include <algorithm>
#include <thread>
#include <vector>

#include "loader_internal.h"
#include "util.h"

namespace store {

namespace {

using util::joinPath;

constexpr wchar_t kStudioFolder[] = L"studio";
constexpr wchar_t kCatalogFolder[] = L"catalog";
constexpr wchar_t kConfigFile[] = L"studio.json";
constexpr wchar_t kRemovalMarker[] = L".uninstall";
constexpr size_t kMaxFilesPerItem = 64;
constexpr DWORD kIniValueBufferSize = 512;

const char *const kItemTypes[] = {"plugin", "theme", "app"};

/** Lit et analyse un JSON ; renvoie null si absent ou invalide. */
json readJsonFile(const std::wstring &path) {
    std::string text;
    if (!util::readFile(path, &text)) {
        return nullptr;
    }
    json parsed = json::parse(text, nullptr, false, true);
    return parsed.is_discarded() ? json(nullptr) : parsed;
}

bool isItemType(const std::string &type) {
    return std::any_of(std::begin(kItemTypes), std::end(kItemTypes), [&](const char *t) { return type == t; });
}

bool isSafeIniFile(const std::string &file) {
    constexpr char kSuffix[] = ".ini";
    constexpr size_t kSuffixLength = sizeof(kSuffix) - 1;
    return file.size() > kSuffixLength && file.compare(file.size() - kSuffixLength, kSuffixLength, kSuffix) == 0 &&
           util::isSafeId(file.substr(0, file.size() - kSuffixLength));
}

std::wstring typeDirectory(const std::string &type) {
    const std::wstring &base = loader::configDirectory();
    if (type == "plugin") {
        return joinPath(base, L"plugins");
    }
    if (type == "theme") {
        return joinPath(base, L"themes");
    }
    return joinPath(base, L"apps");
}

std::wstring catalogDirectory() {
    return joinPath(studioDirectory(), kCatalogFolder);
}

bool containsId(const std::vector<std::wstring> &ids, const std::wstring &id) {
    return std::any_of(ids.begin(), ids.end(),
                       [&](const std::wstring &value) { return _wcsicmp(value.c_str(), id.c_str()) == 0; });
}

std::wstring stem(const std::wstring &fileName) {
    const size_t dot = fileName.find_last_of(L'.');
    return dot == std::wstring::npos ? fileName : fileName.substr(0, dot);
}

json pluginInventory() {
    json plugins = json::array();
    const std::vector<std::wstring> loaded = loader::loadedPluginIds();
    for (const std::wstring &file : util::listEntries(loader::builtinPluginsDirectory(), L"*.dll", false)) {
        const std::wstring id = stem(file);
        plugins.push_back({{"id", toUtf8(id)},
                           {"source", "builtin"},
                           {"loaded", containsId(loaded, id)},
                           {"pendingRemoval", false},
                           {"manifest", readJsonFile(joinPath(loader::builtinPluginsDirectory(), id + L".json"))}});
    }
    const std::wstring userDir = userPluginsDirectory();
    for (const std::wstring &id : util::listEntries(userDir, L"*", true)) {
        const std::wstring folder = joinPath(userDir, id);
        if (!util::fileExists(joinPath(folder, id + L".dll"))) {
            continue;
        }
        plugins.push_back({{"id", toUtf8(id)},
                           {"source", "user"},
                           {"loaded", containsId(loaded, id)},
                           {"pendingRemoval", util::fileExists(joinPath(folder, kRemovalMarker))},
                           {"manifest", readJsonFile(joinPath(folder, L"plugin.json"))}});
    }
    return plugins;
}

json themeInventory() {
    json themes = json::array();
    const std::wstring directory = typeDirectory("theme");
    for (const std::wstring &file : util::listEntries(directory, L"*.json", false)) {
        json manifest = readJsonFile(joinPath(directory, file));
        if (manifest.is_object()) {
            themes.push_back({{"id", toUtf8(stem(file))}, {"manifest", manifest}});
        }
    }
    return themes;
}

json appInventory() {
    json apps = json::array();
    const std::wstring directory = typeDirectory("app");
    for (const std::wstring &id : util::listEntries(directory, L"*", true)) {
        const std::wstring folder = joinPath(directory, id);
        if (!util::fileExists(joinPath(folder, L"index.html"))) {
            continue;
        }
        apps.push_back({{"id", toUtf8(id)},
                        {"manifest", readJsonFile(joinPath(folder, L"app.json"))},
                        {"url", "https://user.ls/apps/" + toUtf8(id) + "/index.html"}});
    }
    return apps;
}

/** Récupère les octets d'un fichier de catalogue (HTTPS ou catalogue local livré). */
bool fetchItemFile(const json &file, std::string *bytes, std::string *error) {
    const std::string source = file.value("src", "");
    const std::string expected = util::lowercase(file.value("sha256", ""));
    if (util::startsWith(source, "https://")) {
        if (expected.empty()) {
            *error = "Fichier distant sans empreinte SHA-256 : installation refusée.";
            return false;
        }
        if (!util::httpsGet(source, bytes, error)) {
            return false;
        }
    } else {
        std::wstring relative;
        if (!util::toSafeRelativePath(source, &relative) || !util::readFile(joinPath(catalogDirectory(), relative), bytes)) {
            *error = "Fichier introuvable dans le catalogue : " + source;
            return false;
        }
    }
    if (!expected.empty() && util::sha256Hex(*bytes) != expected) {
        *error = "Empreinte SHA-256 incorrecte pour " + source + " : fichier rejeté.";
        return false;
    }
    return true;
}

/** Installation complète d'un élément (exécutée sur un thread de travail). */
json installItem(const json &item) {
    const std::string type = item.value("type", "");
    const std::string id = item.value("id", "");
    if (!isItemType(type) || !util::isSafeId(id)) {
        throw std::runtime_error("Élément de catalogue invalide.");
    }
    const json files = item.value("files", json::array());
    if (!files.is_array() || files.empty() || files.size() > kMaxFilesPerItem) {
        throw std::runtime_error("Cet élément ne contient aucun fichier à installer.");
    }

    const std::wstring wideId = toWide(id);
    const std::wstring staging = joinPath(joinPath(loader::configDirectory(), L"cache"), wideId + L"-staging");
    util::removeTree(staging);
    util::ensureDirectory(staging);

    std::vector<std::wstring> written;
    for (const json &file : files) {
        std::wstring relative;
        if (!util::toSafeRelativePath(file.value("path", ""), &relative)) {
            util::removeTree(staging);
            throw std::runtime_error("Chemin de fichier refusé dans le catalogue.");
        }
        if (type == "theme" && relative != wideId + L".json") {
            util::removeTree(staging);
            throw std::runtime_error("Un thème doit être un unique fichier <id>.json.");
        }
        std::string bytes;
        std::string error;
        if (!fetchItemFile(file, &bytes, &error)) {
            util::removeTree(staging);
            throw std::runtime_error(error);
        }
        if (!util::writeFileAtomic(joinPath(staging, relative), bytes)) {
            util::removeTree(staging);
            throw std::runtime_error("Écriture impossible dans le dossier de réglages.");
        }
        written.push_back(relative);
    }

    const std::wstring base = typeDirectory(type);
    util::ensureDirectory(base);
    if (type == "theme") {
        const std::wstring target = joinPath(base, wideId + L".json");
        if (!MoveFileExW(joinPath(staging, written.front()).c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            util::removeTree(staging);
            throw std::runtime_error("Installation du thème impossible.");
        }
        util::removeTree(staging);
        return {{"installed", true}, {"restartRequired", false}};
    }

    const std::wstring target = joinPath(base, wideId);
    if (util::directoryExists(target) && !util::removeTree(target)) {
        util::removeTree(staging);
        throw std::runtime_error(
            "Une version en cours d'utilisation bloque la mise à jour. Redémarre LonelyScreen puis réessaie.");
    }
    if (!MoveFileExW(staging.c_str(), target.c_str(), 0)) {
        util::removeTree(staging);
        throw std::runtime_error("Installation impossible (erreur " + std::to_string(GetLastError()) + ").");
    }
    if (type == "plugin") {
        // Réglages par défaut fournis avec le plugin : copiés une seule fois.
        const std::wstring defaults = joinPath(target, wideId + L".ini");
        if (util::fileExists(defaults)) {
            CopyFileW(defaults.c_str(), joinPath(loader::configDirectory(), wideId + L".ini").c_str(), TRUE);
        }
    }
    loader::log("installé : %s %s", type.c_str(), id.c_str());
    return {{"installed", true}, {"restartRequired", type == "plugin"}};
}

std::wstring configPath() {
    return joinPath(loader::configDirectory(), kConfigFile);
}

} // namespace

std::wstring toWide(const std::string &text) {
    return util::toWide(text);
}

std::string toUtf8(const std::wstring &text) {
    return util::toUtf8(text);
}

std::wstring userPluginsDirectory() {
    return typeDirectory("plugin");
}

std::wstring studioDirectory() {
    return joinPath(loader::installDirectory(), kStudioFolder);
}

json buildState() {
    json config = readJsonFile(configPath());
    return {{"plugins", pluginInventory()},
            {"themes", themeInventory()},
            {"apps", appInventory()},
            {"config", config.is_object() ? config : json::object()},
            {"localCatalog", readJsonFile(joinPath(catalogDirectory(), L"catalog.json"))},
            {"rendering", loader::isRendering()},
            {"configDirectory", toUtf8(loader::configDirectory())},
            {"version", loader::coreVersion()},
            {"stubVersion", loader::stubVersion()}};
}

json readIni(const json &args) {
    const std::string file = args.value("file", "");
    const std::string section = args.value("section", "");
    if (!isSafeIniFile(file) || !util::isSafeId(section)) {
        throw std::runtime_error("Fichier de réglages invalide.");
    }
    const std::wstring path = joinPath(loader::configDirectory(), toWide(file));
    const std::wstring wideSection = toWide(section);
    json values = json::object();
    const json keys = args.value("keys", json::array());
    for (const json &key : keys) {
        const std::string name = key.is_string() ? key.get<std::string>() : "";
        if (!util::isSafeId(name)) {
            continue;
        }
        wchar_t buffer[kIniValueBufferSize] = {};
        const DWORD length = GetPrivateProfileStringW(wideSection.c_str(), toWide(name).c_str(), L"\x01", buffer,
                                                      kIniValueBufferSize, path.c_str());
        if (!(length == 1 && buffer[0] == L'\x01')) {
            values[name] = toUtf8(buffer);
        }
    }
    return values;
}

json writeIni(const json &args) {
    const std::string file = args.value("file", "");
    const std::string section = args.value("section", "");
    if (!isSafeIniFile(file) || !util::isSafeId(section)) {
        throw std::runtime_error("Fichier de réglages invalide.");
    }
    const std::wstring path = joinPath(loader::configDirectory(), toWide(file));
    const std::wstring wideSection = toWide(section);
    // Copie locale : items() sur le temporaire renvoyé par value() pointerait vers un objet détruit.
    const json values = args.value("values", json::object());
    for (const auto &[key, value] : values.items()) {
        if (!util::isSafeId(key)) {
            throw std::runtime_error("Nom de réglage invalide : " + key);
        }
        const std::string text = value.is_string() ? value.get<std::string>() : value.dump();
        if (!WritePrivateProfileStringW(wideSection.c_str(), toWide(key).c_str(), toWide(text).c_str(), path.c_str())) {
            throw std::runtime_error("Impossible d'écrire " + file + ".");
        }
    }
    loader::markConfigDirty();
    return {{"saved", true}};
}

json saveConfig(const json &args) {
    const json config = args.value("config", json::object());
    if (!config.is_object() || !util::writeFileAtomic(configPath(), config.dump(2))) {
        throw std::runtime_error("Impossible d'enregistrer la configuration du Studio.");
    }
    return {{"saved", true}};
}

json uninstall(const json &args) {
    const std::string type = args.value("type", "");
    const std::string id = args.value("id", "");
    if (!isItemType(type) || !util::isSafeId(id)) {
        throw std::runtime_error("Élément invalide.");
    }
    const std::wstring wideId = toWide(id);
    const std::wstring base = typeDirectory(type);
    if (type == "theme") {
        DeleteFileW(joinPath(base, wideId + L".json").c_str());
        return {{"removed", true}, {"restartRequired", false}};
    }
    const std::wstring target = joinPath(base, wideId);
    if (!util::directoryExists(target)) {
        throw std::runtime_error("Cet élément n'est pas installé depuis la boutique.");
    }
    if (type == "plugin" && containsId(loader::loadedPluginIds(), wideId)) {
        // DLL en cours d'utilisation : suppression au prochain démarrage.
        util::writeFileAtomic(joinPath(target, kRemovalMarker), "");
        loader::log("désinstallation programmée : %s", id.c_str());
        return {{"removed", false}, {"restartRequired", true}};
    }
    if (!util::removeTree(target)) {
        throw std::runtime_error("Suppression incomplète : un fichier est utilisé.");
    }
    loader::log("désinstallé : %s %s", type.c_str(), id.c_str());
    return {{"removed", true}, {"restartRequired", false}};
}

void installAsync(const json &item, Completion done) {
    std::thread([item, done]() {
        try {
            done(installItem(item), {});
        } catch (const std::exception &error) {
            loader::log("échec d'installation : %s", error.what());
            done(nullptr, error.what());
        }
    }).detach();
}

void fetchTextAsync(const std::string &url, Completion done) {
    std::thread([url, done]() {
        std::string body;
        std::string error;
        if (util::httpsGet(url, &body, &error)) {
            done(json{{"text", body}}, {});
        } else {
            done(nullptr, error);
        }
    }).detach();
}

void purgePendingRemovals(const std::wstring &directory) {
    for (const std::wstring &id : util::listEntries(directory, L"*", true)) {
        const std::wstring folder = joinPath(directory, id);
        if (util::fileExists(joinPath(folder, kRemovalMarker))) {
            util::removeTree(folder);
            loader::log("plugin supprimé au démarrage : %s", toUtf8(id).c_str());
        }
    }
}

} // namespace store
