/**
 * @file util.cpp
 * Implémentation des utilitaires partagés du moteur.
 */
#include "util.h"

#include <windows.h>
#include <bcrypt.h>
#include <wincrypt.h>
#include <winhttp.h>

#include <algorithm>

namespace util {

namespace {

constexpr size_t kMaxIdLength = 48;
constexpr size_t kMaxRelativePathLength = 160;
constexpr size_t kMaxFileBytes = 50u * 1024u * 1024u;
constexpr int kResolveTimeoutMs = 10000;
constexpr int kConnectTimeoutMs = 10000;
constexpr int kSendTimeoutMs = 15000;
constexpr int kReceiveTimeoutMs = 30000;
constexpr DWORD kHttpOk = 200;
constexpr size_t kSha256Bytes = 32;
constexpr size_t kUrlPartBufferSize = 2048;
constexpr size_t kHostBufferSize = 256;

} // namespace

std::wstring joinPath(const std::wstring &left, const std::wstring &right) {
    if (left.empty()) {
        return right;
    }
    return (left.back() == L'\\' ? left : left + L"\\") + right;
}

bool fileExists(const std::wstring &path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool directoryExists(const std::wstring &path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

void ensureDirectory(const std::wstring &path) {
    if (path.empty() || directoryExists(path)) {
        return;
    }
    const size_t separator = path.find_last_of(L'\\');
    if (separator != std::wstring::npos) {
        ensureDirectory(path.substr(0, separator));
    }
    CreateDirectoryW(path.c_str(), nullptr);
}

bool removeTree(const std::wstring &path) {
    WIN32_FIND_DATAW entry = {};
    HANDLE search = FindFirstFileW(joinPath(path, L"*").c_str(), &entry);
    bool ok = true;
    if (search != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring name = entry.cFileName;
            if (name == L"." || name == L"..") {
                continue;
            }
            const std::wstring child = joinPath(path, name);
            if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                ok = removeTree(child) && ok;
            } else {
                SetFileAttributesW(child.c_str(), FILE_ATTRIBUTE_NORMAL);
                ok = DeleteFileW(child.c_str()) != 0 && ok;
            }
        } while (FindNextFileW(search, &entry));
        FindClose(search);
    }
    return RemoveDirectoryW(path.c_str()) != 0 && ok;
}

bool readFile(const std::wstring &path, std::string *out) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    LARGE_INTEGER size = {};
    bool ok = GetFileSizeEx(file, &size) != 0 && static_cast<unsigned long long>(size.QuadPart) <= kMaxFileBytes;
    if (ok) {
        out->resize(static_cast<size_t>(size.QuadPart));
        DWORD read = 0;
        ok = out->empty() ||
             (ReadFile(file, out->data(), static_cast<DWORD>(out->size()), &read, nullptr) != 0 && read == out->size());
    }
    CloseHandle(file);
    return ok;
}

bool writeFileAtomic(const std::wstring &path, const std::string &data) {
    const size_t separator = path.find_last_of(L'\\');
    if (separator != std::wstring::npos) {
        ensureDirectory(path.substr(0, separator));
    }
    const std::wstring temporary = path + L".tmp";
    HANDLE file =
        CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD written = 0;
    const bool ok = data.empty() ||
                    (WriteFile(file, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) != 0 &&
                     written == data.size());
    CloseHandle(file);
    if (!ok) {
        DeleteFileW(temporary.c_str());
        return false;
    }
    return MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

std::vector<std::wstring> listEntries(const std::wstring &directory, const wchar_t *pattern, bool directories) {
    std::vector<std::wstring> names;
    WIN32_FIND_DATAW entry = {};
    HANDLE search = FindFirstFileW(joinPath(directory, pattern).c_str(), &entry);
    if (search == INVALID_HANDLE_VALUE) {
        return names;
    }
    do {
        const std::wstring name = entry.cFileName;
        const bool isDirectory = (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (name != L"." && name != L".." && isDirectory == directories) {
            names.push_back(name);
        }
    } while (FindNextFileW(search, &entry));
    FindClose(search);
    std::sort(names.begin(), names.end());
    return names;
}

bool isSafeId(const std::string &id) {
    if (id.empty() || id.size() > kMaxIdLength) {
        return false;
    }
    return std::all_of(id.begin(), id.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
    });
}

bool toSafeRelativePath(const std::string &relative, std::wstring *out) {
    if (relative.empty() || relative.size() > kMaxRelativePathLength || relative.front() == '/' ||
        relative.front() == '\\') {
        return false;
    }
    std::string segment;
    std::string normalized;
    for (size_t i = 0; i <= relative.size(); ++i) {
        const char c = i < relative.size() ? relative[i] : '/';
        if (c == '/' || c == '\\') {
            if (segment.empty() || segment == "." || segment == "..") {
                return false;
            }
            normalized += (normalized.empty() ? "" : "\\") + segment;
            segment.clear();
            continue;
        }
        const bool allowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
                             c == '-' || c == '_';
        if (!allowed) {
            return false;
        }
        segment += c;
    }
    *out = toWide(normalized);
    return true;
}

std::string sha256Raw(const std::string &data) {
    unsigned char digest[kSha256Bytes] = {};
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) {
        return {};
    }
    const NTSTATUS status =
        BCryptHash(algorithm, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<char *>(data.data())),
                   static_cast<ULONG>(data.size()), digest, sizeof(digest));
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return status == 0 ? std::string(reinterpret_cast<const char *>(digest), sizeof(digest)) : std::string();
}

