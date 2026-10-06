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
    // MEGA: l'indirizzo del download si chiede all'API al momento, i dati arrivano cifrati
    std::string megaApi, megaFolder, megaNode;
    std::string megaKey;  // chiave del file (32 byte, gia' decifrata)
};

struct SourceConfig {
    std::string name;
    std::string type;     // "http" oppure "mega"
    std::string url;      // http: cartella (elenco file) o manifest .json; mega: link della cartella
    std::string apiBase;  // mega: indirizzo dell'API (vuoto = quello ufficiale; usato nei test)
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
// File di configurazione di sviluppo letto dal gioco all'avvio (debug.xml): con
// <MAP filename="..."/> il gioco carica quel livello invece del menu (come -om).
// La versione in commercio lo legge solo con la patch creata da tools/crea_avvio_livello.py.
std::string debugXml(const std::string& levelId);
// Livello indicato in un debug.xml (attributo filename di <MAP>), vuoto se manca
std::string debugXmlLevel(const std::string& xml);
// Cartella delle patch dell'eseguibile per l'avvio diretto (sotto /atmosphere/exefs_patches)
extern const char* const DIRECT_LAUNCH_PATCH_DIR;
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
    // target vuoto = nome automatico: per un livello quello che ha dentro (l'unico con cui il
    // gioco lo trova), altrimenti l'originale suggerito o il nome del file remoto.
    // installedAs riceve il nome usato.
    bool install(const RemoteFile& file, const std::string& target, const std::string& sourceName,
                 const net::Progress& progress, std::string& err, std::string* installedAs = nullptr);
    // Elenco dei .pak originali (originali.txt): grafia dei nomi automatici e suggerimenti
    void setOriginals(const std::vector<std::string>& originals) { originals_ = originals; }
    // Livelli nuovi (convertiti con --nuovo): unisce i file update/ del .pak installato all'update.pak
    // originale (copia in originalUpdatePath()) e installa il risultato come update.pak, con backup.
    // registered = false se il livello non ha nulla da registrare (ha il nome di un livello originale).
    bool registerLevel(const std::string& pakName, bool& registered, std::string& err);
    std::string originalUpdatePath() const;

    // Rimuove il file installato e rimette il backup. In caso di successo
    // note puo' contenere un avviso (es. backup non piu' presente).
    bool restore(const std::string& name, std::string& err, std::string& note);

    // Avvio diretto in un livello: debug.xml nella radice della romfs della mod
    std::string romfsDir() const;
    std::string directLaunchPath() const { return romfsDir() + "/debug.xml"; }
    bool setDirectLaunch(const std::string& levelId, std::string& err);
    void clearDirectLaunch();
    std::string directLaunchLevel() const;  // vuoto se l'avvio diretto non e' attivo
    bool directLaunchPatchInstalled() const;

    const std::string& modDir() const { return modDir_; }
    const std::string& appDir() const { return appDir_; }
    const std::string& backupDir() const { return backupDir_; }

private:
    bool save(std::string& err);
    std::string autoTarget(const RemoteFile& file, const std::string& downloaded) const;
    bool place(const std::string& tmp, const std::string& target, const std::string& remoteName,
               const std::string& sourceName, uint64_t written, std::string& err);
    std::string uniqueBackupName(const std::string& name) const;

    std::string root_, modDir_, appDir_, backupDir_, statePath_;
    std::vector<InstalledFile> installed_;
    std::vector<std::pair<std::string, std::string>> remembered_;  // remoto -> originale
    std::vector<std::string> originals_;
};

// Percorsi standard dell'app sulla SD
std::string appDirFor(const std::string& root);
