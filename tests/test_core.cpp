// Test del core su PC (Linux): sorgenti remote, installazione, backup, ripristino.
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
    CHECK(byName(man, "custom_v2.pak") && byName(man, "custom_v2.pak")->target == "L101_NSanityBeach.pak");
    CHECK(byName(man, "update.pak") && byName(man, "update.pak")->target.empty());
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
    const RemoteFile* updMega = byName(megaFiles, "update.pak");
    CHECK(m.install(*updMega, updMega->name, "MEGA", noProgress, err));
    CHECK(slurp(updDest).size() == 5000 && slurp(updDest).compare(0, 19, "IGA\x1amega-update.pak") == 0);
    CHECK(slurp(updDest).compare(4990, 10, slurp(updDest).substr(4990 - 15, 10)) == 0);  // decifrato fino in fondo
    CHECK(util::listFiles(m.backupDir()).size() == 1);
    CHECK(m.find("update.pak")->source == "MEGA" && m.find("update.pak")->backup == "update.pak");

    // file nuovo, senza nulla da salvare; nome con spazi
    CHECK(m.install(*byName(httpList, "Nome con spazi.pak"), "Nome con spazi.pak", "PC", noProgress, err));
    CHECK(m.find("Nome con spazi.pak") && m.find("Nome con spazi.pak")->backup.empty());
    CHECK(m.install(*byName(megaFiles, "L101_NSanityBeach.pak"), "L101_NSanityBeach.pak", "MEGA", noProgress, err));
    CHECK(slurp(m.modDir() + "/L101_NSanityBeach.pak").find("mega-L101_NSanityBeach.pak", 69900 - 30) != std::string::npos);

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
    RemoteFile huge = *byName(megaFiles, "L102_Jungle.pak");
    huge.size = 1ull << 50;
    CHECK(!m.install(huge, huge.name, "MEGA", noProgress, err) && err.find("spazio insufficiente") != std::string::npos);

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
        CHECK(m3.install(*updMega, updMega->name, "MEGA", noProgress, err));
        CHECK(m3.find("update.pak")->backup == "update.pak.1");
        CHECK(slurp(m.backupDir() + "/update.pak") == "MOD-PRECEDENTE");
        CHECK(slurp(m.backupDir() + "/update.pak.1").size() == 3000);
        // stato corrotto: messo da parte, l'app continua
        spit(state, "{ non e' json");
        Manager m4(sd, cfg);
        CHECK(m4.load(err, warn) && !warn.empty() && m4.installed().empty());
        CHECK(util::fileExists(state + ".corrotto"));
    }

    // livello con un nome interno diverso dal nome di destinazione
    {
        const RemoteFile* custom = byName(httpList, "Custom_Level.pak");
        CHECK(custom != nullptr);
        std::string l101 = m.modDir() + "/L101_NSanityBeach.pak";
        bool hadL101 = util::fileExists(l101);
        std::string beforeL101 = slurp(l101);
        CHECK(!m.install(*custom, "L101_NSanityBeach.pak", "PC", noProgress, err));
        CHECK(err.find("Custom_Level") != std::string::npos && err.find("Nessuna modifica") != std::string::npos);
        CHECK(util::fileExists(l101) == hadL101 && slurp(l101) == beforeL101);
        CHECK(!util::fileExists(l101 + ".part"));
        CHECK(m.install(*custom, "custom_level.PAK", "PC", noProgress, err));  // stesso livello: OK
        std::vector<std::string> lv;
        CHECK(pakLevelNames(m.modDir() + "/custom_level.PAK", lv) && lv.size() == 1 && lv[0] == "Custom_Level");
        CHECK(pakLevelNames(updDest, lv) == false || lv.empty());  // file senza elenco valido: nessun livello
        std::vector<std::string> ids;
        CHECK(pakLevelIds(m.modDir() + "/custom_level.PAK", ids) && ids.size() == 1 &&
              ids[0] == "crash1/custom_level/custom_level");
        CHECK(launchArguments("nst -om {livello}", ids[0]) == "nst -om crash1/custom_level/custom_level");
        CHECK(launchArguments("{livello} {livello}", "a") == "a a");
        CHECK(launchArguments("nst", "a") == "nst");
        // avvio diretto con debug.xml
        CHECK(debugXmlLevel(debugXml(ids[0])) == ids[0]);
        CHECK(debugXmlLevel("<config><MAP checkpoint='x' filename='a/b/c'/></config>") == "a/b/c");
        CHECK(debugXmlLevel("<config><INIT debugGameMode=\"1\"/></config>").empty());
        CHECK(m.romfsDir() + "/archives" == m.modDir());
        CHECK(m.directLaunchLevel().empty());
        CHECK(m.setDirectLaunch(ids[0], err) && m.directLaunchLevel() == ids[0]);
        CHECK(util::fileExists(m.romfsDir() + "/debug.xml"));
        m.clearDirectLaunch();
        CHECK(m.directLaunchLevel().empty() && !util::fileExists(m.romfsDir() + "/debug.xml"));
        CHECK(!m.directLaunchPatchInstalled());
        util::mkdirs(sd + DIRECT_LAUNCH_PATCH_DIR);
        spit(sd + DIRECT_LAUNCH_PATCH_DIR + "/LEGGIMI.txt", "x");
        CHECK(!m.directLaunchPatchInstalled());
        spit(sd + DIRECT_LAUNCH_PATCH_DIR + "/29E1A37D84227147A50A18D055FBB032.ips", "IPS32EEOF");
        CHECK(m.directLaunchPatchInstalled());
        std::string note;
        CHECK(m.restore("custom_level.PAK", err, note));
    }

    // nome automatico (target vuoto): per un livello quello che ha dentro, altrimenti suggerito o remoto
    {
        const RemoteFile* custom = byName(httpList, "Custom_Level.pak");
        RemoteFile renamed = *custom;
        renamed.name = "Il mio livello v2.pak";  // nome qualsiasi sulla sorgente
        std::string as;
        CHECK(m.install(renamed, "", "PC", noProgress, err, &as) && as == "Custom_Level.pak");
        CHECK(util::fileExists(m.modDir() + "/Custom_Level.pak") && !util::fileExists(m.modDir() + "/Il mio livello v2.pak.part"));
        CHECK(m.find("Custom_Level.pak") && m.find("Custom_Level.pak")->remote == "Il mio livello v2.pak");
        std::string note;
        CHECK(m.restore("Custom_Level.pak", err, note));
        m.setOriginals({"CUSTOM_LEVEL.pak", "update.pak"});
        CHECK(m.install(*custom, "", "PC", noProgress, err, &as) && as == "CUSTOM_LEVEL.pak");  // grafia dell'elenco
        CHECK(m.restore("CUSTOM_LEVEL.pak", err, note));
        // file che non e' un livello: l'ultima scelta fatta per lui (qui L102_Jungle.pak), altrimenti il suo nome
        CHECK(m.rememberedTarget("Nome con spazi.pak") == "L102_Jungle.pak");
        CHECK(m.install(*byName(httpList, "Nome con spazi.pak"), "", "PC", noProgress, err, &as) && as == "L102_Jungle.pak");
        CHECK(m.restore("L102_Jungle.pak", err, note));
        RemoteFile fresh = *byName(httpList, "Nome con spazi.pak");
        fresh.name = "Mai visto.pak";
        CHECK(m.install(fresh, "", "PC", noProgress, err, &as) && as == "Mai visto.pak");
        CHECK(m.restore("Mai visto.pak", err, note));
        CHECK(m.install(*byName(man, "custom_v2.pak"), "", "PC", noProgress, err, &as) && as == "L101_NSanityBeach.pak");  // dal manifest
        CHECK(m.restore("L101_NSanityBeach.pak", err, note));
        m.setOriginals({});
    }

    // download interrotti
    spit(m.modDir() + "/vecchio.pak.part", "x");
    CHECK(m.cleanupPartials() == 1 && !util::fileExists(m.modDir() + "/vecchio.pak.part"));

    // ---------- livello nuovo: l'app crea update.pak (pak.cpp + Manager::registerLevel)
    if (argc > 3) {
        const std::string pakDir = argv[3];  // archivi creati da tests/crea_pak_prova.py
        pak::Archive base;
        CHECK(pak::read(pakDir + "/base_update.pak", base, err) && base.version == 12 && base.entries.size() == 6);
        int compressed = 0;
        for (const pak::Entry& e : base.entries) {
            compressed += e.compression != 0;
            CHECK(e.hash == pak::hashPath(e.path));
        }
        CHECK(compressed == 5);
        std::string data;
        CHECK(!pak::extract(base, base.entries[0].compression ? base.entries[0] : base.entries[1], data, err) ||
              base.entries[0].compression == 0);  // i file compressi non si estraggono
        CHECK(pak::write(base, pakDir + "/riscritto.pak", err));  // copia identica (verificata da verifica_pak.py)

        Manager mr(sd + "/reg", cfg);
        CHECK(mr.load(err, warn));
        util::mkdirs(mr.modDir());
        spit(mr.modDir() + "/Custom_Level.pak", slurp(pakDir + "/Custom_Level.pak"));
        bool reg = true;
        CHECK(!mr.registerLevel("Custom_Level.pak", reg, err) && !reg && err.find("update.pak originale") != std::string::npos);
        util::mkdirs(mr.appDir() + "/originali");
        spit(mr.originalUpdatePath(), slurp(pakDir + "/base_update.pak"));
        spit(mr.modDir() + "/update.pak", "MOD-UPDATE-A-MANO");  // un update.pak gia' presente: va in backup
        CHECK(mr.registerLevel("Custom_Level.pak", reg, err) && reg);
        CHECK(mr.find("update.pak") && mr.find("update.pak")->source == "generato" &&
              mr.find("update.pak")->backup == "update.pak" && mr.find("update.pak")->remote == "registrazione di Custom_Level.pak");
        CHECK(slurp(mr.backupDir() + "/update.pak") == "MOD-UPDATE-A-MANO");
        CHECK(!util::fileExists(mr.modDir() + "/update.pak.part"));
        pak::Archive merged;
        CHECK(pak::read(mr.modDir() + "/update.pak", merged, err) && merged.entries.size() == 7);
        for (const pak::Entry& e : merged.entries)
            if (e.path == "maps/crash1/custom_level/custom_level_zoneinfo.igz") {
                CHECK(pak::extract(merged, e, data, err) && data.size() == 2600 && data.compare(0, 11, "zone-nuova:") == 0);
            }
        spit(pakDir + "/unito.pak", slurp(mr.modDir() + "/update.pak"));  // controllato da verifica_pak.py
        // seconda registrazione: sostituisce la nostra, il backup resta quello di prima
        CHECK(mr.registerLevel("Custom_Level.pak", reg, err) && reg && mr.find("update.pak")->backup == "update.pak");
        // un .pak senza file update/: niente da registrare
        CHECK(mr.registerLevel("update.pak", reg, err) && !reg);
        // il gioco non ha update.pak: file vuoto come originale, update.pak con la sola registrazione
        spit(mr.originalUpdatePath(), "");
        CHECK(mr.registerLevel("Custom_Level.pak", reg, err) && reg);
        CHECK(pak::read(mr.modDir() + "/update.pak", merged, err) && merged.entries.size() == 2);
        std::string note;
        CHECK(mr.restore("update.pak", err, note) && slurp(mr.modDir() + "/update.pak") == "MOD-UPDATE-A-MANO");
    }

    net::shutdown();
    printf("%d controlli superati, %d falliti\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
