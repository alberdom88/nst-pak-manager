// main.cpp - interfaccia console per Nintendo Switch (libnx)
//
// Una sola schermata: l'elenco dei livelli della sorgente. A scarica il livello sotto il cursore,
// crea update.pak se serve, scrive debug.xml e avvia il gioco direttamente nel livello.
// Testi solo ASCII: il font della console libnx non ha le lettere accentate.

#include <switch.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "core.hpp"
#include "net.hpp"
#include "util.hpp"

#define APP_VERSION_STR "2.0"

static const char* ROOT = "sdmc:";
static const int COLS = 79;  // la console e' 80x45: lasciamo libera l'ultima colonna
static const int LIST_TOP = 5;
static const int LIST_ROWS = 35;

static const char* C_RESET = "\x1b[0m";
static const char* C_TITLE = "\x1b[36m";
static const char* C_REV = "\x1b[7m";
static const char* C_OK = "\x1b[32m";
static const char* C_WARN = "\x1b[33m";
static const char* C_ERR = "\x1b[31m";
static const char* C_DIM = "\x1b[90m";

static PadState g_pad;
static bool g_exit = false;
static bool g_pmOk = false;
static bool g_ldrOk = false;  // ldr:shel: argomenti di avvio del gioco (avvio diretto di un livello)

// ---------------------------------------------------------------- disegno

static void cls() { printf("\x1b[2J"); }

static std::string fit(const std::string& s, size_t width) {
    if (s.size() > width) return width ? s.substr(0, width - 1) + "~" : "";
    return s + std::string(width - s.size(), ' ');
}

static std::string rfit(const std::string& s, size_t width) {
    if (s.size() >= width) return s.substr(0, width);
    return std::string(width - s.size(), ' ') + s;
}

static void line(int row, const std::string& text, const char* color = nullptr) {
    printf("\x1b[%d;1H%s%s%s\x1b[K", row, color ? color : "", fit(text, COLS).c_str(), C_RESET);
}

static void drawRow(int row, const std::string& text, bool highlighted, const char* color) {
    if (highlighted)
        printf("\x1b[%d;1H%s%s%s\x1b[K", row, C_REV, fit(text, COLS).c_str(), C_RESET);
    else
        line(row, text, color);
}

// Va a capo tra le parole, rispettando il rientro iniziale della riga
static std::vector<std::string> wrap(const std::string& text, size_t width) {
    size_t indent = text.find_first_not_of(' ');
    if (indent == std::string::npos) return {};
    if (indent > width / 2) indent = 0;
    const std::string body = text.substr(indent);
    const size_t w = width - indent;
    std::vector<std::string> out;
    std::string cur;
    size_t i = 0;
    while (i < body.size()) {
        size_t sp = body.find(' ', i);
        std::string word = body.substr(i, sp == std::string::npos ? std::string::npos : sp - i);
        i = sp == std::string::npos ? body.size() : sp + 1;
        while (word.size() > w) {
            if (!cur.empty()) {
                out.push_back(cur);
                cur.clear();
            }
            out.push_back(word.substr(0, w));
            word = word.substr(w);
        }
        if (cur.size() + word.size() + (cur.empty() ? 0 : 1) > w) {
            out.push_back(cur);
            cur.clear();
        }
        cur += (cur.empty() ? "" : " ") + word;
    }
    if (!cur.empty()) out.push_back(cur);
    for (std::string& l : out) l = std::string(indent, ' ') + l;
    return out;
}

// ---------------------------------------------------------------- input

static u64 readKeys() {
    static int repeat = 0;
    padUpdate(&g_pad);
    u64 down = padGetButtonsDown(&g_pad);
    u64 held = padGetButtons(&g_pad) & (HidNpadButton_AnyUp | HidNpadButton_AnyDown);
    if (held) {
        repeat++;
        if (repeat > 24 && repeat % 3 == 0) down |= held;
    } else {
        repeat = 0;
    }
    return down;
}

// Aspetta uno dei tasti indicati; 0 se l'app deve chiudersi
static u64 waitFor(u64 mask) {
    while (appletMainLoop()) {
        u64 k = readKeys() & mask;
        consoleUpdate(NULL);
        if (k) return k;
    }
    g_exit = true;
    return 0;
}

