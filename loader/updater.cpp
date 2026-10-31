/**
 * @file updater.cpp
 * Implémentation des mises à jour signées du Studio.
 */
#include "updater.h"

#include <windows.h>
#include <bcrypt.h>

#include <atomic>
#include <cstring>
#include <thread>
#include <vector>

#include "loader_internal.h"
#include "lsstub.h"
#include "release_key.h"
#include "util.h"

namespace updater {

namespace {

using json = store::json;
using util::joinPath;

constexpr char kManifestName[] = "latest.json";
constexpr char kSignatureName[] = "latest.json.sig";
constexpr char kCoreFileName[] = "lsstudio.dll";
constexpr char kStubPrefix[] = "stub/";
constexpr size_t kKeyBytes = 32;
constexpr size_t kSignatureBytes = 64;
constexpr size_t kMaxVersionLength = 32;
constexpr size_t kMaxReleaseFiles = 512;

std::atomic<bool> g_applying{false};

std::wstring appBase() {
    wchar_t localAppData[MAX_PATH] = {};
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return {};
    }
    return joinPath(localAppData, LS_APP_FOLDER);
}

/** Flux de mise à jour : URL HTTPS (défaut GitHub) ou dossier local (LS_STUDIO_FEED, tests). */
std::string feed() {
    wchar_t custom[MAX_PATH] = {};
    const DWORD length = GetEnvironmentVariableW(L"LS_STUDIO_FEED", custom, MAX_PATH);
    if (length > 0 && length < MAX_PATH) {
        return util::toUtf8(custom);
    }
    return std::string("https://github.com/") + LS_STUDIO_REPO + "/releases/latest/download/";
}

bool feedIsRemote(const std::string &location) {
    return util::startsWith(location, "https://");
}

bool fetchFromFeed(const std::string &location, const std::string &name, std::string *bytes, std::string *error) {
    if (feedIsRemote(location)) {
        return util::httpsGet(location + name, bytes, error);
    }
    if (!util::readFile(joinPath(util::toWide(location), util::toWide(name)), bytes)) {
        *error = "Fichier absent du flux local : " + name;
        return false;
    }
    return true;
}

bool signingConfigured() {
    for (size_t i = 0; i < kKeyBytes; ++i) {
        if (kReleasePublicKeyX[i] != 0 || kReleasePublicKeyY[i] != 0) {
            return true;
        }
    }
    return false;
}

/** Vérifie une signature ECDSA P-256 (r||s) sur les octets exacts du manifeste. */
bool verifySignature(const std::string &data, const std::string &signature) {
    if (signature.size() != kSignatureBytes) {
        return false;
    }
    struct {
        BCRYPT_ECCKEY_BLOB header;
        unsigned char xy[2 * kKeyBytes];
    } blob = {};
    blob.header.dwMagic = BCRYPT_ECDSA_PUBLIC_P256_MAGIC;
    blob.header.cbKey = static_cast<ULONG>(kKeyBytes);
    std::memcpy(blob.xy, kReleasePublicKeyX, kKeyBytes);
    std::memcpy(blob.xy + kKeyBytes, kReleasePublicKeyY, kKeyBytes);

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_KEY_HANDLE key = nullptr;
    bool valid = false;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0) == 0 &&
        BCryptImportKeyPair(algorithm, nullptr, BCRYPT_ECCPUBLIC_BLOB, &key, reinterpret_cast<PUCHAR>(&blob),
                            sizeof(blob), 0) == 0) {
        std::string digest = util::sha256Raw(data);
        valid = BCryptVerifySignature(key, nullptr, reinterpret_cast<PUCHAR>(digest.data()),
                                      static_cast<ULONG>(digest.size()),
                                      reinterpret_cast<PUCHAR>(const_cast<char *>(signature.data())),
                                      static_cast<ULONG>(signature.size()), 0) == 0;
    }
    if (key != nullptr) {
        BCryptDestroyKey(key);
    }
    if (algorithm != nullptr) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
    }
    return valid;
}

bool isSafeVersion(const std::string &version) {
    if (version.empty() || version.size() > kMaxVersionLength) {
        return false;
    }
    for (char c : version) {
        const bool allowed = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.' ||
                             c == '-';
        if (!allowed) {
            return false;
        }
    }
    return true;
}

/** Compare deux versions "a.b.c" numériquement (suffixes ignorés). */
int compareVersions(const std::string &left, const std::string &right) {
    size_t i = 0;
    size_t j = 0;
    while (i < left.size() || j < right.size()) {
        long a = 0;
        long b = 0;
        while (i < left.size() && left[i] >= '0' && left[i] <= '9') {
            a = a * 10 + (left[i++] - '0');
        }
        while (j < right.size() && right[j] >= '0' && right[j] <= '9') {
            b = b * 10 + (right[j++] - '0');
        }
        if (a != b) {
            return a < b ? -1 : 1;
        }
        while (i < left.size() && left[i] != '.') {
            ++i;
        }
        while (j < right.size() && right[j] != '.') {
            ++j;
        }
        if (i < left.size()) {
            ++i;
        }
        if (j < right.size()) {
            ++j;
        }
    }
    return 0;
}

