// net.hpp - richieste HTTP(S) con libcurl
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace net {

// Ritorna false per annullare il trasferimento
using Progress = std::function<bool(uint64_t done, uint64_t total)>;

bool init();
void shutdown();

// File PEM con certificati aggiuntivi (vuoto = solo certificati di sistema)
void setCaFile(const std::string& path);

// Scarica una risorsa piccola in memoria (elenchi, JSON).
// effectiveUrl (opzionale) riceve l'URL finale dopo eventuali redirect.
bool get(const std::string& url, std::string& body, long& status, std::string& err,
         std::string* effectiveUrl = nullptr);

// Richiesta POST con corpo JSON; la risposta finisce in body (anche con stato HTTP >= 400)
bool post(const std::string& url, const std::string& data, std::string& body, long& status,
          std::string& err);

// Dimensione (Content-Length) di ogni URL con richieste HEAD; -1 se sconosciuta.
// Si ferma al primo errore di connessione per non bloccare l'elenco.
std::vector<int64_t> contentLengths(const std::vector<std::string>& urls);

// Trasforma i dati ricevuti prima di scriverli (es. decifratura), nell'ordine in cui arrivano
using Transform = std::function<void(char* data, size_t size)>;

// Scarica un file su disco. In caso di errore il file parziale viene eliminato.
bool download(const std::string& url, const std::string& path, uint64_t expectedSize,
              const Progress& progress, uint64_t& written, std::string& err,
              const Transform* transform = nullptr);

}  // namespace net