static void messageScreen(const std::string& title, const std::vector<std::string>& lines,
                          const char* color = nullptr) {
    cls();
    line(2, title, C_TITLE);
    int row = 4;
    for (const std::string& l : lines) {
        if (l.empty()) {
            row++;
            continue;
        }
        for (const std::string& w : wrap(l, COLS - 2)) {
            if (row > 42) break;
            line(row++, " " + w, color);
        }
    }
}

static void inform(const std::string& title, const std::vector<std::string>& lines, const char* color = nullptr) {
    messageScreen(title, lines, color);
    line(44, "A: continua", C_TITLE);
    waitFor(HidNpadButton_A | HidNpadButton_B | HidNpadButton_Plus);
}

static void fatal(const std::string& title, const std::vector<std::string>& lines) {
    messageScreen(title, lines, C_ERR);
    line(44, "+: esci", C_TITLE);
    waitFor(HidNpadButton_Plus);
}

// Un gioco e' aperto in background? (solo se l'app gira in modalita' applet)
static bool gameRunning() {
    AppletType t = appletGetAppletType();
    if (t == AppletType_Application || t == AppletType_SystemApplication) return false;
    if (!g_pmOk) return false;
    u64 pid = 0;
    return R_SUCCEEDED(pmdmntGetApplicationProcessId(&pid));
}

// ---------------------------------------------------------------- stato app

struct App {
    Config cfg;
    Manager* mgr = nullptr;
    size_t source = 0;
    std::vector<RemoteFile> remote;
    bool remoteOk = false;
    std::string remoteErr;
    int cursor = 0;
    int scroll = 0;
    bool game = false;         // un gioco e' aperto in background
    bool patch = false;        // patch dell'avvio diretto presente sulla SD
    std::string directLevel;   // livello in debug.xml, vuoto se l'avvio diretto e' spento
    std::string status;
    const char* statusColor = nullptr;
};

static void refreshLocal(App& a) {
    a.game = gameRunning();
    a.patch = a.mgr->directLaunchPatchInstalled();
    a.directLevel = a.mgr->directLaunchLevel();
}

static void loadRemote(App& a) {
    const SourceConfig& s = a.cfg.sources[a.source];
    cls();
    line(2, "Caricamento elenco da \"" + s.name + "\"...", C_TITLE);
    consoleUpdate(NULL);
    a.cursor = a.scroll = 0;
    std::string err;
    a.remoteOk = listSource(s, a.remote, err);
    a.remoteErr = err;
    if (!a.remoteOk) a.remote.clear();
    a.status = !a.remoteOk ? "Errore nel caricamento"
               : a.remote.size() == 1 ? "1 file .pak disponibile"
                                      : std::to_string(a.remote.size()) + " file .pak disponibili";
    a.statusColor = a.remoteOk ? C_OK : C_ERR;
    refreshLocal(a);
}

static void draw(App& a) {
    cls();
    const SourceConfig& s = a.cfg.sources[a.source];
    line(1, " NST Pak Manager " APP_VERSION_STR "    gioco " + a.cfg.titleId, C_TITLE);
    line(2, " Sorgente: " + s.name + " (" + s.type + ")" + (a.cfg.sources.size() > 1 ? "    [-] cambia" : ""));
    const std::string game = a.mgr->game();
    const std::string label = "< " + gameLabel(game) + " >";
    const std::string mode = game == "auto" ? " (dal livello)" : " (a mano)";
    const std::string direct = "  debug.xml: " + (a.directLevel.empty() ? "spento" : a.directLevel);
    const size_t used = 8 + label.size() + mode.size();
    printf("\x1b[3;1H Gioco: %s%s%s%s%s\x1b[K", C_OK, label.c_str(), C_RESET, mode.c_str(),
           fit(direct, COLS > (int)used ? COLS - used : 0).c_str());
    line(4, std::string(COLS, '-'), C_DIM);

    const int n = (int)a.remote.size();
    if (a.cursor < a.scroll) a.scroll = a.cursor;
    if (a.cursor >= a.scroll + LIST_ROWS) a.scroll = a.cursor - LIST_ROWS + 1;
    if (!a.remoteOk) {
        int row = LIST_TOP;
        line(row++, "Impossibile leggere la sorgente:", C_ERR);
        for (const std::string& w : wrap(a.remoteErr, COLS - 2)) line(row++, " " + w, C_ERR);
        line(row + 1, "ZR: riprova", C_DIM);
    } else if (n == 0) {
        line(LIST_TOP, "Nessun file .pak nella sorgente.", C_DIM);
    }
    for (int i = 0; i < LIST_ROWS && a.scroll + i < n; i++) {
        const RemoteFile& f = a.remote[a.scroll + i];
        std::string size = f.sizeKnown ? util::formatSize(f.size) : "?";
        drawRow(LIST_TOP + i, "  " + fit(f.name, 64) + rfit(size, 11), a.scroll + i == a.cursor, nullptr);
    }
    if (n > LIST_ROWS)
        line(LIST_TOP + LIST_ROWS, rfit(std::to_string(a.cursor + 1) + "/" + std::to_string(n) + " ", COLS), C_DIM);

    line(41, std::string(COLS, '-'), C_DIM);
    line(42, " " + a.status, a.statusColor);
    std::string info = " A scarica il livello, crea update.pak e debug.xml e avvia il gioco";
    const char* infoColor = C_DIM;
    if (!a.patch) {
        info = " Manca la patch per l'avvio diretto: il gioco si aprira' dal menu (README)";
        infoColor = C_WARN;
    }
    if (a.game) {
        info = " ATTENZIONE: un gioco e' aperto. Chiudilo prima di giocare un livello.";
        infoColor = C_WARN;
    }
    line(43, info, infoColor);
    line(44, " A gioca   L/R gioco (Auto, Crash 1, 2, 3)   ZR aggiorna l'elenco", C_TITLE);
    printf("\x1b[45;1H%s - sorgente   ZL gioco normale (toglie debug.xml)   + esci%s\x1b[K", C_TITLE, C_RESET);
}

