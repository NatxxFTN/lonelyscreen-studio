/**
 * @file util.h
 * Utilitaires partagés du moteur : fichiers, chemins sûrs, HTTPS, SHA-256, texte.
 */
#pragma once

#include <string>
#include <vector>

namespace util {

std::wstring joinPath(const std::wstring &left, const std::wstring &right);
bool fileExists(const std::wstring &path);
bool directoryExists(const std::wstring &path);

/** Crée un dossier et tous ses parents. */
void ensureDirectory(const std::wstring &path);

/** Supprime un dossier et son contenu. @return false si un élément n'a pas pu être supprimé. */
bool removeTree(const std::wstring &path);

/** Lit un fichier entier (50 Mo max). */
bool readFile(const std::wstring &path, std::string *out);

/** Écrit via un fichier temporaire puis remplace : jamais de fichier à moitié écrit. */
bool writeFileAtomic(const std::wstring &path, const std::string &data);

/** Noms des sous-dossiers (directories = true) ou fichiers correspondant au motif, triés. */
std::vector<std::wstring> listEntries(const std::wstring &directory, const wchar_t *pattern, bool directories);

/** Identifiant sûr : [a-z0-9_-], 1 à 48 caractères. */
bool isSafeId(const std::string &id);

/** Chemin relatif sûr (pas de "..", pas de lecteur, caractères limités) converti en chemin Windows. */
bool toSafeRelativePath(const std::string &relative, std::wstring *out);

/** Empreinte SHA-256 en hexadécimal minuscule. */
std::string sha256Hex(const std::string &data);

/** Empreinte SHA-256 brute (32 octets). */
std::string sha256Raw(const std::string &data);

/** Décode du Base64 ; renvoie false si le texte est invalide. */
bool base64Decode(const std::string &text, std::string *out);

/** GET HTTPS vers la mémoire (redirections suivies, taille bornée). */
bool httpsGet(const std::string &url, std::string *body, std::string *error);

std::string lowercase(std::string text);
bool startsWith(const std::string &text, const char *prefix);
std::wstring toWide(const std::string &text);
std::string toUtf8(const std::wstring &text);

} // namespace util
