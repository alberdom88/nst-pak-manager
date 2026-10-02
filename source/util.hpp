// util.hpp - piccole funzioni di supporto (file, percorsi, URL, formattazione)
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace util {

std::string joinPath(const std::string& a, const std::string& b);

bool fileExists(const std::string& path);
bool dirExists(const std::string& path);
bool fileSize(const std::string& path, uint64_t& out);

// Crea tutte le cartelle del percorso (anche con prefisso dispositivo "sdmc:")
bool mkdirs(const std::string& path);
bool removeFile(const std::string& path);
// Rinomina; fallisce se la destinazione esiste (come sul filesystem della Switch)
bool moveFile(const std::string& from, const std::string& to);

bool readFile(const std::string& path, std::string& out);
// Scrive su path.tmp e poi sostituisce path
bool writeFileAtomic(const std::string& path, const std::string& data);

// Solo file regolari, senza percorso
std::vector<std::string> listFiles(const std::string& dir);

// Spazio libero sul volume che contiene path
bool freeSpace(const std::string& path, uint64_t& out);

std::string toLower(std::string s);
bool endsWithCI(const std::string& s, const std::string& suffix);
std::string urlEncode(const std::string& s);
// Decodifica solo le sequenze %XX (percorsi URL, '+' resta '+')
std::string urlDecode(const std::string& s);
std::string formatSize(uint64_t bytes);

}  // namespace util