// ---------------------------------------------------------------- azioni

static void chooseSource(App& a) {
    if (a.cfg.sources.size() < 2) return;
    int cur = (int)a.source;
    const int n = (int)a.cfg.sources.size();
    while (true) {
        cls();
        line(2, "Scegli la sorgente", C_TITLE);
        for (int i = 0; i < n && i < 36; i++) {
            const SourceConfig& s = a.cfg.sources[i];
            // per MEGA solo la cartella: la chiave del link non si mostra
            std::string where = s.type == "http" ? s.url : s.url.substr(0, s.url.find('#'));
            drawRow(i + 4, "  " + fit(s.name, 30) + " " + fit(s.type, 4) + "  " + where, i == cur, nullptr);
        }
        line(44, "A: scegli     B: annulla", C_TITLE);
        u64 k = waitFor(HidNpadButton_A | HidNpadButton_B | HidNpadButton_AnyUp | HidNpadButton_AnyDown);
        if (!k || (k & HidNpadButton_B)) return;
        if (k & HidNpadButton_AnyUp) cur = (cur + n - 1) % n;
        if (k & HidNpadButton_AnyDown) cur = (cur + 1) % n;
        if (k & HidNpadButton_A) {
            a.source = (size_t)cur;
            loadRemote(a);
            return;
        }
    }
}

static void changeGame(App& a, int step) {
    std::string err;
    if (!a.mgr->setGame(nextGame(a.mgr->game(), step), err)) {
        a.status = err;
        a.statusColor = C_WARN;
        return;
    }
    a.status = "Gioco per l'avvio diretto: " + gameLabel(a.mgr->game()) +
               (a.mgr->game() == "auto" ? " (quello del livello)" : "");
    a.statusColor = nullptr;
}

// Toglie gli argomenti di avvio impostati per il gioco: si riapre normalmente
static void clearLaunchArguments() {
    if (g_ldrOk) ldrShellFlushArguments();
}

// Spegne l'avvio diretto: il gioco torna ad aprirsi dal menu
static void clearDirectLaunch(App& a) {
    clearLaunchArguments();
    a.mgr->clearDirectLaunch();
    a.directLevel.clear();
}

// Chiude l'app e avvia il gioco. keepDirect: lascia l'avvio diretto (altrimenti parte dal menu)
static void launchGame(App& a, bool keepDirect) {
    if (!keepDirect) clearDirectLaunch(a);
    if (gameRunning()) {
        inform("Gioco gia' aperto",
               {"Un gioco e' aperto in background: chiudilo dal menu HOME e poi avvialo,",
                "altrimenti continua a usare i file di prima."},
               C_WARN);
        return;
    }
    u64 tid = strtoull(a.cfg.titleId.c_str(), nullptr, 16);
    Result rc = appletRequestLaunchApplication(tid, NULL);
    if (R_FAILED(rc)) {
        char code[16];
        snprintf(code, sizeof(code), "0x%X", (unsigned)rc);
        inform("Avvio non riuscito",
               {std::string("Il sistema ha rifiutato l'avvio del gioco (errore ") + code + ").",
                "Avvialo dal menu HOME: il livello e debug.xml sono gia' al loro posto."},
               C_WARN);
        return;
    }
    g_exit = true;  // l'app deve chiudersi perche' il gioco parta
}

