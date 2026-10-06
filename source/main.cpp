// main.cpp - interfaccia console per Nintendo Switch (libnx)
//
// Testi solo ASCII: il font della console libnx non ha le lettere accentate.

#include <switch.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "core.hpp"
#include "net.hpp"
#include "util.hpp"

#define APP_VERSION_STR "1.8.0"

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

static std::vector<std::string> wrapWords(const std::string& text, size_t width);

// Va a capo rispettando il rientro iniziale della riga
static std::vector<std::string> wrap(const std::string& text, size_t width) {
    size_t indent = text.find_first_not_of(' ');
    if (indent == std::string::npos) return {};
    if (indent > width / 2) indent = 0;
    std::vector<std::string> out = wrapWords(text.substr(indent), width - indent);
    for (std::string& l : out) l = std::string(indent, ' ') + l;
    return out;
}

static std::vector<std::string> wrapWords(const std::string& text, size_t width) {
    std::vector<std::string> out;
    std::string cur;
    size_t i = 0;
    while (i < text.size()) {
        size_t sp = text.find(' ', i);
        std::string word = text.substr(i, sp == std::string::npos ? std::string::npos : sp - i);
        i = sp == std::string::npos ? text.size() : sp + 1;
        while (word.size() > width) {
            if (!cur.empty()) { out.push_back(cur); cur.clear(); }
            out.push_back(word.substr(0, width));
            word = word.substr(width);
        }
        if (cur.size() + word.size() + (cur.empty() ? 0 : 1) > width) {
            out.push_back(cur);
            cur.clear();
        }
        cur += (cur.empty() ? "" : " ") + word;
    }
    if (!cur.empty()) out.push_back(cur);
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

static bool confirm(const std::string& title, const std::vector<std::string>& lines) {
    messageScreen(title, lines);
    line(44, "A: conferma     B: annulla", C_TITLE);
    return waitFor(HidNpadButton_A | HidNpadButton_B) == HidNpadButton_A;
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

// ---------------------------------------------------------------- tastiera

// Tastiera di sistema; false se annullata o non disponibile
static bool keyboard(const std::string& header, const std::string& initial, std::string& out) {
    SwkbdConfig kbd;
    if (R_FAILED(swkbdCreate(&kbd, 0))) return false;
    swkbdConfigMakePresetDefault(&kbd);
    swkbdConfigSetHeaderText(&kbd, header.c_str());
    swkbdConfigSetGuideText(&kbd, header.c_str());
    swkbdConfigSetInitialText(&kbd, initial.c_str());
    char buf[256] = {0};
    Result rc = swkbdShow(&kbd, buf, sizeof(buf));
    swkbdClose(&kbd);
    if (R_FAILED(rc)) return false;
    out = buf;
    return true;
}

static bool sameName(const std::string& a, const std::string& b) { return util::toLower(a) == util::toLower(b); }

static std::string arrowLabel(const std::string& remote, const std::string& target) {
    if (target.empty()) return remote;  // nome automatico (dal contenuto del file)
    return sameName(remote, target) ? remote : remote + " -> " + target;
}

// ---------------------------------------------------------------- stato app

struct App {
    Config cfg;
    Manager* mgr = nullptr;
    size_t source = 0;
    std::vector<RemoteFile> remote;
    bool remoteOk = false;
    std::string remoteErr;
    std::vector<std::string> originals;  // .pak originali del gioco (originali.txt)
    bool originalsFound = false;
    std::vector<InstalledFile> installed;  // ordinati per nome
    std::vector<std::string> external;
    std::set<std::string> present;  // file nella cartella mod (letti una volta, non a ogni frame)
    bool game = false;              // un gioco e' aperto in background
    int tab = 0;                    // 0 = remoti, 1 = installati
    int cursor[2] = {0, 0};
    int scroll[2] = {0, 0};
    std::map<std::string, std::string> pick;  // file remoto scelto -> originale da sostituire
    std::set<std::string> sel;                // selezione nella scheda installati
    std::string status;
    const char* statusColor = nullptr;
    std::string directLevel;  // livello dell'avvio diretto attivo (debug.xml), vuoto se spento
};

static std::string originalsPath() { return appDirFor(ROOT) + "/originali.txt"; }

static void refreshLocal(App& a) {
    a.installed = a.mgr->installed();
    std::sort(a.installed.begin(), a.installed.end(), [](const InstalledFile& x, const InstalledFile& y) {
        return util::toLower(x.name) < util::toLower(y.name);
    });
    a.external = a.mgr->externalFiles();
    a.present.clear();
    for (const std::string& f : util::listFiles(a.mgr->modDir())) a.present.insert(util::toLower(f));
    a.game = gameRunning();
    a.directLevel = a.mgr->directLaunchLevel();
    for (int t = 0; t < 2; t++) {
        int n = t == 0 ? (int)a.remote.size() : (int)a.installed.size();
        if (a.cursor[t] >= n) a.cursor[t] = n > 0 ? n - 1 : 0;
    }
    std::set<std::string> keep;
    for (const InstalledFile& f : a.installed)
        if (a.sel.count(f.name)) keep.insert(f.name);
    a.sel = keep;
}

static void loadRemote(App& a) {
    const SourceConfig& s = a.cfg.sources[a.source];
    cls();
    line(2, "Caricamento elenco da \"" + s.name + "\"...", C_TITLE);
    consoleUpdate(NULL);
    a.originalsFound = loadOriginals(originalsPath(), a.originals);
    if (!a.originalsFound) a.originals.clear();
    a.mgr->setOriginals(a.originals);
    a.pick.clear();
    a.cursor[0] = a.scroll[0] = 0;
    std::string err;
    a.remoteOk = listSource(s, a.remote, err);
    a.remoteErr = err;
    if (!a.remoteOk) a.remote.clear();
    a.status = a.remoteOk ? std::to_string(a.remote.size()) + " file .pak disponibili" : "Errore nel caricamento";
    a.statusColor = a.remoteOk ? C_OK : C_ERR;
    refreshLocal(a);
}

static void drawRow(int row, const std::string& text, bool highlighted, const char* color) {
    if (highlighted)
        printf("\x1b[%d;1H%s%s%s\x1b[K", row, C_REV, fit(text, COLS).c_str(), C_RESET);
    else
        line(row, text, color);
}

static void draw(App& a) {
    cls();
    const SourceConfig& s = a.cfg.sources[a.source];
    line(1, " NST Pak Manager " APP_VERSION_STR "    gioco " + a.cfg.titleId, C_TITLE);
    line(2, " Sorgente: " + s.name + " (" + s.type + ")" +
                (a.cfg.sources.size() > 1 ? "    [-] cambia" : ""));
    std::string t0 = "REMOTI (" + std::to_string(a.remote.size()) + ")";
    std::string t1 = "INSTALLATI (" + std::to_string(a.installed.size()) + ")";
    std::string direct;
    if (!a.directLevel.empty())
        direct = "avvio diretto: " + a.directLevel.substr(a.directLevel.rfind('/') + 1);
    printf("\x1b[3;2H%s %s %s   %s %s %s   %s%s%s\x1b[K", a.tab == 0 ? C_REV : C_DIM, t0.c_str(), C_RESET,
           a.tab == 1 ? C_REV : C_DIM, t1.c_str(), C_RESET, C_OK, fit(direct, 44).c_str(), C_RESET);
    line(4, std::string(COLS, '-'), C_DIM);

    int n = a.tab == 0 ? (int)a.remote.size() : (int)a.installed.size();
    int& cur = a.cursor[a.tab];
    int& top = a.scroll[a.tab];
    if (cur < top) top = cur;
    if (cur >= top + LIST_ROWS) top = cur - LIST_ROWS + 1;

    if (a.tab == 0 && !a.remoteOk) {
        int row = LIST_TOP;
        line(row++, "Impossibile leggere la sorgente:", C_ERR);
        for (const std::string& w : wrap(a.remoteErr, COLS - 2)) line(row++, " " + w, C_ERR);
        line(row + 1, "ZR: riprova", C_DIM);
    } else if (n == 0) {
        line(LIST_TOP, a.tab == 0 ? "Nessun file .pak nella sorgente." : "Nessun file installato da questa app.",
             C_DIM);
    }

    for (int i = 0; i < LIST_ROWS && top + i < n; i++) {
        int idx = top + i;
        std::string text;
        const char* color = nullptr;
        if (a.tab == 0) {
            const RemoteFile& f = a.remote[idx];
            std::vector<const InstalledFile*> from = a.mgr->installedFrom(f.name);
            auto p = a.pick.find(f.name);
            std::string right;
            if (p != a.pick.end())
                right = p->second.empty() ? " -> nome automatico" : " -> " + p->second;
            else if (!from.empty())
                right = " (= " + from[0]->name + (from.size() > 1 ? " +" + std::to_string(from.size() - 1) : "") + ")";
            std::string size = f.sizeKnown ? util::formatSize(f.size) : "?";
            text = std::string(p != a.pick.end() ? "[x] " : "[ ] ") + (from.empty() ? "  " : "I ") + fit(f.name, 30) +
                   fit(right, 32) + rfit(size, 11);
            color = p != a.pick.end() ? C_TITLE : from.empty() ? nullptr : C_OK;
        } else {
            const InstalledFile& f = a.installed[idx];
            std::string from = sameName(f.remote, f.name) ? fit("", 27) : " <- " + fit(f.remote, 23);
            text = std::string(a.sel.count(f.name) ? "[x] " : "[ ] ") + fit(f.name, 29) + from +
                   rfit(util::formatSize(f.size), 10) + "  " + fit(f.backup.empty() ? "nessuno" : "backup", 7);
        }
        drawRow(LIST_TOP + i, text, idx == cur, color);
    }
    if (n > LIST_ROWS)
        line(LIST_TOP + LIST_ROWS, rfit(std::to_string(cur + 1) + "/" + std::to_string(n) + " ", COLS), C_DIM);

    line(41, std::string(COLS, '-'), C_DIM);
    line(42, " " + a.status, a.statusColor);
    std::string info;
    const char* infoColor = C_DIM;
    if (a.tab == 0) {
        info = " Il nome di installazione viene dal file   I = gia' installato (= con che nome)";
        if (!a.originalsFound) {
            info = " originali.txt non trovato: vedi README (tools/list_originals.py)";
            infoColor = C_WARN;
        }
    } else if (!a.external.empty()) {
        info = " Altri .pak nella cartella mod, non gestiti: " + std::to_string(a.external.size());
    }
    if (a.game) {
        info = " ATTENZIONE: un gioco e' aperto. Chiudilo prima di installare.";
        infoColor = C_WARN;
    }
    line(43, info, infoColor);
    if (a.tab == 0)
        line(44, " A seleziona  X installa  Y gioca  B scegli originale  ZR aggiorna", C_TITLE);
    else
        line(44, " A seleziona  Y tutti/nessuno  X ripristina originale  ZR entra nel livello", C_TITLE);
    printf("\x1b[45;1H%s L/R scheda  Sinistra/Destra pagina  - sorgente  ZL avvia dal menu  + esci%s\x1b[K", C_TITLE, C_RESET);
}

// ---------------------------------------------------------------- scelta dell'originale

struct PickItem {
    int kind;  // 0 = scrivi a mano, 1 = stesso nome, 2 = suggerito fuori elenco, 3 = originale
    std::string name;
};

// Chiede quale file originale sostituire con il file remoto f
static bool pickTarget(App& a, const RemoteFile& f, std::string& out) {
    const std::string suggestion = a.mgr->suggestTarget(f, a.originals);
    auto prev = a.pick.find(f.name);
    const std::string current = prev != a.pick.end() ? prev->second : suggestion;
    std::string filter;
    int cur = -1, top = 0;
    bool toFirstOriginal = false;
    while (true) {
        std::vector<PickItem> items;
        items.push_back({0, ""});
        items.push_back({1, f.name});
        bool inList = false;
        for (const std::string& o : a.originals) inList = inList || sameName(o, current);
        if (!current.empty() && !inList && !sameName(current, f.name)) items.push_back({2, current});
        for (const std::string& o : a.originals)
            if (filter.empty() || util::toLower(o).find(util::toLower(filter)) != std::string::npos)
                items.push_back({3, o});
        const int n = (int)items.size();
        if (toFirstOriginal) {
            cur = 1;
            for (int i = 0; i < n; i++)
                if (items[i].kind == 3) {
                    cur = i;
                    break;
                }
            toFirstOriginal = false;
        }
        if (cur < 0 || cur >= n) {
            cur = 1;
            int firstOriginal = -1;
            for (int i = 0; i < n; i++) {
                if (items[i].kind == 3 && firstOriginal < 0) firstOriginal = i;
                if (items[i].kind >= 2 && !current.empty() && sameName(items[i].name, current)) cur = i;
            }
            if (current.empty() && firstOriginal >= 0) cur = firstOriginal;
        }
        if (cur < top) top = cur;
        if (cur >= top + LIST_ROWS) top = cur - LIST_ROWS + 1;

        cls();
        line(1, " Quale file originale vuoi sostituire?", C_TITLE);
        line(2, " Scaricato: " + f.name + (f.sizeKnown ? "  (" + util::formatSize(f.size) + ")" : ""));
        if (!filter.empty())
            line(3, " Filtro: \"" + filter + "\"   X: togli il filtro", C_WARN);
        else if (a.originalsFound)
            line(3, " " + std::to_string(a.originals.size()) + " originali in originali.txt", C_DIM);
        else
            line(3, " originali.txt non trovato: crealo con tools/list_originals.py (vedi README)", C_WARN);
        line(4, std::string(COLS, '-'), C_DIM);
        for (int i = 0; i < LIST_ROWS && top + i < n; i++) {
            const PickItem& it = items[top + i];
            std::string text;
            const char* color = nullptr;
            if (it.kind == 0) {
                text = "  [ Scrivi il nome a mano... ]";
                color = C_DIM;
            } else if (it.kind == 1) {
                text = "  [ Stesso nome: " + it.name + " ]";
                color = C_DIM;
            } else {
                const InstalledFile* mine = a.mgr->find(it.name);
                std::string state;
                if (mine)
                    state = "ora: " + mine->remote;
                else if (a.present.count(util::toLower(it.name)))
                    state = "mod esterna (va nel backup)";
                text = std::string(sameName(it.name, suggestion) ? "* " : "  ") + fit(it.name, 40) + "  " + state;
                if (it.kind == 2) text += "  (non in elenco)";
                color = mine ? C_OK : state.empty() ? nullptr : C_WARN;
            }
            drawRow(LIST_TOP + i, text, top + i == cur, color);
        }
        if (n > LIST_ROWS)
            line(LIST_TOP + LIST_ROWS, rfit(std::to_string(cur + 1) + "/" + std::to_string(n) + " ", COLS), C_DIM);
        line(41, std::string(COLS, '-'), C_DIM);
        line(43, " * = suggerito   ora: = gia' sostituito da questa app", C_DIM);
        line(44, " A scegli  B annulla  Y cerca  X togli filtro", C_TITLE);
        printf("\x1b[45;1H%s Su/Giu scorri  Sinistra/Destra pagina%s\x1b[K", C_TITLE, C_RESET);

        u64 k = waitFor(HidNpadButton_A | HidNpadButton_B | HidNpadButton_X | HidNpadButton_Y |
                        HidNpadButton_AnyUp | HidNpadButton_AnyDown | HidNpadButton_AnyLeft |
                        HidNpadButton_AnyRight);
        if (!k || (k & HidNpadButton_B)) return false;
        if (k & HidNpadButton_AnyUp) cur = (cur + n - 1) % n;
        if (k & HidNpadButton_AnyDown) cur = (cur + 1) % n;
        if (k & HidNpadButton_AnyLeft) cur = std::max(0, cur - LIST_ROWS);
        if (k & HidNpadButton_AnyRight) cur = std::min(n - 1, cur + LIST_ROWS);
        if (k & HidNpadButton_X && !filter.empty()) {
            filter.clear();
            cur = -1;
        }
        if (k & HidNpadButton_Y) {
            std::string typed;
            if (keyboard("Cerca tra gli originali", filter, typed)) {
                filter = typed;
                toFirstOriginal = true;
            }
        }
        if (k & HidNpadButton_A) {
            const PickItem& it = items[cur];
            if (it.kind != 0) {
                out = it.name;
                return true;
            }
            std::string typed;
            if (!keyboard("Nome del file originale (.pak)", current.empty() ? f.name : current, typed)) continue;
            while (!typed.empty() && typed.back() == ' ') typed.pop_back();
            while (!typed.empty() && typed.front() == ' ') typed.erase(0, 1);
            if (!typed.empty() && !util::endsWithCI(typed, ".pak")) typed += ".pak";
            if (!isValidPakName(typed)) {
                inform("Nome non valido", {"\"" + typed + "\" non e' un nome di file .pak valido."}, C_ERR);
                continue;
            }
            for (const std::string& o : a.originals)
                if (sameName(o, typed)) typed = o;
            out = typed;
            return true;
        }
    }
}

// Assegna l'originale al file remoto; se era gia' assegnato a un altro file, lo toglie a quello
static void assignPick(App& a, const std::string& remoteName, const std::string& target) {
    a.status = "Scelto: " + arrowLabel(remoteName, target);
    a.statusColor = nullptr;
    for (auto it = a.pick.begin(); it != a.pick.end();) {
        if (it->first != remoteName && sameName(it->second, target)) {
            a.status = target + " era assegnato a " + it->first + ": ora a " + remoteName;
            a.statusColor = C_WARN;
            it = a.pick.erase(it);
        } else {
            ++it;
        }
    }
    a.pick[remoteName] = target;
}

// ---------------------------------------------------------------- azioni

static void chooseSource(App& a) {
    if (a.cfg.sources.size() < 2) return;
    int cur = (int)a.source;
    while (true) {
        cls();
        line(2, "Scegli la sorgente", C_TITLE);
        for (size_t i = 0; i < a.cfg.sources.size() && i < 36; i++) {
            const SourceConfig& s = a.cfg.sources[i];
            // per MEGA solo la cartella: la chiave del link non si mostra
            std::string where = s.type == "http" ? s.url : s.url.substr(0, s.url.find('#'));
            std::string text = "  " + fit(s.name, 30) + " " + fit(s.type, 4) + "  " + where;
            drawRow((int)i + 4, text, (int)i == cur, nullptr);
        }
        line(44, "A: scegli     B: annulla", C_TITLE);
        u64 k = waitFor(HidNpadButton_A | HidNpadButton_B | HidNpadButton_AnyUp | HidNpadButton_AnyDown);
        if (!k || (k & HidNpadButton_B)) return;
        if (k & HidNpadButton_AnyUp) cur = (cur + (int)a.cfg.sources.size() - 1) % (int)a.cfg.sources.size();
        if (k & HidNpadButton_AnyDown) cur = (cur + 1) % (int)a.cfg.sources.size();
        if (k & HidNpadButton_A) {
            a.source = (size_t)cur;
            loadRemote(a);
            return;
        }
    }
}

struct ProgressState {
    u64 lastDraw = 0;
    u64 start = 0;
    bool cancelled = false;
};

static void drawProgress(int index, int count, const std::string& label, uint64_t done, uint64_t total,
                         double seconds) {
    line(4, "File " + std::to_string(index) + " di " + std::to_string(count) + ": " + label);
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

// Chiude l'app e avvia il gioco, cosi' carica i file appena installati.
// keepDirect: lascia l'avvio diretto preparato da launchLevel (altrimenti il gioco parte dal menu)
static void launchGame(App& a, bool keepDirect = false) {
    if (!keepDirect) clearDirectLaunch(a);
    if (gameRunning()) {
        clearDirectLaunch(a);
        inform("Gioco gia' aperto",
               {"Un gioco e' aperto in background: chiudilo dal menu HOME e poi avvialo,",
                "altrimenti continua a usare i file di prima."},
               C_WARN);
        return;
    }
    u64 tid = strtoull(a.cfg.titleId.c_str(), nullptr, 16);
    Result rc = appletRequestLaunchApplication(tid, NULL);
    if (R_FAILED(rc)) {
        clearDirectLaunch(a);
        char code[16];
        snprintf(code, sizeof(code), "0x%X", (unsigned)rc);
        inform("Avvio non riuscito",
               {std::string("Il sistema ha rifiutato l'avvio del gioco (errore ") + code + ").",
                "Avvialo dal menu HOME: i file installati restano al loro posto."},
               C_WARN);
        return;
    }
    g_exit = true;  // l'app deve chiudersi perche' il gioco parta
}

// Avvia il gioco direttamente nel livello contenuto in un .pak installato. Il gioco legge
// debug.xml (serve la patch dell'eseguibile, vedi tools/crea_avvio_livello.py); in piu', se
// il servizio ldr:shel di Atmosphere e' disponibile, gli passa anche l'opzione -om.
static void launchLevel(App& a, const std::string& pakName) {
    std::vector<std::string> ids;
    if (!pakLevelIds(a.mgr->modDir() + "/" + pakName, ids) || ids.empty()) {
        inform("Nessun livello", {pakName + " non contiene un livello da avviare."}, C_WARN);
        return;
    }
    if (gameRunning()) {
        inform("Gioco gia' aperto", {"Un gioco e' aperto in background: chiudilo dal menu HOME e riprova."}, C_WARN);
        return;
    }
    const std::string& level = ids[0];
    if (!a.mgr->directLaunchPatchInstalled()) {
        if (!confirm("Patch per l'avvio diretto non trovata",
                     {"Per entrare nel livello il gioco legge il file debug.xml, che la versione in "
                      "commercio ignora senza una piccola patch dell'eseguibile (si installa una volta sola).",
                      "",
                      "Creala sul PC con tools/crea_avvio_livello.py e copia la cartella \"sd\" che crea "
                      "nella radice della SD (la patch finisce in " + std::string(DIRECT_LAUNCH_PATCH_DIR) + ").",
                      "",
                      "Senza la patch il gioco probabilmente si aprira' dal menu. Avviare lo stesso?"}))
            return;
    }
    std::string err;
    // Livello nuovo (convertito con --nuovo): update.pak con la sua registrazione, creato qui
    messageScreen("Preparazione", {"Controllo se " + pakName + " e' un livello nuovo da registrare..."});
    consoleUpdate(NULL);
    bool registered = false;
    if (!a.mgr->registerLevel(pakName, registered, err)) {
        inform("Livello nuovo non registrato", {err, "", "Il gioco non e' stato avviato."}, C_WARN);
        refreshLocal(a);
        return;
    }
    if (registered) refreshLocal(a);
    if (!a.mgr->setDirectLaunch(level, err)) {
        inform("Avvio diretto non riuscito", {err}, C_WARN);
        return;
    }
    a.directLevel = level;
    if (g_ldrOk && !a.cfg.launchArgs.empty()) {
        std::string args = launchArguments(a.cfg.launchArgs, level);
        ldrShellSetProgramArguments(strtoull(a.cfg.titleId.c_str(), nullptr, 16), args.c_str(), args.size() + 1);
    }
    a.status = "Avvio diretto: " + level;
    launchGame(a, true);
}

// Primo .pak (tra quelli indicati) che contiene un livello
static std::string firstLevelPak(App& a, const std::vector<std::string>& names) {
    for (const std::string& name : names) {
        std::vector<std::string> ids;
        if (pakLevelIds(a.mgr->modDir() + "/" + name, ids) && !ids.empty()) return name;
    }
    return "";
}

// Scarica e installa un file mostrando l'avanzamento. target vuoto = nome automatico.
static bool installWithProgress(App& a, const RemoteFile& f, const std::string& target, int index, int count,
                                std::string& installedAs, std::string& err, bool& cancelled) {
    const std::string label = arrowLabel(f.name, target);
    cls();
    line(2, "Installazione in corso", C_TITLE);
    ProgressState ps;
    ps.start = armGetSystemTick();
    const u64 freq = armGetSystemTickFreq();
    net::Progress cb = [&](uint64_t done, uint64_t tot) {
        u64 now = armGetSystemTick();
        if (!appletMainLoop()) {
            g_exit = true;
            ps.cancelled = true;
            return false;
        }
        padUpdate(&g_pad);
        if (padGetButtonsDown(&g_pad) & HidNpadButton_B) {
            ps.cancelled = true;
            return false;
        }
        if (now - ps.lastDraw > freq / 8) {
            ps.lastDraw = now;
            drawProgress(index, count, label, done, tot, (double)(now - ps.start) / (double)freq);
            consoleUpdate(NULL);
        }
        return true;
    };
    drawProgress(index, count, label, 0, f.size, 0);
    consoleUpdate(NULL);
    bool ok = a.mgr->install(f, target, a.cfg.sources[a.source].name, cb, err, &installedAs);
    cancelled = ps.cancelled;
    return ok;
}

static void installSelected(App& a) {
    if (a.pick.empty() && !a.remote.empty()) a.pick[a.remote[a.cursor[0]].name] = "";  // nome automatico
    std::vector<std::pair<RemoteFile, std::string>> todo;
    for (const RemoteFile& f : a.remote) {
        auto p = a.pick.find(f.name);
        if (p != a.pick.end()) todo.push_back({f, p->second});
    }
    if (todo.empty()) return;

    uint64_t total = 0;
    bool allKnown = true;
    int backups = 0, replaces = 0, unknown = 0;
    for (const auto& t : todo) {
        total += t.first.size;
        allKnown = allKnown && t.first.sizeKnown;
        if (t.second.empty()) continue;  // nome automatico: si sa solo dopo il download
        if (a.mgr->find(t.second)) replaces++;
        else if (a.present.count(util::toLower(t.second))) backups++;
        bool known = false;
        for (const std::string& o : a.originals) known = known || sameName(o, t.second);
        if (!known) unknown++;
    }
    std::vector<std::string> lines;
    lines.push_back("Installare " + std::to_string(todo.size()) + " file" +
                    (total ? " (" + util::formatSize(total) + (allKnown ? "" : " o piu'") + ")" : "") + "?");
    lines.push_back("");
    for (size_t i = 0; i < todo.size() && i < 18; i++)
        lines.push_back("  " + arrowLabel(todo[i].first.name, todo[i].second) +
                        (todo[i].second.empty() ? "  (nome dal contenuto del file)" : ""));
    if (todo.size() > 18) lines.push_back("  ... e altri " + std::to_string(todo.size() - 18));
    lines.push_back("");
    if (backups == 1) lines.push_back("1 file gia' presente verra' salvato nel backup.");
    if (backups > 1) lines.push_back(std::to_string(backups) + " file gia' presenti verranno salvati nel backup.");
    if (replaces == 1) lines.push_back("1 originale gia' sostituito verra' aggiornato (il backup resta).");
    if (replaces > 1)
        lines.push_back(std::to_string(replaces) + " originali gia' sostituiti verranno aggiornati (il backup resta).");
    if (unknown == 1 && a.originalsFound)
        lines.push_back("1 nome non e' in originali.txt: il gioco potrebbe ignorarlo.");
    if (unknown > 1 && a.originalsFound)
        lines.push_back(std::to_string(unknown) + " nomi non sono in originali.txt: il gioco potrebbe ignorarli.");
    lines.push_back("Cartella: " + a.mgr->modDir());
    if (gameRunning()) lines.push_back("ATTENZIONE: un gioco e' aperto. Chiudilo prima di continuare.");
    if (!confirm("Conferma installazione", lines)) return;

    appletSetAutoSleepDisabled(true);
    appletSetMediaPlaybackState(true);
    std::vector<std::string> report;
    std::vector<std::string> installedNames;
    int ok = 0;
    bool stop = false;
    for (size_t i = 0; i < todo.size(); i++) {
        const std::string label = arrowLabel(todo[i].first.name, todo[i].second);
        if (stop) {
            report.push_back("SALTATO  " + label);
            continue;
        }
        std::string err, as;
        bool cancelled = false;
        if (installWithProgress(a, todo[i].first, todo[i].second, (int)i + 1, (int)todo.size(), as, err, cancelled)) {
            ok++;
            installedNames.push_back(as);
            report.push_back("OK       " + arrowLabel(todo[i].first.name, as));
        } else {
            report.push_back("ERRORE   " + label + ": " + err);
            if (cancelled) stop = true;
        }
        if (g_exit) break;
    }
    appletSetMediaPlaybackState(false);
    appletSetAutoSleepDisabled(false);
    if (g_exit) return;

    a.pick.clear();
    refreshLocal(a);
    a.status = std::to_string(ok) + " di " + std::to_string(todo.size()) + " file installati";
    a.statusColor = ok == (int)todo.size() ? C_OK : C_WARN;
    messageScreen("Risultato", report, ok == (int)todo.size() ? C_OK : C_WARN);
    if (ok == 0) {
        line(44, "A: continua", C_TITLE);
        waitFor(HidNpadButton_A | HidNpadButton_B | HidNpadButton_Plus);
        return;
    }
    std::string levelPak = firstLevelPak(a, installedNames);
    if (levelPak.empty()) {
        line(44, "A: avvia il gioco     B: torna all'elenco", C_TITLE);
        if (waitFor(HidNpadButton_A | HidNpadButton_B) == HidNpadButton_A) launchGame(a);
        return;
    }
    line(44, "A: avvia il gioco     Y: entra direttamente nel livello     B: torna all'elenco", C_TITLE);
    u64 k = waitFor(HidNpadButton_A | HidNpadButton_B | HidNpadButton_Y);
    if (k == HidNpadButton_A) launchGame(a);
    if (k == HidNpadButton_Y) launchLevel(a, levelPak);
}

// Gioca il file sotto il cursore: lo scarica (se non e' gia' installato uguale), registra il
// livello se e' nuovo e avvia il gioco direttamente dentro
static void playRemote(App& a) {
    if (a.remote.empty()) return;
    const RemoteFile f = a.remote[a.cursor[0]];
    if (gameRunning()) {
        inform("Gioco gia' aperto", {"Un gioco e' aperto in background: chiudilo dal menu HOME e riprova."}, C_WARN);
        return;
    }
    std::string name;
    for (const InstalledFile* i : a.mgr->installedFrom(f.name))
        if (f.sizeKnown && i->size == f.size && a.mgr->existsInModDir(i->name)) name = i->name;
    if (name.empty()) {
        appletSetAutoSleepDisabled(true);
        appletSetMediaPlaybackState(true);
        std::string err;
        bool cancelled = false;
        bool ok = installWithProgress(a, f, "", 1, 1, name, err, cancelled);
        appletSetMediaPlaybackState(false);
        appletSetAutoSleepDisabled(false);
        if (g_exit) return;
        refreshLocal(a);
        if (!ok) {
            inform("Installazione non riuscita", {f.name + ": " + err}, C_WARN);
            return;
        }
    }
    a.status = "Installato " + arrowLabel(f.name, name);
    a.statusColor = C_OK;
    launchLevel(a, name);
}

static void restoreSelected(App& a) {
    std::vector<InstalledFile> todo;
    for (const InstalledFile& f : a.installed)
        if (a.sel.count(f.name)) todo.push_back(f);
    if (todo.empty() && !a.installed.empty()) todo.push_back(a.installed[a.cursor[1]]);
    if (todo.empty()) return;

    std::vector<std::string> lines;
    lines.push_back("Ripristinare " + std::to_string(todo.size()) + " file?");
    lines.push_back("");
    for (size_t i = 0; i < todo.size() && i < 24; i++)
        lines.push_back("  " + todo[i].name +
                        (todo[i].backup.empty() ? "  -> rimosso (torna l'originale del gioco)"
                                                : "  -> rimesso il file salvato"));
    if (todo.size() > 24) lines.push_back("  ... e altri " + std::to_string(todo.size() - 24));
    if (gameRunning()) {
        lines.push_back("");
        lines.push_back("ATTENZIONE: un gioco e' aperto. Chiudilo prima di continuare.");
    }
    if (!confirm("Conferma ripristino", lines)) return;

    std::vector<std::string> report;
    int ok = 0;
    for (const InstalledFile& f : todo) {
        std::string err, note;
        std::vector<std::string> ids;
        bool direct = !a.directLevel.empty() && pakLevelIds(a.mgr->modDir() + "/" + f.name, ids) &&
                      std::find(ids.begin(), ids.end(), a.directLevel) != ids.end();
        if (a.mgr->restore(f.name, err, note)) {
            ok++;
            if (direct) {
                clearDirectLaunch(a);
                note += std::string(note.empty() ? "" : "; ") + "avvio diretto spento";
            }
            report.push_back("OK       " + f.name + (note.empty() ? "" : "  (" + note + ")"));
        } else {
            report.push_back("ERRORE   " + f.name + ": " + err);
        }
    }
    a.sel.clear();
    refreshLocal(a);
    a.status = std::to_string(ok) + " di " + std::to_string(todo.size()) + " file ripristinati";
    a.statusColor = ok == (int)todo.size() ? C_OK : C_WARN;
    inform("Risultato", report, ok == (int)todo.size() ? C_OK : C_WARN);
}

static void toggleAllInstalled(App& a) {
    if (a.sel.size() == a.installed.size()) {
        a.sel.clear();
        return;
    }
    for (const InstalledFile& f : a.installed) a.sel.insert(f.name);
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
               "Metti nella stessa cartella anche originali.txt (elenco dei .pak del gioco).",
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
    std::string err, warn;
    if (!mgr.load(err, warn)) {
        fatal("Errore", {err});
        return;
    }
    if (!warn.empty()) inform("Attenzione", {warn}, C_WARN);
    if (g_exit) return;
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
        int n = a.tab == 0 ? (int)a.remote.size() : (int)a.installed.size();
        int& cur = a.cursor[a.tab];
        if (k & HidNpadButton_AnyUp) cur = n ? (cur + n - 1) % n : 0;
        if (k & HidNpadButton_AnyDown) cur = n ? (cur + 1) % n : 0;
        if (k & HidNpadButton_AnyLeft) cur = std::max(0, cur - LIST_ROWS);
        if (k & HidNpadButton_AnyRight) cur = std::max(0, std::min(n - 1, cur + LIST_ROWS));
        if (k & HidNpadButton_L) a.tab = 0;
        if (k & HidNpadButton_R) a.tab = 1;
        if (k & HidNpadButton_A && n > 0) {
            if (a.tab == 0) {
                const RemoteFile& f = a.remote[cur];
                if (a.pick.erase(f.name) == 0) a.pick[f.name] = "";  // nome automatico
            } else {
                const std::string& name = a.installed[cur].name;
                if (!a.sel.erase(name)) a.sel.insert(name);
            }
        }
        if (k & HidNpadButton_Y) {
            if (a.tab == 0) playRemote(a);
            else toggleAllInstalled(a);
        }
        if (k & HidNpadButton_B && a.tab == 0 && n > 0) {  // scelta a mano dell'originale da sostituire
            const RemoteFile& f = a.remote[cur];
            std::string target;
            if (pickTarget(a, f, target)) assignPick(a, f.name, target);
        }
        if (k & HidNpadButton_X) {
            if (a.tab == 0) installSelected(a);
            else restoreSelected(a);
        }
        if (k & HidNpadButton_ZR && a.tab == 0) loadRemote(a);
        if (k & HidNpadButton_ZR && a.tab == 1 && n > 0) launchLevel(a, a.installed[cur].name);
        if (k & HidNpadButton_Minus) chooseSource(a);
        if (k & HidNpadButton_ZL) launchGame(a);
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
