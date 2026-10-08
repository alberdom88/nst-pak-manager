// Test del core su PC (Linux): sorgenti remote e preparazione del livello da giocare.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>

#include "../source/core.hpp"
#include "../source/mega.hpp"
#include "../source/pak.hpp"
#include "../source/util.hpp"

static int g_fail = 0, g_pass = 0;
#define CHECK(cond)                                                                  \
    do {                                                                             \
        if (cond) {                                                                  \
            g_pass++;                                                                \
        } else {                                                                     \
            g_fail++;                                                                \
            fprintf(stderr, "FALLITO %s:%d  %s\n", __FILE__, __LINE__, #cond);       \
        }                                                                            \
    } while (0)

static std::string slurp(const std::string& p) {
    std::string s;
    util::readFile(p, s);
    return s;
}

static void spit(const std::string& p, const std::string& data) {
    FILE* f = fopen(p.c_str(), "wb");
    fwrite(data.data(), 1, data.size(), f);
    fclose(f);
}

static const RemoteFile* byName(const std::vector<RemoteFile>& v, const std::string& n) {
    for (const RemoteFile& f : v)
        if (f.name == n) return &f;
    return nullptr;
}

static net::Progress noProgress = [](uint64_t, uint64_t) { return true; };

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "uso: test_core <base_url> <cartella_sd>\n");
        return 2;
    }
    const std::string base = argv[1];  // es. http://127.0.0.1:18765
    const std::string sd = argv[2];
    net::init();

    // ---------- funzioni pure
    CHECK(resolveUrl("http://h:1/a/b/", "x.pak") == "http://h:1/a/b/x.pak");
    CHECK(resolveUrl("http://h:1/a/b/list.html", "x.pak") == "http://h:1/a/b/x.pak");
    CHECK(resolveUrl("http://h:1", "x.pak") == "http://h:1/x.pak");
    CHECK(resolveUrl("http://h:1/a/b/", "/c/x.pak") == "http://h:1/c/x.pak");
    CHECK(resolveUrl("https://h/a/?sort=1", "./x.pak") == "https://h/a/x.pak");
    CHECK(resolveUrl("https://h/a/", "//cdn.example/x.pak") == "https://cdn.example/x.pak");
    CHECK(resolveUrl("https://h/a/", "https://o/x.pak") == "https://o/x.pak");
    CHECK(isValidPakName("L101_NSanityBeach.pak"));
    CHECK(isValidPakName("update.PAK"));
    CHECK(!isValidPakName("../evil.pak"));
    CHECK(!isValidPakName("a/b.pak"));
    CHECK(!isValidPakName(".pak"));
    CHECK(!isValidPakName("note.txt"));
    // ---------- MEGA: AES, CTR, base64 e link
    {
        uint8_t key[16], pt[16], ct[16], back[16];
        for (int i = 0; i < 16; i++) {
            key[i] = (uint8_t)i;
            pt[i] = (uint8_t)(i * 0x11);
        }
        mega::Aes128 aes(key);  // vettore di prova FIPS-197
        aes.encrypt(pt, ct);
        static const uint8_t expect[16] = {0x69, 0xc4, 0xe0, 0xd8, 0x6a, 0x7b, 0x04, 0x30,
                                           0xd8, 0xcd, 0xb7, 0x80, 0x70, 0xb4, 0xc5, 0x5a};
        CHECK(memcmp(ct, expect, 16) == 0);
        aes.decrypt(ct, back);
        CHECK(memcmp(back, pt, 16) == 0);
        // CTR a pezzi di dimensioni qualsiasi = CTR in un colpo solo
        std::string data(1000, '\0'), once, pieces;
        for (size_t i = 0; i < data.size(); i++) data[i] = (char)(i * 31 + 7);
        uint8_t iv[16] = {1, 2, 3, 4, 5, 6, 7, 8, 0, 0, 0, 0, 0, 0, 0, 0xFE};
        once = data;
        mega::Ctr c1(key, iv);
        c1.crypt((uint8_t*)&once[0], once.size());
        pieces = data;
        mega::Ctr c2(key, iv);
        size_t pos = 0, step = 1;
        while (pos < pieces.size()) {
            size_t n = std::min(step, pieces.size() - pos);
            c2.crypt((uint8_t*)&pieces[pos], n);
            pos += n;
            step = step * 3 % 41 + 1;
        }
        CHECK(once == pieces && once != data);
        mega::Ctr c3(key, iv);
        c3.crypt((uint8_t*)&once[0], once.size());
        CHECK(once == data);

        CHECK(mega::b64decode("AAEC") == std::string("\0\1\2", 3));
        CHECK(mega::b64decode("_-8") == std::string("\xff\xef", 2));
        mega::Link l;
        std::string e;
        CHECK(mega::parseLink(" https://mega.nz/folder/AbC123xy#EBESExQVFhcYGRobHB0eHw ", l, e) &&
              l.folder == "AbC123xy" && l.key.size() == 16 && l.sub.empty());
        CHECK(mega::parseLink("https://mega.nz/folder/AbC#EBESExQVFhcYGRobHB0eHw/folder/SUB1", l, e) &&
              l.folder == "AbC" && l.sub == "SUB1");
        CHECK(mega::parseLink("https://mega.nz/#F!AbC!EBESExQVFhcYGRobHB0eHw!SUB2", l, e) &&
              l.folder == "AbC" && l.sub == "SUB2" && l.key[0] == 0x10);
        CHECK(!mega::parseLink("https://mega.nz/file/AbC#EBESExQVFhcYGRobHB0eHw", l, e) &&
              e.find("singolo file") != std::string::npos);
        CHECK(!mega::parseLink("https://mega.nz/folder/AbC", l, e) && e.find("chiave") != std::string::npos);
        CHECK(!mega::parseLink("https://example.com/x", l, e));
    }
    {
        std::vector<RemoteFile> v;
        parseDirectoryListing(
            "<a href=\"a.pak\">a</a> <A HREF='b%20c.pak?x=1&amp;y=2'>b</A> <a href=d.pak>d</a>"
            "<a href=\"../up.pak\">u</a> <a href=\"x.txt\">x</a> <a href=\"a.pak\">dup</a>",
            "http://h/dir/", v);
        CHECK(v.size() == 4);
        CHECK(byName(v, "a.pak") && byName(v, "a.pak")->url == "http://h/dir/a.pak");
        CHECK(byName(v, "b c.pak") && byName(v, "b c.pak")->url == "http://h/dir/b%20c.pak?x=1&y=2");
        CHECK(byName(v, "d.pak") != nullptr);
        CHECK(byName(v, "up.pak") && byName(v, "up.pak")->url == "http://h/dir/../up.pak");
    }
    {
        Config c;
        std::string err;
        CHECK(parseConfig(defaultConfigJson(), c, err));
        CHECK(c.sources.size() == 2 && c.sources[1].type == "mega");
        CHECK(c.titleId == "0100D1B006744000");
        CHECK(!parseConfig("{\"sources\":[]}", c, err));
        CHECK(!parseConfig("{\"title_id\":\"xyz\",\"sources\":[{\"url\":\"http://a/\"}]}", c, err));
        CHECK(!parseConfig("{\"sources\":[{\"type\":\"gdrive\",\"url\":\"x\"}]}", c, err) &&
              err.find("Google Drive") != std::string::npos);
        // link MEGA incompleto: la configurazione si legge, l'errore compare nell'elenco della sorgente
        CHECK(parseConfig("{\"sources\":[{\"type\":\"mega\",\"url\":\"https://mega.nz/folder/X\"}]}", c, err));
        {
            std::vector<RemoteFile> v;
            CHECK(!listSource(c.sources[0], v, err) && err.find("chiave") != std::string::npos);
        }
        CHECK(parseConfig("{\"sources\":[{\"url\":\"https://mega.nz/folder/X#EBESExQVFhcYGRobHB0eHw\"}]}", c, err) &&
              c.sources[0].type == "mega" && c.sources[0].name == "MEGA");
        CHECK(!parseConfig("{ rotto", c, err));
        CHECK(parseConfig("{\"title_id\":\"0100d1b006744000\",\"sources\":[{\"url\":\"http://a/\"}]}", c, err));
        CHECK(c.titleId == "0100D1B006744000" && c.sources[0].type == "http" && c.sources[0].name == "http://a/");
        CHECK(c.launchArgs == "nst -om {livello}");
        CHECK(parseConfig("{\"launch_args\":\"x -om {livello}\",\"sources\":[{\"url\":\"http://a/\"}]}", c, err));
        CHECK(c.launchArgs == "x -om {livello}");
    }

    // ---------- sorgente http: elenco cartella
    std::vector<RemoteFile> httpList;
    std::string err;
    SourceConfig httpSrc;
    httpSrc.name = "PC";
    httpSrc.type = "http";
    httpSrc.url = base + "/files/";
    CHECK(listSource(httpSrc, httpList, err));
    CHECK(httpList.size() == 4);
    CHECK(httpList.size() == 4 && httpList[0].name == "Custom_Level.pak" &&
          httpList[1].name == "L101_NSanityBeach.pak" && httpList[2].name == "Nome con spazi.pak" &&
          httpList[3].name == "update.pak");
    CHECK(byName(httpList, "Nome con spazi.pak") &&
          byName(httpList, "Nome con spazi.pak")->url == base + "/files/Nome%20con%20spazi.pak");
    CHECK(byName(httpList, "update.pak") && byName(httpList, "update.pak")->sizeKnown &&
          byName(httpList, "update.pak")->size == 3000);
    CHECK(byName(httpList, "L101_NSanityBeach.pak")->size == 70000);

    // redirect: /redirect -> /files/ (i link vanno risolti rispetto all'URL finale)
    std::vector<RemoteFile> redirList;
    SourceConfig redirSrc = httpSrc;
    redirSrc.url = base + "/redirect";
    CHECK(listSource(redirSrc, redirList, err));
    CHECK(byName(redirList, "update.pak") && byName(redirList, "update.pak")->url == base + "/files/update.pak");

    // cartella inesistente
    std::vector<RemoteFile> none;
    SourceConfig missing = httpSrc;
    missing.url = base + "/non-esiste/";
    CHECK(!listSource(missing, none, err));
    CHECK(err.find("404") != std::string::npos);

    // ---------- sorgente http: manifest
    std::vector<RemoteFile> man;
    SourceConfig manSrc = httpSrc;
    manSrc.url = base + "/m/manifest.json";
    CHECK(listSource(manSrc, man, err));
    CHECK(man.size() == 5);
    CHECK(byName(man, "custom_v2.pak") != nullptr);  // "target" del manifest: ignorato
    CHECK(byName(man, "update.pak") && byName(man, "update.pak")->url == base + "/m/files/update.pak" &&
          byName(man, "update.pak")->sizeKnown && byName(man, "update.pak")->size == 3000);
    CHECK(byName(man, "Nome con spazi.pak") &&
          byName(man, "Nome con spazi.pak")->url == base + "/m/Nome%20con%20spazi.pak");
    CHECK(byName(man, "L101_NSanityBeach.pak") && byName(man, "L101_NSanityBeach.pak")->size == 70000);
    CHECK(byName(man, "wrong_size.pak") && byName(man, "wrong_size.pak")->size == 9999);
    CHECK(byName(man, "../evil.pak") == nullptr);

    // ---------- sorgente MEGA (finta API: la prima richiesta risponde "occupato")
    std::vector<RemoteFile> megaFiles;  // usati anche nei test di installazione
    SourceConfig megaSrc;
    megaSrc.name = "MEGA";
    megaSrc.type = "mega";
    megaSrc.url = "https://mega.nz/folder/PUBFOLD1#EBESExQVFhcYGRobHB0eHw";
    megaSrc.apiBase = base + "/mega";
    CHECK(listSource(megaSrc, megaFiles, err));
    CHECK(megaFiles.size() == 3);  // .txt e file della sottocartella esclusi
    CHECK(megaFiles.size() == 3 && megaFiles[0].name == "L101_NSanityBeach.pak" && megaFiles[1].name == "L102_Jungle.pak" &&
          megaFiles[2].name == "update.pak");
    CHECK(byName(megaFiles, "L102_Jungle.pak") && byName(megaFiles, "L102_Jungle.pak")->size == 1234 &&
          byName(megaFiles, "L102_Jungle.pak")->megaKey.size() == 32);
    {
        SourceConfig sub = megaSrc;
        sub.url += "/folder/SUB00001";
        std::vector<RemoteFile> v;
        CHECK(listSource(sub, v, err) && v.size() == 1 && v[0].name == "L201_Sub.pak");
        SourceConfig wrongKey = megaSrc;
        wrongKey.url = "https://mega.nz/folder/PUBFOLD1#ZGVmZ2hpamtsbW5vcHFycw";
        CHECK(!listSource(wrongKey, v, err) && err.find("chiave") != std::string::npos);
        SourceConfig wrongFolder = megaSrc;
        wrongFolder.url = "https://mega.nz/folder/ALTRA001#EBESExQVFhcYGRobHB0eHw";
        CHECK(!listSource(wrongFolder, v, err) && err.find("non trovata") != std::string::npos);
        SourceConfig missing = sub;
        missing.url = megaSrc.url + "/folder/NOSUB001";
        CHECK(!listSource(missing, v, err) && err.find("sottocartella") != std::string::npos);
    }

    // ---------- nomi, gioco scelto, debug.xml
    CHECK(levelPakName("Custom_Level") == "custom_level.pak");
    CHECK(levelPakName("crash3/oichi_level2/oichi_level2") == "oichi_level2.pak");
    CHECK(normalizeGame("Crash2") == "crash2" && normalizeGame("") == "auto" && normalizeGame("crash4") == "auto");
    CHECK(nextGame("auto", 1) == "crash1" && nextGame("crash3", 1) == "auto");
    CHECK(nextGame("auto", -1) == "crash3" && nextGame("crash1", -1) == "auto" && nextGame("xyz", 2) == "crash2");
    CHECK(gameLabel("auto") == "Auto" && gameLabel("crash2") == "Crash 2" && gameLabel("?") == "Auto");
    CHECK(levelIdForGame("crash3/oichi/oichi", "auto") == "crash3/oichi/oichi");
    CHECK(levelIdForGame("crash3/oichi/oichi", "crash1") == "crash1/oichi/oichi");
    CHECK(levelIdForGame("senza_cartella", "crash1") == "senza_cartella");
    CHECK(launchArguments("nst -om {livello}", "crash1/a/a") == "nst -om crash1/a/a");
    CHECK(launchArguments("{livello} {livello}", "a") == "a a");
    CHECK(launchArguments("nst", "a") == "nst");
    CHECK(debugXmlLevel(debugXml("crash1/a/a")) == "crash1/a/a");
    CHECK(debugXmlLevel("<config><MAP checkpoint='x' filename='a/b/c'/></config>") == "a/b/c");
    CHECK(debugXmlLevel("<config><INIT debugGameMode=\"1\"/></config>").empty());

    // ---------- preparazione del livello
    Config cfg;
    Manager m(sd, cfg);
    CHECK(m.load(err) && m.game() == "auto" && m.lastLevel().empty());
    CHECK(m.modDir() == sd + "/atmosphere/contents/0100D1B006744000/romfs/archives");
    CHECK(m.romfsDir() + "/archives" == m.modDir());
    const std::string mod = m.modDir();
    auto modFiles = [&]() {
        std::vector<std::string> v = util::listFiles(mod);
        std::sort(v.begin(), v.end());
        return v;
    };
    Prepared p;

    // un .pak che non e' un livello: errore, niente installato
    CHECK(!m.prepare(*byName(httpList, "update.pak"), noProgress, p, err) &&
          err.find("non contiene un livello") != std::string::npos);
    CHECK(modFiles().empty() && !util::fileExists(m.directLaunchPath()));

    // errori di download: nessuna modifica
    RemoteFile bad;
    bad.name = "livello.pak";
    bad.url = base + "/fake/not_a_pak.pak";
    CHECK(!m.prepare(bad, noProgress, p, err) && err.find("non e' un .pak") != std::string::npos);
    const RemoteFile* wrong = byName(man, "wrong_size.pak");
    CHECK(wrong && !m.prepare(*wrong, noProgress, p, err) && err.find("incompleto") != std::string::npos);
    RemoteFile gone;
    gone.name = "manca.pak";
    gone.url = base + "/files/manca.pak";
    CHECK(!m.prepare(gone, noProgress, p, err) && err.find("404") != std::string::npos);
    RemoteFile evil;
    evil.name = "../evil.pak";
    evil.url = base + "/files/update.pak";
    CHECK(!m.prepare(evil, noProgress, p, err));
    RemoteFile slow;
    slow.name = "lento.pak";
    slow.url = base + "/fake/slow.pak";
    net::Progress cancelSoon = [](uint64_t done, uint64_t) { return done < 100 * 1024; };
    CHECK(!m.prepare(slow, cancelSoon, p, err) && err == "annullato");
    RemoteFile huge = *byName(megaFiles, "L102_Jungle.pak");
    huge.size = 1ull << 50;
    CHECK(!m.prepare(huge, noProgress, p, err) && err.find("spazio insufficiente") != std::string::npos);
    CHECK(modFiles().empty() && !util::fileExists(m.directLaunchPath()));

    // livello con un nome remoto qualsiasi: installato con il nome che cerca il gioco
    spit(mod + "/Custom_Level.pak", "INSTALLATO-DALLA-1.8.0");  // stesso livello, con le maiuscole
    spit(mod + "/update.pak", "REGISTRAZIONE-DI-PRIMA");         // di un livello nuovo giocato prima
    RemoteFile renamed = *byName(httpList, "Custom_Level.pak");
    renamed.name = "Il mio livello v2.pak";
    int calls = 0;
    uint64_t lastTotal = 0;
    net::Progress count = [&](uint64_t, uint64_t total) {
        calls++;
        lastTotal = total;
        return true;
    };
    CHECK(m.prepare(renamed, count, p, err));
    CHECK(calls > 0 && lastTotal == renamed.size);
    CHECK(p.pakName == "custom_level.pak" && p.levelId == "crash1/custom_level/custom_level" && p.launchId == p.levelId);
    CHECK(!p.registered && p.updateRemoved && p.removed.empty());
    CHECK(modFiles() == std::vector<std::string>{"custom_level.pak"});
    std::vector<std::string> ids;
    CHECK(pakLevelIds(mod + "/custom_level.pak", ids) && ids.size() == 1 && ids[0] == "crash1/custom_level/custom_level");
    CHECK(pakLevelIds(mod + "/manca.pak", ids) == false);
    CHECK(m.directLaunchLevel() == "crash1/custom_level/custom_level");
    CHECK(slurp(m.directLaunchPath()).find("<MAP filename=\"crash1/custom_level/custom_level\"/>") != std::string::npos);
    CHECK(m.lastLevel() == "custom_level.pak");

    // di nuovo: scarica sempre, anche se c'e' gia'
    calls = 0;
    CHECK(m.prepare(renamed, count, p, err) && calls > 0 && !p.updateRemoved && p.removed.empty());
    CHECK(modFiles() == std::vector<std::string>{"custom_level.pak"});

    // gioco scelto a mano: cambia solo l'identificativo in debug.xml
    CHECK(m.setGame("crash3", err) && m.game() == "crash3");
    CHECK(m.prepare(renamed, noProgress, p, err) && p.levelId == "crash1/custom_level/custom_level" &&
          p.launchId == "crash3/custom_level/custom_level");
    CHECK(m.directLaunchLevel() == "crash3/custom_level/custom_level");
    {
        Manager again(sd, cfg);
        CHECK(again.load(err) && again.game() == "crash3" && again.lastLevel() == "custom_level.pak");
    }
    CHECK(m.setGame("qualcosa", err) && m.game() == "auto");

    // stato illeggibile: si riparte da zero
    spit(m.appDir() + "/stato-0100D1B006744000.json", "{ rotto");
    {
        Manager broken(sd, cfg);
        CHECK(broken.load(err) && broken.game() == "auto" && broken.lastLevel().empty());
    }
    CHECK(m.setGame("auto", err));  // riscrive lo stato con l'ultimo livello

    // avvio diretto e patch
    m.clearDirectLaunch();
    CHECK(m.directLaunchLevel().empty() && !util::fileExists(m.directLaunchPath()));
    CHECK(m.setDirectLaunch("crash1/a/a", err) && m.directLaunchLevel() == "crash1/a/a");
    CHECK(!m.directLaunchPatchInstalled());
    util::mkdirs(sd + DIRECT_LAUNCH_PATCH_DIR);
    spit(sd + DIRECT_LAUNCH_PATCH_DIR + "/LEGGIMI.txt", "x");
    CHECK(!m.directLaunchPatchInstalled());
    spit(sd + DIRECT_LAUNCH_PATCH_DIR + "/29E1A37D84227147A50A18D055FBB032.ips", "IPS32EEOF");
    CHECK(m.directLaunchPatchInstalled());

    // download interrotti
    spit(mod + "/vecchio.pak.part", "x");
    CHECK(m.cleanupPartials() == 1 && !util::fileExists(mod + "/vecchio.pak.part"));

    // ---------- livello nuovo: l'app crea update.pak (pak.cpp); il livello di prima si toglie
    if (argc > 3) {
        const std::string pakDir = argv[3];  // archivi creati da tests/crea_pak_prova.py
        pak::Archive basePak;
        CHECK(pak::read(pakDir + "/base_update.pak", basePak, err) && basePak.version == 12 && basePak.entries.size() == 6);
        int compressed = 0;
        for (const pak::Entry& e : basePak.entries) {
            compressed += e.compression != 0;
            CHECK(e.hash == pak::hashPath(e.path));
        }
        CHECK(compressed == 5);
        std::string data;
        CHECK(!pak::extract(basePak, basePak.entries[0].compression ? basePak.entries[0] : basePak.entries[1], data, err) ||
              basePak.entries[0].compression == 0);  // i file compressi non si estraggono
        CHECK(pak::write(basePak, pakDir + "/riscritto.pak", err));  // copia identica (verificata da verifica_pak.py)

        // il gioco (1.0.0) non ha update.pak: nessuna copia dell'originale, update.pak con la sola registrazione
        RemoteFile nuovo;
        nuovo.name = "Nuovo.pak";
        nuovo.url = base + "/nuovo/Nuovo.pak";
        CHECK(!util::fileExists(m.originalUpdatePath()));
        CHECK(m.prepare(nuovo, noProgress, p, err));
        CHECK(p.pakName == "nuovo.pak" && p.levelId == "crash1/nuovo/nuovo" && p.registered && !p.updateRemoved);
        CHECK(p.removed == "custom_level.pak");  // si gioca un livello alla volta
        CHECK(modFiles() == (std::vector<std::string>{"nuovo.pak", "update.pak"}));
        pak::Archive merged;
        CHECK(pak::read(mod + "/update.pak", merged, err) && merged.entries.size() == 2);
        CHECK(m.directLaunchLevel() == "crash1/nuovo/nuovo");

        // con la copia dell'update.pak originale: unione
        util::mkdirs(m.appDir() + "/originali");
        spit(m.originalUpdatePath(), slurp(pakDir + "/base_update.pak"));
        RemoteFile reg;
        reg.name = "Custom_Level.pak";
        reg.url = base + "/reg/Custom_Level.pak";
        CHECK(m.prepare(reg, noProgress, p, err));
        CHECK(p.pakName == "custom_level.pak" && p.registered && p.removed == "nuovo.pak");
        CHECK(modFiles() == (std::vector<std::string>{"custom_level.pak", "update.pak"}));
        CHECK(!util::fileExists(mod + "/update.pak.part"));
        CHECK(pak::read(mod + "/update.pak", merged, err) && merged.entries.size() == 7);
        for (const pak::Entry& e : merged.entries)
            if (e.path == "maps/crash1/custom_level/custom_level_zoneinfo.igz") {
                CHECK(pak::extract(merged, e, data, err) && data.size() == 2600 && data.compare(0, 11, "zone-nuova:") == 0);
            }
        spit(pakDir + "/unito.pak", slurp(mod + "/update.pak"));  // controllato da verifica_pak.py

        // copia dell'originale rovinata: errore chiaro
        spit(m.originalUpdatePath(), "NON E' UN PAK");
        CHECK(!m.prepare(reg, noProgress, p, err) && err.find("update.pak originale") != std::string::npos);

        // un livello con il nome di uno originale dopo uno nuovo: update.pak tolto
        CHECK(m.prepare(renamed, noProgress, p, err) && p.updateRemoved && !p.registered);
        CHECK(modFiles() == std::vector<std::string>{"custom_level.pak"});
    }

    net::shutdown();
    printf("%d controlli superati, %d falliti\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