std::string sha256Hex(const std::string &data) {
    static const char kHex[] = "0123456789abcdef";
    std::string hex;
    for (unsigned char byte : sha256Raw(data)) {
        hex += kHex[byte >> 4];
        hex += kHex[byte & 0x0F];
    }
    return hex;
}

bool base64Decode(const std::string &text, std::string *out) {
    DWORD size = 0;
    if (!CryptStringToBinaryA(text.c_str(), static_cast<DWORD>(text.size()), CRYPT_STRING_BASE64, nullptr, &size,
                              nullptr, nullptr)) {
        return false;
    }
    out->resize(size);
    return CryptStringToBinaryA(text.c_str(), static_cast<DWORD>(text.size()), CRYPT_STRING_BASE64,
                                reinterpret_cast<BYTE *>(out->data()), &size, nullptr, nullptr) != 0;
}

bool httpsGet(const std::string &url, std::string *body, std::string *error) {
    const std::wstring wideUrl = toWide(url);
    wchar_t host[kHostBufferSize] = {};
    wchar_t path[kUrlPartBufferSize] = {};
    wchar_t extra[kUrlPartBufferSize] = {};
    URL_COMPONENTS parts = {};
    parts.dwStructSize = sizeof(parts);
    parts.lpszHostName = host;
    parts.dwHostNameLength = kHostBufferSize;
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = kUrlPartBufferSize;
    parts.lpszExtraInfo = extra;
    parts.dwExtraInfoLength = kUrlPartBufferSize;
    if (!WinHttpCrackUrl(wideUrl.c_str(), 0, 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS) {
        *error = "Adresse invalide : seul HTTPS est accepté.";
        return false;
    }
    const std::wstring pathAndQuery = std::wstring(path) + extra;

    HINTERNET session = WinHttpOpen(L"LonelyScreenStudio/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET connection = session != nullptr ? WinHttpConnect(session, host, parts.nPort, 0) : nullptr;
    HINTERNET request = connection != nullptr
                            ? WinHttpOpenRequest(connection, L"GET", pathAndQuery.c_str(), nullptr, WINHTTP_NO_REFERER,
                                                 WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                            : nullptr;
    bool ok = false;
    if (request != nullptr) {
        WinHttpSetTimeouts(session, kResolveTimeoutMs, kConnectTimeoutMs, kSendTimeoutMs, kReceiveTimeoutMs);
        ok = WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
             WinHttpReceiveResponse(request, nullptr);
        DWORD status = 0;
        DWORD statusSize = sizeof(status);
        if (ok) {
            WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX);
            if (status != kHttpOk) {
                *error = "Le serveur a répondu " + std::to_string(status) + ".";
                ok = false;
            }
        } else {
            *error = "Connexion impossible (erreur " + std::to_string(GetLastError()) + ").";
        }
        while (ok) {
            DWORD available = 0;
            if (!WinHttpQueryDataAvailable(request, &available)) {
                *error = "Téléchargement interrompu.";
                ok = false;
                break;
            }
            if (available == 0) {
                break;
            }
            if (body->size() + available > kMaxFileBytes) {
                *error = "Fichier trop volumineux.";
                ok = false;
                break;
            }
            const size_t offset = body->size();
            body->resize(offset + available);
            DWORD read = 0;
            if (!WinHttpReadData(request, body->data() + offset, available, &read)) {
                *error = "Téléchargement interrompu.";
                ok = false;
                break;
            }
            body->resize(offset + read);
        }
    } else {
        *error = "Connexion impossible.";
    }
    if (request != nullptr) {
        WinHttpCloseHandle(request);
    }
    if (connection != nullptr) {
        WinHttpCloseHandle(connection);
    }
    if (session != nullptr) {
        WinHttpCloseHandle(session);
    }
    return ok;
}

std::string lowercase(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](char c) { return static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c); });
    return text;
}

bool startsWith(const std::string &text, const char *prefix) {
    return text.rfind(prefix, 0) == 0;
}

std::wstring toWide(const std::string &text) {
    if (text.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size);
    return out;
}

std::string toUtf8(const std::wstring &text) {
    if (text.empty()) {
        return {};
    }
    const int size =
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size, nullptr, nullptr);
    return out;
}

} // namespace util
