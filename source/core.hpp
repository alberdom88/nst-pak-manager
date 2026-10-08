// core.hpp - configurazione, sorgenti remote e preparazione del livello da giocare
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "net.hpp"

struct RemoteFile {
    std::string name;    // nome del file nella sorgente remota (es. MioLivello_v2.pak)
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

// Livelli di un .pak su disco (file packages/generated/maps/<gioco>/<L>/<L>_pkg.igz), con
// l'identificativo usato dal gioco per aprirli: <gioco>/<cartella>/<livello> in minuscolo,
// es. crash1/l112_roadtonowhere/l112_roadtonowhere. Legge solo intestazione ed elenco dei percorsi.
// false se il file non si riesce a leggere.
bool pakLevelIds(const std::string& path, std::vector<std::string>& out);
// Nome del .pak di un livello come lo cerca il gioco: archives/<livello in minuscolo>.pak
// (la romfs distingue le maiuscole). Dall'identificativo o dal solo nome del livello.
std::string levelPakName(const std::string& level);

// Gioco scelto per l'avvio diretto: "auto" (quello del livello), "crash1", "crash2", "crash3"
extern const char* const GAMES[4];
std::string normalizeGame(const std::string& game);  // valori sconosciuti -> "auto"
std::string nextGame(const std::string& game, int step);  // scorre Auto -> Crash 1 -> 2 -> 3
std::string gameLabel(const std::string& game);       // "Auto", "Crash 1", ...
// Identificativo da aprire: con "auto" quello del livello, altrimenti con la prima parte sostituita
std::string levelIdForGame(const std::string& levelId, const std::string& game);

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

// Risultato della preparazione di un livello
struct Prepared {
    std::string pakName;   // nome con cui e' installato (custom_level.pak)
    std::string levelId;   // identificativo letto dal .pak (crash1/custom_level/custom_level)
    std::string launchId;  // identificativo scritto in debug.xml (dipende dal gioco scelto)
    bool registered = false;     // update.pak creato con la registrazione del livello
    bool updateRemoved = false;  // update.pak di un livello precedente tolto (questo non lo usa)
    std::string removed;         // livello precedente tolto dalla cartella delle mod (vuoto se nessuno)
};

class Manager {
public:
    // root: "sdmc:" sulla Switch, una cartella qualsiasi nei test
    Manager(const std::string& root, const Config& cfg);

    // Crea la cartella dell'app e legge lo stato (ultimo livello, gioco scelto)
    bool load(std::string& err);

    // Scarica sempre il file remoto e prepara tutto per giocarlo:
    //  1. download in un file temporaneo (.part) nella cartella delle mod, con controlli;
    //  2. il .pak deve contenere un livello: si installa come <livello in minuscolo>.pak,
    //     al posto del livello installato la volta prima (che viene tolto);
    //  3. update.pak: se il livello ha file update/ (livello nuovo, convertito con --nuovo) li unisce
    //     alla copia dell'update.pak originale (se c'e') e lo scrive; altrimenti toglie quello vecchio;
    //  4. debug.xml con il livello da aprire (gioco scelto con setGame).
    // Se il download o i controlli falliscono non cambia nulla.
    bool prepare(const RemoteFile& file, const net::Progress& progress, Prepared& out, std::string& err);

    const std::string& game() const { return game_; }
    bool setGame(const std::string& game, std::string& err);
    const std::string& lastLevel() const { return lastLevel_; }  // ultimo .pak installato

    // Copia dell'update.pak originale del gioco, unita alla registrazione dei livelli nuovi
    std::string originalUpdatePath() const;

    // Avvio diretto in un livello: debug.xml nella radice della romfs della mod
    std::string romfsDir() const;
    std::string directLaunchPath() const { return romfsDir() + "/debug.xml"; }
    bool setDirectLaunch(const std::string& levelId, std::string& err);
    void clearDirectLaunch();
    std::string directLaunchLevel() const;  // vuoto se l'avvio diretto non e' attivo
    bool directLaunchPatchInstalled() const;

    // Elimina download interrotti (*.part); ritorna quanti ne ha eliminati
    int cleanupPartials();

    const std::string& modDir() const { return modDir_; }
    const std::string& appDir() const { return appDir_; }

private:
    bool download(const RemoteFile& file, const net::Progress& progress, const std::string& tmp, std::string& err);
    bool writeUpdate(const std::string& pakName, Prepared& out, std::string& err);
    bool save(std::string& err);

    std::string root_, modDir_, appDir_, statePath_;
    std::string lastLevel_;
    std::string game_ = "auto";
};

// Percorsi standard dell'app sulla SD
std::string appDirFor(const std::string& root);
