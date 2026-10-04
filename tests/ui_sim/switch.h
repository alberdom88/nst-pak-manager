// switch.h finto: permette di eseguire main.cpp su PC per provare l'interfaccia.
// I tasti arrivano da un file (variabile SIM_KEYS), lo schermo e' lo stdout con
// le sequenze ANSI che interpreta ui_sim.py.
#pragma once

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

typedef uint64_t u64;
typedef uint32_t u32;
typedef int64_t s64;
typedef uint32_t Result;
#define R_SUCCEEDED(r) ((r) == 0)
#define R_FAILED(r) ((r) != 0)
#define BITL(n) (1ull << (n))

enum {
    HidNpadButton_A = BITL(0), HidNpadButton_B = BITL(1), HidNpadButton_X = BITL(2), HidNpadButton_Y = BITL(3),
    HidNpadButton_L = BITL(6), HidNpadButton_R = BITL(7), HidNpadButton_ZL = BITL(8), HidNpadButton_ZR = BITL(9),
    HidNpadButton_Plus = BITL(10), HidNpadButton_Minus = BITL(11),
    HidNpadButton_Left = BITL(12), HidNpadButton_Up = BITL(13), HidNpadButton_Right = BITL(14),
    HidNpadButton_Down = BITL(15),
    HidNpadButton_AnyLeft = BITL(12), HidNpadButton_AnyUp = BITL(13), HidNpadButton_AnyRight = BITL(14),
    HidNpadButton_AnyDown = BITL(15),
};
enum { HidNpadStyleSet_NpadStandard = 0 };
typedef enum { AppletType_Application = 0, AppletType_SystemApplication = 4, AppletType_LibraryApplet = 2 } AppletType;

struct PadState { u64 down = 0; };

namespace sim {
inline std::vector<u64>& script() { static std::vector<u64> s; return s; }
inline std::vector<std::string>& texts() { static std::vector<std::string> t; return t; }  // testi per la tastiera
inline size_t& pos() { static size_t p = 0; return p; }
inline bool& gameOpen() { static bool g = false; return g; }
inline void load() {
    const char* path = getenv("SIM_KEYS");
    FILE* f = path ? fopen(path, "r") : nullptr;
    if (!f) return;
    char tok[64];
    while (fscanf(f, "%63s", tok) == 1) {
        std::string t = tok;
        u64 k = 0;
        if (t == ".") k = 0;
        else if (t == "A") k = HidNpadButton_A; else if (t == "B") k = HidNpadButton_B;
        else if (t == "X") k = HidNpadButton_X; else if (t == "Y") k = HidNpadButton_Y;
        else if (t == "L") k = HidNpadButton_L; else if (t == "R") k = HidNpadButton_R;
        else if (t == "ZL") k = HidNpadButton_ZL; else if (t == "ZR") k = HidNpadButton_ZR; else if (t == "PLUS") k = HidNpadButton_Plus;
        else if (t == "MINUS") k = HidNpadButton_Minus; else if (t == "UP") k = HidNpadButton_Up;
        else if (t == "DOWN") k = HidNpadButton_Down; else if (t == "LEFT") k = HidNpadButton_Left;
        else if (t == "RIGHT") k = HidNpadButton_Right;
        else if (t == "SNAP") k = 1ull << 63;
        else if (t == "GAME") k = 1ull << 62;
        else if (t.rfind("TEXT=", 0) == 0) { texts().push_back(t.substr(5)); continue; }
        else if (t.rfind("#", 0) == 0) { int c; while ((c = fgetc(f)) != EOF && c != '\n') {} continue; }
        else { fprintf(stderr, "tasto sconosciuto %s\n", tok); exit(3); }
        script().push_back(k);
    }
    fclose(f);
}
}  // namespace sim

inline void padConfigureInput(u32, u32) { sim::load(); }
inline void padInitializeDefault(PadState*) {}
inline void padUpdate(PadState* p) {
    p->down = 0;
    while (sim::pos() < sim::script().size()) {
        u64 k = sim::script()[sim::pos()++];
        if (k == (1ull << 63)) { printf("\x1b]SNAP\x07"); fflush(stdout); continue; }
        if (k == (1ull << 62)) { sim::gameOpen() = !sim::gameOpen(); continue; }
        p->down = k;
        return;
    }
}
inline u64 padGetButtonsDown(const PadState* p) { return p->down; }
inline u64 padGetButtons(const PadState*) { return 0; }
inline bool appletMainLoop() { return sim::pos() < sim::script().size(); }
inline AppletType appletGetAppletType() { return AppletType_LibraryApplet; }
inline Result appletSetAutoSleepDisabled(bool) { return 0; }
inline Result appletSetMediaPlaybackState(bool) { return 0; }
struct AppletStorage;
inline Result appletRequestLaunchApplication(u64 tid, AppletStorage*) {
    printf("\x1b]EVENT avvio gioco %016llX\x07", (unsigned long long)tid);
    return 0;
}
inline void* consoleInit(void*) { return nullptr; }
inline void consoleUpdate(void*) { fflush(stdout); }
inline void consoleExit(void*) {}
inline Result socketInitializeDefault() { return 0; }
inline void socketExit() {}
inline Result ldrShellInitialize() { return 0; }
inline void ldrShellExit() {}
inline Result ldrShellFlushArguments() {
    printf("\x1b]EVENT argomenti di avvio azzerati\x07");
    return 0;
}
inline Result ldrShellSetProgramArguments(u64 tid, const void* args, size_t size) {
    printf("\x1b]EVENT argomenti di avvio %016llX: '%s' (%zu byte)\x07", (unsigned long long)tid, (const char*)args, size);
    return 0;
}
inline Result pmdmntInitialize() { return 0; }
inline void pmdmntExit() {}
inline Result pmdmntGetApplicationProcessId(u64* pid) { *pid = 1; return sim::gameOpen() ? 0 : 1; }
inline u64 armGetSystemTickFreq() { return 19200000; }
inline u64 armGetSystemTick() {
    using namespace std::chrono;
    return (u64)duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count() * 192 / 10000;
}

// Tastiera: restituisce il prossimo TEXT= dello scenario (i '~' diventano spazi); errore se finiti
struct SwkbdConfig { int dummy; };
inline Result swkbdCreate(SwkbdConfig*, int) { return 0; }
inline void swkbdConfigMakePresetDefault(SwkbdConfig*) {}
inline void swkbdConfigSetHeaderText(SwkbdConfig*, const char*) {}
inline void swkbdConfigSetGuideText(SwkbdConfig*, const char*) {}
inline void swkbdConfigSetInitialText(SwkbdConfig*, const char*) {}
inline void swkbdClose(SwkbdConfig*) {}
inline Result swkbdShow(SwkbdConfig*, char* out, size_t size) {
    static size_t next = 0;
    if (next >= sim::texts().size()) return 1;
    std::string t = sim::texts()[next++];
    for (char& c : t) if (c == '~') c = ' ';
    snprintf(out, size, "%s", t.c_str());
    return 0;
}