/** Télécharge et vérifie le manifeste signé. */
json loadVerifiedManifest(const std::string &location) {
    if (!signingConfigured()) {
        throw std::runtime_error("Mises à jour non configurées dans cette version (clé de signature absente).");
    }
    std::string manifestBytes;
    std::string signatureText;
    std::string error;
    if (!fetchFromFeed(location, kManifestName, &manifestBytes, &error) ||
        !fetchFromFeed(location, kSignatureName, &signatureText, &error)) {
        throw std::runtime_error("Impossible de joindre le serveur de mises à jour : " + error);
    }
    std::string signature;
    if (!util::base64Decode(signatureText, &signature) || !verifySignature(manifestBytes, signature)) {
        loader::log("mise à jour refusée : signature invalide");
        throw std::runtime_error("Signature de la mise à jour invalide : elle a été refusée par sécurité.");
    }
    json manifest = json::parse(manifestBytes, nullptr, false);
    if (!manifest.is_object() || !isSafeVersion(manifest.value("version", ""))) {
        throw std::runtime_error("Manifeste de mise à jour illisible.");
    }
    return manifest;
}

json checkNow() {
    const std::string location = feed();
    const json manifest = loadVerifiedManifest(location);
    const std::string latest = manifest.value("version", "");
    const int minStub = manifest.value("minStub", 1);
    return {{"current", loader::coreVersion()},
            {"latest", latest},
            {"available", compareVersions(latest, loader::coreVersion()) > 0},
            {"notes", manifest.value("notes", "")},
            {"stubUpdateRequired", minStub > loader::stubVersion()}};
}

json applyNow() {
    const std::wstring base = appBase();
    if (base.empty()) {
        throw std::runtime_error("Dossier LOCALAPPDATA introuvable.");
    }
    const std::string location = feed();
    const json manifest = loadVerifiedManifest(location);
    const std::string version = manifest.value("version", "");
    if (compareVersions(version, loader::coreVersion()) <= 0) {
        return {{"version", version}, {"updated", false}, {"restartRequired", false}};
    }
    if (manifest.value("minStub", 1) > loader::stubVersion()) {
        throw std::runtime_error("Cette version demande de relancer la commande d'installation (droits administrateur).");
    }
    const json files = manifest.value("files", json::array());
    if (!files.is_array() || files.empty() || files.size() > kMaxReleaseFiles) {
        throw std::runtime_error("Manifeste de mise à jour sans fichiers.");
    }

    const std::wstring versions = joinPath(base, L"versions");
    const std::wstring wideVersion = util::toWide(version);
    const std::wstring staging = joinPath(versions, wideVersion + L".staging");
    util::removeTree(staging);
    util::ensureDirectory(staging);
    bool hasCore = false;
    for (const json &file : files) {
        const std::string path = file.value("path", "");
        if (util::startsWith(path, kStubPrefix)) {
            continue; // le relais est installé par la commande d'installation, avec droits admin
        }
        std::wstring relative;
        const std::string expected = util::lowercase(file.value("sha256", ""));
        if (!util::toSafeRelativePath(path, &relative) || expected.empty()) {
            util::removeTree(staging);
            throw std::runtime_error("Manifeste de mise à jour invalide (chemin ou empreinte).");
        }
        std::string bytes;
        std::string error;
        const bool fetched = feedIsRemote(location) ? util::httpsGet(file.value("url", ""), &bytes, &error)
                                                    : fetchFromFeed(location, file.value("asset", ""), &bytes, &error);
        if (!fetched || util::sha256Hex(bytes) != expected) {
            util::removeTree(staging);
            throw std::runtime_error(fetched ? "Fichier corrompu : " + path : "Téléchargement impossible : " + error);
        }
        if (!util::writeFileAtomic(joinPath(staging, relative), bytes)) {
            util::removeTree(staging);
            throw std::runtime_error("Écriture impossible dans " + util::toUtf8(staging));
        }
        hasCore = hasCore || path == kCoreFileName;
    }
    if (!hasCore) {
        util::removeTree(staging);
        throw std::runtime_error("Mise à jour incomplète : moteur absent.");
    }

    const std::wstring target = joinPath(versions, wideVersion);
    if (util::directoryExists(target)) {
        util::removeTree(target);
    }
    if (!MoveFileExW(staging.c_str(), target.c_str(), 0) ||
        !util::writeFileAtomic(joinPath(base, L"current.txt"), version)) {
        throw std::runtime_error("Activation de la nouvelle version impossible.");
    }
    loader::log("mise à jour %s prête (active au prochain démarrage)", version.c_str());
    return {{"version", version}, {"updated", true}, {"restartRequired", true}};
}

} // namespace

store::json status() {
    return {{"version", loader::coreVersion()},
            {"stubVersion", loader::stubVersion()},
            {"signingConfigured", signingConfigured()},
            {"feed", feed()}};
}

void checkAsync(store::Completion done) {
    std::thread([done]() {
        try {
            done(checkNow(), {});
        } catch (const std::exception &error) {
            done(nullptr, error.what());
        }
    }).detach();
}

void applyAsync(store::Completion done) {
    if (g_applying.exchange(true)) {
        done(nullptr, "Une mise à jour est déjà en cours.");
        return;
    }
    std::thread([done]() {
        try {
            done(applyNow(), {});
        } catch (const std::exception &error) {
            loader::log("échec de mise à jour : %s", error.what());
            done(nullptr, error.what());
        }
        g_applying.store(false);
    }).detach();
}

void cleanupOldVersionsAsync() {
    const std::wstring base = appBase();
    std::wstring running = loader::installDirectory();
    running = running.substr(running.find_last_of(L'\\') + 1);
    std::thread([base, running]() {
        if (base.empty()) {
            return;
        }
        std::string current;
        util::readFile(joinPath(base, L"current.txt"), &current);
        while (!current.empty() && (current.back() == '\n' || current.back() == '\r' || current.back() == ' ')) {
            current.pop_back();
        }
        const std::wstring versions = joinPath(base, L"versions");
        for (const std::wstring &name : util::listEntries(versions, L"*", true)) {
            if (name != util::toWide(current) && name != running) {
                util::removeTree(joinPath(versions, name));
            }
        }
    }).detach();
}

} // namespace updater
