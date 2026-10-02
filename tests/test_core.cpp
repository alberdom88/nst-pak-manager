// Test del core su PC (Linux): sorgenti remote, installazione, backup, ripristino.
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "../source/core.hpp"
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
    CHECK(driveFolderId("https://drive.google.com/drive/folders/ABC_12-x?usp=sharing") == "ABC_12-x");
    CHECK(driveFolderId("https://drive.google.com/open?id=XYZ&foo=1") == "XYZ");
    CHECK(driveFolderId("  RAWID ") == "RAWID");
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
        CHECK(c.sources.size() == 2 && c.sources[1].type == "gdrive");
        CHECK(c.titleId == "0100D1B006744000");
        CHECK(!parseConfig("{\"sources\":[]}", c, err));
        CHECK(!parseConfig("{\"title_id\":\"xyz\",\"sources\":[{\"url\":\"http://a/\"}]}", c, err));
        CHECK(!parseConfig("{\"sources\":[{\"type\":\"gdrive\",\"folder\":\"x\"}]}", c, err));
        CHECK(!parseConfig("{ rotto", c, err));
        CHECK(parseConfig("{\"title_id\":\"0100d1b006744000\",\"sources\":[{\"url\":\"http://a/\"}]}", c, err));
        CHECK(c.titleId == "0100D1B006744000" && c.sources[0].type == "http" && c.sources[0].name == "http://a/");
    }

    // ---------- elenco degli originali
    {
        std::vector<std::string> o = parseOriginals(
            "\xEF\xBB\xBFupdate.pak\r\n# commento\n\n  archives/L101_NSanityBeach.pak \n"
            "C:\\dump\\archives\\L102_Jungle.pak\nUPDATE.PAK\nnote.txt\n../x.pak");
        CHECK(o.size() == 4);
        CHECK(o.size() == 4 && o[0] == "L101_NSanityBeach.pak" && o[1] == "L102_Jungle.pak" &&
              o[2] == "update.pak" && o[3] == "x.pak");
    }

    // ---------- sorgente http: elenco cartella
    std::vector<RemoteFile> httpList;
    std::string err;
    SourceConfig httpSrc;
    httpSrc.name = "PC";
    httpSrc.type = "http";
    httpSrc.url = base + "/files/";
    CHECK(listSource(httpSrc, httpList, err));
    CHECK(httpList.size() == 3);
    CHECK(httpList.size() == 3 && httpList[0].name == "L101_NSanityBeach.pak" &&
          httpList[1].name == "Nome con spazi.pak" && httpList[2].name == "update.pak");
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
    CHECK(byName(man, "custom_v2.pak") && byName(man, "custom_v2.pak")->target == "L101_NSanityBeach.pak");
    CHECK(byName(man, "update.pak") && byName(man, "update.pak")->target.empty());
    CHECK(byName(man, "update.pak") && byName(man, "update.pak")->url == base + "/m/files/update.pak" &&
          byName(man, "update.pak")->sizeKnown && byName(man, "update.pak")->size == 3000);
    CHECK(byName(man, "Nome con spazi.pak") &&
          byName(man, "Nome con spazi.pak")->url == base + "/m/Nome%20con%20spazi.pak");
    CHECK(byName(man, "L101_NSanityBeach.pak") && byName(man, "L101_NSanityBeach.pak")->size == 70000);
    CHECK(byName(man, "wrong_size.pak") && byName(man, "wrong_size.pak")->size == 9999);
    CHECK(byName(man, "../evil.pak") == nullptr);

    // ---------- sorgente Google Drive (finta API)
    std::vector<RemoteFile> drive;
    SourceConfig driveSrc;
    driveSrc.name = "Drive";
    driveSrc.type = "gdrive";
    driveSrc.folder = "https://drive.google.com/drive/folders/FOLDER123?usp=sharing";
    driveSrc.apiKey = "TESTKEY";
    driveSrc.apiBase = base;
    CHECK(listSource(driveSrc, drive, err));
    CHECK(drive.size() == 3);  // 2 pagine, cartelle e .txt esclusi
    CHECK(byName(drive, "L102_Jungle.pak") && byName(drive, "L102_Jungle.pak")->size == 1234);
    SourceConfig badKey = driveSrc;
    badKey.apiKey = "SBAGLIATA";
    CHECK(!listSource(badKey, none, err));
    CHECK(err.find("API key not valid") != std::string::npos);

    // ---------- installazione / backup / ripristino
    Config cfg;
    Manager m(sd, cfg);
    std::string warn;
    CHECK(m.load(err, warn) && warn.empty());
    CHECK(m.modDir() == sd + "/atmosphere/contents/0100D1B006744000/romfs/archives");
    util::mkdirs(m.modDir());
    const std::string updDest = m.modDir() + "/update.pak";
    spit(updDest, "MOD-PRECEDENTE");  // es. una mod gia' installata a mano

    uint64_t lastTotal = 0;
    int calls = 0;
    net::Progress track = [&](uint64_t, uint64_t total) {
        calls++;
        lastTotal = total;
        return true;
    };
    const RemoteFile* upd = byName(httpList, "update.pak");
    CHECK(m.install(*upd, upd->name, "PC", track, err));
    CHECK(calls > 0 && lastTotal == 3000);
    CHECK(slurp(updDest).compare(0, 4, "IGA\x1a") == 0 && slurp(updDest).size() == 3000);
    CHECK(slurp(m.backupDir() + "/update.pak") == "MOD-PRECEDENTE");
    CHECK(m.find("update.pak") && m.find("update.pak")->backup == "update.pak");

    // reinstallo da un'altra sorgente: niente secondo backup
    const RemoteFile* updDrive = byName(drive, "update.pak");
    CHECK(m.install(*updDrive, updDrive->name, "Drive", noProgress, err));
    CHECK(slurp(updDest).size() == 5000);
    CHECK(util::listFiles(m.backupDir()).size() == 1);
    CHECK(m.find("update.pak")->source == "Drive" && m.find("update.pak")->backup == "update.pak");

    // file nuovo, senza nulla da salvare; nome con spazi
    CHECK(m.install(*byName(httpList, "Nome con spazi.pak"), "Nome con spazi.pak", "PC", noProgress, err));
    CHECK(m.find("Nome con spazi.pak") && m.find("Nome con spazi.pak")->backup.empty());
    CHECK(m.install(*byName(drive, "L101_NSanityBeach.pak"), "L101_NSanityBeach.pak", "Drive", noProgress, err));

    // file remoto con un nome diverso dall'originale da sostituire
    const std::vector<std::string> originals = {"L101_NSanityBeach.pak", "L102_Jungle.pak", "update.pak"};
    const RemoteFile* custom = byName(man, "custom_v2.pak");
    CHECK(m.suggestTarget(*custom, originals) == "L101_NSanityBeach.pak");      // dal manifest
    CHECK(m.suggestTarget(*byName(httpList, "update.pak"), originals) == "update.pak");  // stesso nome
    RemoteFile mixed = *custom;
    mixed.target = "l101_nsanitybeach.pak";
    CHECK(m.suggestTarget(mixed, originals) == "L101_NSanityBeach.pak");        // grafia dell'elenco
    RemoteFile unknown;
    unknown.name = "MioLivello.pak";
    CHECK(m.suggestTarget(unknown, originals).empty());                          // da scegliere
    spit(m.modDir() + "/L102_Jungle.pak", "ALTRA-MOD");
    const RemoteFile* spazi = byName(httpList, "Nome con spazi.pak");
    CHECK(m.install(*spazi, "L102_Jungle.pak", "PC", noProgress, err));
    CHECK(slurp(m.modDir() + "/L102_Jungle.pak").size() == 2048);
    CHECK(slurp(m.backupDir() + "/L102_Jungle.pak") == "ALTRA-MOD");
    CHECK(m.find("L102_Jungle.pak") && m.find("L102_Jungle.pak")->remote == "Nome con spazi.pak");
    CHECK(m.rememberedTarget("Nome con spazi.pak") == "L102_Jungle.pak");
    CHECK(m.suggestTarget(*spazi, originals) == "L102_Jungle.pak");             // scelta ricordata
    CHECK(m.installedFrom("Nome con spazi.pak").size() == 2);  // anche con il suo nome, installato sopra

    // lo stato sopravvive al riavvio, abbinamenti compresi
    {
        Manager m2(sd, cfg);
        CHECK(m2.load(err, warn) && m2.installed().size() == 4);
        CHECK(m2.find("L102_Jungle.pak") && m2.find("L102_Jungle.pak")->remote == "Nome con spazi.pak");
        CHECK(m2.rememberedTarget("Nome con spazi.pak") == "L102_Jungle.pak");
    }
    std::string note0;
    CHECK(m.restore("L102_Jungle.pak", err, note0));
    CHECK(slurp(m.modDir() + "/L102_Jungle.pak") == "ALTRA-MOD");
    CHECK(m.rememberedTarget("Nome con spazi.pak") == "L102_Jungle.pak");      // il ricordo resta

    // file esterni
    spit(m.modDir() + "/altra_mod.pak", "X");
    std::vector<std::string> ext = m.externalFiles();
    CHECK(ext.size() == 2 && ext[0] == "L102_Jungle.pak" && ext[1] == "altra_mod.pak");

    // errori: nessuna modifica ai file presenti
    std::string before = slurp(updDest);
    RemoteFile bad;
    bad.name = "update.pak";
    bad.url = base + "/fake/not_a_pak.pak";
    CHECK(!m.install(bad, bad.name, "PC", noProgress, err) && err.find("non e' un .pak") != std::string::npos);
    CHECK(slurp(updDest) == before && !util::fileExists(updDest + ".part"));
    const RemoteFile* wrong = byName(man, "wrong_size.pak");
    CHECK(wrong && !m.install(*wrong, wrong->name, "PC", noProgress, err) && err.find("incompleto") != std::string::npos);
    CHECK(!util::fileExists(m.modDir() + "/wrong_size.pak"));
    RemoteFile gone;
    gone.name = "update.pak";
    gone.url = base + "/files/manca.pak";
    CHECK(!m.install(gone, gone.name, "PC", noProgress, err) && err.find("404") != std::string::npos);
    CHECK(slurp(updDest) == before);
    RemoteFile evil;
    evil.name = "../evil.pak";
    evil.url = base + "/files/update.pak";
    CHECK(!m.install(evil, evil.name, "PC", noProgress, err));
    CHECK(!m.install(*upd, "../evil.pak", "PC", noProgress, err));
    CHECK(!m.install(*upd, "nome.txt", "PC", noProgress, err));

    // annullamento a meta' download
    RemoteFile slow;
    slow.name = "update.pak";
    slow.url = base + "/fake/slow.pak";
    net::Progress cancelSoon = [](uint64_t done, uint64_t) { return done < 100 * 1024; };
    CHECK(!m.install(slow, slow.name, "PC", cancelSoon, err) && err == "annullato");
    CHECK(slurp(updDest) == before && !util::fileExists(updDest + ".part"));

    // spazio insufficiente (dimensione dichiarata enorme)
    RemoteFile huge = *byName(drive, "L102_Jungle.pak");
    huge.size = 1ull << 50;
    CHECK(!m.install(huge, huge.name, "Drive", noProgress, err) && err.find("spazio insufficiente") != std::string::npos);

    // ripristino
    std::string note;
    CHECK(m.restore("update.pak", err, note) && note.empty());
    CHECK(slurp(updDest) == "MOD-PRECEDENTE");
    CHECK(!util::fileExists(m.backupDir() + "/update.pak"));
    CHECK(m.restore("L101_NSanityBeach.pak", err, note));
    CHECK(!util::fileExists(m.modDir() + "/L101_NSanityBeach.pak"));
    CHECK(!m.restore("altra_mod.pak", err, note));
    CHECK(util::fileExists(m.modDir() + "/altra_mod.pak"));

    // backup cancellato a mano: il ripristino rimuove solo il file
    CHECK(m.install(*upd, upd->name, "PC", noProgress, err));
    util::removeFile(m.backupDir() + "/update.pak");
    CHECK(m.restore("update.pak", err, note) && !note.empty());
    CHECK(!util::fileExists(updDest));

    // stato perso: il file gia' presente viene salvato con un nome nuovo
    spit(updDest, "MOD-PRECEDENTE");
    CHECK(m.install(*upd, upd->name, "PC", noProgress, err));
    {
        std::string state = m.appDir() + "/state-0100D1B006744000.json";
        util::removeFile(state);
        Manager m3(sd, cfg);
        CHECK(m3.load(err, warn) && m3.installed().empty());
        CHECK(m3.install(*updDrive, updDrive->name, "Drive", noProgress, err));
        CHECK(m3.find("update.pak")->backup == "update.pak.1");
        CHECK(slurp(m.backupDir() + "/update.pak") == "MOD-PRECEDENTE");
        CHECK(slurp(m.backupDir() + "/update.pak.1").size() == 3000);
        // stato corrotto: messo da parte, l'app continua
        spit(state, "{ non e' json");
        Manager m4(sd, cfg);
        CHECK(m4.load(err, warn) && !warn.empty() && m4.installed().empty());
        CHECK(util::fileExists(state + ".corrotto"));
    }

    // download interrotti
    spit(m.modDir() + "/vecchio.pak.part", "x");
    CHECK(m.cleanupPartials() == 1 && !util::fileExists(m.modDir() + "/vecchio.pak.part"));

    net::shutdown();
    printf("%d controlli superati, %d falliti\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