static void drawProgress(const std::string& label, uint64_t done, uint64_t total, double seconds) {
    line(4, "Download: " + label);
    const int barWidth = 60;
    int filled = total ? (int)(barWidth * (double)done / (double)total) : 0;
    if (filled > barWidth) filled = barWidth;
    char pct[16] = "";
    if (total) snprintf(pct, sizeof(pct), " %3d%%", (int)(100.0 * (double)done / (double)total));
    line(6, "[" + std::string(filled, '#') + std::string(barWidth - filled, '.') + "]" + pct, C_OK);
    std::string amount = util::formatSize(done) + (total ? " / " + util::formatSize(total) : "");
    if (seconds > 0.5) amount += "   " + util::formatSize((uint64_t)(done / seconds)) + "/s";
    line(7, amount);
    line(9, "B: annulla", C_DIM);
}

// Scarica il livello sotto il cursore (sempre), lo prepara e avvia il gioco direttamente dentro
static void play(App& a) {
    if (a.remote.empty()) return;
    const RemoteFile f = a.remote[a.cursor];
    if (gameRunning()) {
        inform("Gioco gia' aperto", {"Un gioco e' aperto in background: chiudilo dal menu HOME e riprova."}, C_WARN);
        return;
    }
    cls();
    line(2, "Preparazione di " + f.name, C_TITLE);
    u64 lastDraw = 0;
    const u64 start = armGetSystemTick();
    const u64 freq = armGetSystemTickFreq();
    bool cancelled = false;
    net::Progress cb = [&](uint64_t done, uint64_t total) {
        u64 now = armGetSystemTick();
        if (!appletMainLoop()) {
            g_exit = true;
            cancelled = true;
            return false;
        }
        padUpdate(&g_pad);
        if (padGetButtonsDown(&g_pad) & HidNpadButton_B) {
            cancelled = true;
            return false;
        }
        if (now - lastDraw > freq / 8) {
            lastDraw = now;
            drawProgress(f.name, done, total, (double)(now - start) / (double)freq);
            consoleUpdate(NULL);
        }
        return true;
    };
    drawProgress(f.name, 0, f.size, 0);
    consoleUpdate(NULL);

    appletSetAutoSleepDisabled(true);
    appletSetMediaPlaybackState(true);
    Prepared p;
    std::string err;
    bool ok = a.mgr->prepare(f, cb, p, err);
    appletSetMediaPlaybackState(false);
    appletSetAutoSleepDisabled(false);
    if (g_exit) return;
    refreshLocal(a);
    if (!ok) {
        a.status = cancelled ? "Download annullato" : "Livello non pronto: " + f.name;
        a.statusColor = C_WARN;
        if (!cancelled) inform("Livello non pronto", {f.name + ": " + err, "", "Il gioco non e' stato avviato."}, C_WARN);
        return;
    }

    std::vector<std::string> lines;
    lines.push_back("Livello: " + p.levelId + "  ->  " + p.pakName);
    if (!p.removed.empty() && p.removed != p.pakName) lines.push_back("Tolto il livello di prima: " + p.removed);
    if (p.registered) lines.push_back("update.pak: creato con la registrazione del livello");
    else if (p.updateRemoved) lines.push_back("update.pak: tolto (questo livello non lo usa)");
    else lines.push_back("update.pak: non serve");
    lines.push_back("debug.xml: " + p.launchId);
    if (p.launchId != p.levelId)
        lines.push_back("ATTENZIONE: " + gameLabel(a.mgr->game()) + " scelto a mano, ma il livello e' in " +
                        p.levelId.substr(0, p.levelId.find('/')) + ": il gioco potrebbe non trovarlo.");
    if (!a.patch)
        lines.push_back("ATTENZIONE: patch per l'avvio diretto non trovata, il gioco si aprira' dal menu.");
    lines.push_back("");
    lines.push_back("Avvio del gioco...");
    messageScreen("Livello pronto", lines, C_OK);
    consoleUpdate(NULL);

    if (g_ldrOk && !a.cfg.launchArgs.empty()) {
        std::string args = launchArguments(a.cfg.launchArgs, p.launchId);
        ldrShellSetProgramArguments(strtoull(a.cfg.titleId.c_str(), nullptr, 16), args.c_str(), args.size() + 1);
    }
    a.status = "Pronto: " + p.launchId;
    a.statusColor = C_OK;
    launchGame(a, true);
}

