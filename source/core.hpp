// core.hpp - configurazione, sorgenti remote e gestione installazioni/backup
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "net.hpp"

struct RemoteFile {
    std::string name;    // nome del file nella sorgente remota (es. MioLivello_v2.pak)
    std::string target;  // originale suggerito dal manifest ("target"), vuoto se assente
    std::string url;
    uint64_t size = 0;
    bool sizeKnown = false;
};

struct SourceConfig {
    std::string name;
    std::string type;    // "http" oppure "gdrive"
    std::string url;     // http: cartella (elenco file) o manifest .json
    std::string folder;  // gdrive: ID o link della cartella condivisa
    std::string apiKey;  // gdrive: chiave API di Google
    std::string apiBase = "https://www.googleapis.com";  // usato solo nei test
};

struct Config {
    std::string titleId = "0100D1B006744000";  // Crash Bandicoot N. Sane Trilogy
    std::string modDir = "/atmosphere/contents/{title_id}/romfs/archives";
    std::string caFile;
    // Argomenti per avviare il gioco direttamente in un livello ({livello} = crash1/l112_x/l112_x).
    // Il primo elemento fa da nome del programma (il gioco lo salta).
    std::string launchArgs = "nst -om {livello}";
    std::vector<SourceConfig> sources;
};

std::string defaultConfigJson();
bool parseConfig(const std::string& json, Config& cfg, std::string& err);

// Elenco dei .pak disponibili nella sorgente, ordinati per nome
bool listSource(const SourceConfig& src, std::vector<RemoteFile>& out, std::string& err);

// Funzioni esposte per i test
bool isValidPakName(const std::string& name);
std::string resolveUrl(const std::string& base, const std::string& href);
void parseDirectoryListing(const std::string& html, const std::string& baseUrl,
                           std::vector<RemoteFile>& out);
bool parseManifest(const std::string& json, const std::string& manifestUrl,
                   std::vector<RemoteFile>& out, std::string& err);
std::string driveFolderId(const std::string& folderOrLink);

// Elenco dei .pak originali del gioco: un nome per riga, righe vuote e '#' ignorate,
// eventuali percorsi ridotti al solo nome. Ordinato, senza doppioni.
std::vector<std::string> parseOriginals(const std::string& text);

// Livelli dichiarati in un .pak su disco (file packages/generated/maps/<gioco>/<L>/<L>_pkg.igz).
// Legge solo intestazione ed elenco dei percorsi. false se il file non si riesce a leggere.
bool pakLevelNames(const std::string& path, std::vector<std::string>& out);
// Come sopra, ma con l'identificativo usato dal gioco per aprire il livello
// (<gioco>/<cartella>/<livello> in minuscolo, es. crash1/l112_roadtonowhere/l112_roadtonowhere)
bool pakLevelIds(const std::string& path, std::vector<std::string>& out);
// Argomenti di avvio per un livello: sostituisce {livello} nel modello
std::string launchArguments(const std::string& pattern, const std::string& levelId);
bool loadOriginals(const std::string& path, std::vector<std::string>& out);

struct InstalledFile {
    std::string name;    // nome nella cartella del gioco (l'originale sostituito)
    std::string remote;  // nome del file scaricato
    std::string source;
    uint64_t size = 0;
    std::string backup;  // nome del file nella cartella backup, vuoto se non c'era nulla da salvare
};

class Manager {
public:
    // root: "sdmc:" sulla Switch, una cartella qualsiasi nei test
    Manager(const std::string& root, const Config& cfg);

    // Se lo stato salvato e' illeggibile viene messo da parte e warning spiega cosa e' successo
    bool load(std::string& err, std::string& warning);

    const std::vector<InstalledFile>& installed() const { return installed_; }
    const InstalledFile* find(const std::string& name) const;
    // Installazioni che vengono da questo file remoto
    std::vector<const InstalledFile*> installedFrom(const std::string& remoteName) const;
    // Ultimo originale scelto per questo file remoto (vuoto se mai scelto)
    std::string rememberedTarget(const std::string& remoteName) const;
    // Originale da proporre: scelta precedente, poi "target" del manifest,
    // poi lo stesso nome se e' tra gli originali. Vuoto se non c'e' un suggerimento.
    std::string suggestTarget(const RemoteFile& file, const std::vector<std::string>& originals) const;
    bool existsInModDir(const std::string& name) const;
    // .pak presenti nella cartella mod ma non installati da questa app
    std::vector<std::string> externalFiles() const;
    // Elimina download interrotti (*.part); ritorna quanti ne ha eliminati
    int cleanupPartials();

    // Scarica il file remoto, lo controlla e lo installa con il nome target
    // (l'originale del gioco da sostituire). Se nella cartella mod c'era gia' un
    // file target non installato da questa app, viene spostato nella cartella backup.
    bool install(const RemoteFile& file, const std::string& target, const std::string& sourceName,
                 const net::Progress& progress, std::string& err);
    // Rimuove il file installato e rimette il backup. In caso di successo
    // note puo' contenere un avviso (es. backup non piu' presente).
    bool restore(const std::string& name, std::string& err, std::string& note);

    const std::string& modDir() const { return modDir_; }
    const std::string& appDir() const { return appDir_; }
    const std::string& backupDir() const { return backupDir_; }

private:
    bool save(std::string& err);
    std::string uniqueBackupName(const std::string& name) const;

    std::string modDir_, appDir_, backupDir_, statePath_;
    std::vector<InstalledFile> installed_;
    std::vector<std::pair<std::string, std::string>> remembered_;  // remoto -> originale
};

// Percorsi standard dell'app sulla SD
std::string appDirFor(const std::string& root);
