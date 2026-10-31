/**
 * @file lsplugin_util.h
 * Petits utilitaires partagés par les plugins (lecture de .ini, couleurs).
 */
#pragma once

#include <cwchar>
#include <string>

#include "lsplugin.h"

namespace ls {

constexpr DWORD kIniBufferSize = 256;
constexpr unsigned kRgbHexDigits = 6;
constexpr unsigned kArgbHexDigits = 8;
constexpr int kHexBase = 16;
constexpr D3DCOLOR kOpaqueAlpha = 0xFF000000u;
constexpr unsigned kAlphaShift = 24;
constexpr D3DCOLOR kByteMask = 0xFFu;

/** Chemin complet d'un fichier .ini dans le dossier de réglages (%APPDATA%). */
inline std::wstring iniPath(const LsHostApi *host, const wchar_t *fileName) {
    return std::wstring(host->configDirectory()) + L"\\" + fileName;
}

/** Lit une chaîne dans un .ini, avec valeur par défaut. */
inline std::wstring readIniString(const std::wstring &path, const wchar_t *section, const wchar_t *key,
                                  const wchar_t *fallback) {
    wchar_t buffer[kIniBufferSize] = {};
    GetPrivateProfileStringW(section, key, fallback, buffer, kIniBufferSize, path.c_str());
    return buffer;
}

/** Lit un entier dans un .ini, avec valeur par défaut. */
inline int readIniInt(const std::wstring &path, const wchar_t *section, const wchar_t *key, int fallback) {
    return static_cast<int>(GetPrivateProfileIntW(section, key, fallback, path.c_str()));
}

/**
 * Convertit "#RRGGBB" (opaque) ou "#AARRGGBB" en D3DCOLOR.
 * @return false si le texte n'est pas une couleur valide.
 */
inline bool parseColor(const std::wstring &text, D3DCOLOR *out) {
    std::wstring hex = text;
    if (!hex.empty() && hex[0] == L'#') {
        hex.erase(0, 1);
    }
    if (hex.size() != kRgbHexDigits && hex.size() != kArgbHexDigits) {
        return false;
    }
    wchar_t *end = nullptr;
    unsigned long value = std::wcstoul(hex.c_str(), &end, kHexBase);
    if (end == nullptr || *end != L'\0') {
        return false;
    }
    if (hex.size() == kRgbHexDigits) {
        value |= kOpaqueAlpha;
    }
    *out = static_cast<D3DCOLOR>(value);
    return true;
}

/** Canal alpha (0-255) d'une couleur. */
inline unsigned alphaOf(D3DCOLOR color) {
    return static_cast<unsigned>((color >> kAlphaShift) & kByteMask);
}

} // namespace ls