// ---------------------------------------------------------------- avvio

static bool loadConfig(Config& cfg) {
    const std::string dir = appDirFor(ROOT);
    const std::string path = dir + "/config.json";
    if (!util::fileExists(path)) {
        util::mkdirs(dir);
        bool written = util::writeFileAtomic(path, defaultConfigJson());
        fatal("Configurazione mancante",
              {written ? "Ho creato un file di esempio:" : "Non riesco a creare il file di configurazione:",
               "  " + path, "",
               "Aprilo dal PC (o via FTP) e inserisci le tue sorgenti:",
               "- \"http\": indirizzo di una cartella web o di un manifest .json",
               "- \"mega\": link di una cartella MEGA condivisa (con la chiave dopo #)", "",
               "Poi riavvia l'app."});
        return false;
    }
    std::string text, err;
    if (!util::readFile(path, text) || !parseConfig(text, cfg, err)) {
        fatal("Errore in config.json", {path, "", err.empty() ? "file illeggibile" : err});
        return false;
    }
    std::string ca = cfg.caFile.empty() ? dir + "/cacert.pem"
                     : (cfg.caFile.find(':') == std::string::npos && cfg.caFile[0] != '/')
                         ? dir + "/" + cfg.caFile
                         : cfg.caFile;
    if (ca[0] == '/') ca = std::string(ROOT) + ca;
    net::setCaFile(ca);
    return true;
}

static void run() {
    App a;
    if (!loadConfig(a.cfg)) return;

    Manager mgr(ROOT, a.cfg);
    std::string err;
    if (!mgr.load(err)) {
        fatal("Errore", {err});
        return;
    }
    mgr.cleanupPartials();
    a.mgr = &mgr;

    loadRemote(a);
    bool dirty = true;
    u64 lastCheck = armGetSystemTick();
    const u64 freq = armGetSystemTickFreq();
    while (!g_exit && appletMainLoop()) {
        u64 k = readKeys();
        if (k & HidNpadButton_Plus) break;
        if (k) dirty = true;
        // Controlla ogni 2 secondi se e' stato aperto o chiuso un gioco
        if (armGetSystemTick() - lastCheck > 2 * freq) {
            lastCheck = armGetSystemTick();
            bool game = gameRunning();
            if (game != a.game) {
                a.game = game;
                dirty = true;
            }
        }
        const int n = (int)a.remote.size();
        if (k & HidNpadButton_AnyUp) a.cursor = n ? (a.cursor + n - 1) % n : 0;
        if (k & HidNpadButton_AnyDown) a.cursor = n ? (a.cursor + 1) % n : 0;
        if (k & HidNpadButton_AnyLeft) a.cursor = std::max(0, a.cursor - LIST_ROWS);
        if (k & HidNpadButton_AnyRight) a.cursor = std::max(0, std::min(n - 1, a.cursor + LIST_ROWS));
        if (k & HidNpadButton_L) changeGame(a, -1);
        if (k & HidNpadButton_R) changeGame(a, 1);
        if (k & HidNpadButton_A) play(a);
        if (k & HidNpadButton_ZR) loadRemote(a);
        if (k & HidNpadButton_Minus) chooseSource(a);
        if (k & HidNpadButton_ZL) launchGame(a, false);
        if (g_exit) break;
        if (dirty) {
            draw(a);
            dirty = false;
        }
        consoleUpdate(NULL);
    }
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    consoleInit(NULL);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&g_pad);

    bool socketOk = R_SUCCEEDED(socketInitializeDefault());
    g_pmOk = R_SUCCEEDED(pmdmntInitialize());
    g_ldrOk = R_SUCCEEDED(ldrShellInitialize());
    clearLaunchArguments();  // eventuali argomenti rimasti da un avvio diretto precedente
    if (!socketOk) {
        fatal("Rete non disponibile", {"Impossibile inizializzare la rete (socketInitializeDefault)."});
    } else if (!net::init()) {
        fatal("Errore", {"Impossibile inizializzare libcurl."});
    } else {
        run();
        net::shutdown();
    }

    if (g_ldrOk) ldrShellExit();
    if (g_pmOk) pmdmntExit();
    if (socketOk) socketExit();
    consoleExit(NULL);
    return 0;
}
