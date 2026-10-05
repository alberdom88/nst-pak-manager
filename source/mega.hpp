// mega.hpp - cartelle condivise di MEGA (mega.nz): elenco dei file e download con decifratura
//
// Un link di cartella MEGA contiene l'identificativo della cartella e la sua chiave:
//   https://mega.nz/folder/<cartella>#<chiave>            (anche .../folder/<sottocartella>)
//   https://mega.nz/#F!<cartella>!<chiave>                (formato vecchio)
// Nomi e contenuti dei file sono cifrati (AES-128): l'app li decifra con la chiave del link.
// Nessun account e nessuna chiave API: basta il link pubblico.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "net.hpp"

struct RemoteFile;
struct SourceConfig;

namespace mega {

extern const char* const DEFAULT_API;  // https://g.api.mega.co.nz

struct Link {
    std::string folder;  // identificativo pubblico della cartella
    std::string key;     // chiave della cartella (16 byte)
    std::string sub;     // sottocartella da usare come radice (vuoto = la cartella del link)
};

// false se il testo non e' un link di cartella MEGA con la chiave
bool parseLink(const std::string& text, Link& out, std::string& err);

// Base64 di MEGA (alfabeto URL: - e _, senza '=')
std::string b64decode(const std::string& s);

// AES-128 su singoli blocchi (implementazione portabile)
class Aes128 {
public:
    explicit Aes128(const void* key16);
    void encrypt(const uint8_t in[16], uint8_t out[16]) const;
    void decrypt(const uint8_t in[16], uint8_t out[16]) const;

private:
    uint8_t rk_[176];
};

// AES-128-CTR a flusso (i dati possono arrivare a pezzi di qualsiasi dimensione)
class Ctr {
public:
    Ctr(const void* key16, const void* iv16);
    ~Ctr();
    void crypt(uint8_t* data, size_t n);

private:
    void* impl_;
};

// Chiave di un nodo cifrata con la chiave della cartella (campo "k": "<handle>:<chiave>")
std::string decryptNodeKey(const std::string& k, const std::string& folderKey, const std::string& folderHandle);
// Chiave AES di un file (32 byte del nodo -> 16 byte) e IV iniziale del CTR
std::string fileAesKey(const std::string& nodeKey);
std::string fileIv(const std::string& nodeKey);
// Nome dagli attributi cifrati (campo "a"); vuoto se la chiave e' sbagliata
std::string decryptName(const std::string& attrB64, const std::string& aesKey);

// Elenco dei .pak nella cartella (solo quelli direttamente nella cartella del link)
bool list(const SourceConfig& src, std::vector<RemoteFile>& out, std::string& err);

// Scarica e decifra un file elencato da list()
bool download(const RemoteFile& file, const std::string& path, const net::Progress& progress,
              uint64_t& written, std::string& err);

}  // namespace mega
