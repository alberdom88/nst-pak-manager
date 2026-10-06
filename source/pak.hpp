// pak.hpp - lettura e scrittura degli archivi .pak del gioco (formato igArchive)
//
// Serve per creare update.pak sulla Switch: i file di un archivio originale vengono copiati
// cosi' come sono (anche quelli compressi, senza decomprimerli) e si aggiungono file nuovi
// non compressi. Stesso formato scritto da Crash NST Maker (versione 12 = Switch).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pak {

extern const char* const SWITCH_ROOT;  // temporary/mack/data/nx/output/

struct Entry {
    std::string fullPath;  // con la radice (temporary/mack/data/...)
    std::string path;      // percorso breve (maps/..., packages/...)
    uint32_t hash = 0;
    int32_t size = 0;            // dimensione non compressa
    uint32_t compression = 0;    // 0 = nessuna, 1 = zlib, 2 = lzma
    std::vector<uint32_t> blocks;  // voci della tabella dei blocchi (offset in settori + flag)
    uint64_t offset = 0;           // posizione dei dati nell'archivio di origine
    uint64_t stored = 0;           // byte occupati dai dati (senza il riempimento finale)
    bool inMemory = false;         // dati in "data" invece che nell'archivio di origine
    std::string data;
};

struct Archive {
    std::string path;  // file su disco da cui copiare i dati
    uint32_t version = 0;
    std::vector<Entry> entries;
};

// Hash dei percorsi usato dal gioco (FNV-1a sul percorso in minuscolo)
uint32_t hashPath(const std::string& path);

bool read(const std::string& path, Archive& out, std::string& err);

// Contenuto di un file non compresso
bool extract(const Archive& a, const Entry& e, std::string& out, std::string& err);

// Aggiunge (o sostituisce, se il percorso c'e' gia') un file non compresso
void put(Archive& a, const std::string& path, const std::string& data);

// Scrive l'archivio (ordinato per hash, con i parametri di ricerca ricalcolati)
bool write(const Archive& a, const std::string& outPath, std::string& err);

}  // namespace pak
