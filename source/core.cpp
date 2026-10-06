// core.cpp
#include "core.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "cJSON.h"
#include "mega.hpp"
#include "pak.hpp"
#include "util.hpp"

static const char PAK_MAGIC[4] = {'I', 'G', 'A', 0x1A};  // 0x1A414749 little-endian
static const uint64_t FREE_SPACE_MARGIN = 16ull * 1024 * 1024;

// ---------------------------------------------------------------- config

std::string defaultConfigJson() {
    return
        "{\n"
        "  \"title_id\": \"0100D1B006744000\",\n"
        "  \"sources\": [\n"
        "    {\n"
        "      \"name\": \"PC di casa\",\n"
        "      \"type\": \"http\",\n"
        "      \"url\": \"http://192.168.1.50:8000/\"\n"
        "    },\n"
        "    {\n"
        "      \"name\": \"MEGA\",\n"
        "      \"type\": \"mega\",\n"
        "      \"url\": \"https://mega.nz/folder/ID_CARTELLA#CHIAVE\"\n"
        "    }\n"
        "  ]\n"
        "}\n";
}

static std::string jsonString(const cJSON* obj, const char* key) {
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return (cJSON_IsString(v) && v->valuestring) ? v->valuestring : "";
}

static bool isHex16(const std::string& s) {
    if (s.size() != 16) return false;
    for (char c : s)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
    return true;
}

static std::string upper(std::string s) {
    for (char& c : s)
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    return s;
}

bool parseConfig(const std::string& json, Config& cfg, std::string& err) {
    cJSON* root = cJSON_Parse(json.c_str());
    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        err = "config.json non valido (controlla virgole e virgolette)";
        return false;
    }
    Config out;
    std::string tid = jsonString(root, "title_id");
    if (!tid.empty()) {
        if (!isHex16(tid)) {
            cJSON_Delete(root);
            err = "title_id deve avere 16 cifre esadecimali";
            return false;
        }
        out.titleId = upper(tid);
    }
    std::string modDir = jsonString(root, "mod_dir");
    if (!modDir.empty()) out.modDir = modDir;
    out.caFile = jsonString(root, "ca_file");
    const cJSON* launch = cJSON_GetObjectItemCaseSensitive(root, "launch_args");
    if (cJSON_IsString(launch) && launch->valuestring) out.launchArgs = launch->valuestring;

    const cJSON* sources = cJSON_GetObjectItemCaseSensitive(root, "sources");
    const cJSON* s;
    cJSON_ArrayForEach(s, sources) {
        if (!cJSON_IsObject(s)) continue;
        SourceConfig sc;
        sc.type = util::toLower(jsonString(s, "type"));
        sc.url = jsonString(s, "url");
        sc.apiBase = jsonString(s, "api_base");
        if (sc.type.empty()) {
            std::string lower = util::toLower(sc.url);
            sc.type = lower.find("mega.nz") != std::string::npos || lower.find("mega.co.nz") != std::string::npos
                          ? "mega" : "http";
        }
        sc.name = jsonString(s, "name");
        if (sc.name.empty()) sc.name = sc.type == "mega" ? "MEGA" : sc.url;
        if (sc.type == "gdrive") {
            cJSON_Delete(root);
            err = "sorgente '" + sc.name + "': Google Drive non e' piu' supportato, usa \"mega\" o \"http\"";
            return false;
        }
        if (sc.type != "http" && sc.type != "mega") {
            cJSON_Delete(root);
            err = "sorgente '" + sc.name + "': type deve essere \"http\" o \"mega\"";
            return false;
        }
        if (sc.url.empty()) {
            cJSON_Delete(root);
            err = "sorgente '" + sc.name + "': manca \"url\"";
            return false;
        }
        out.sources.push_back(sc);
    }
    cJSON_Delete(root);
    if (out.sources.empty()) {
        err = "nessuna sorgente in \"sources\"";
        return false;
    }
    cfg = out;
    return true;
}

// ---------------------------------------------------------------- remote listing

bool isValidPakName(const std::string& name) {
    if (name.empty() || name.size() > 128) return false;
    if (!util::endsWithCI(name, ".pak") || name.size() == 4) return false;
    if (name[0] == '.' || name.find("..") != std::string::npos) return false;
    for (unsigned char c : name) {
        if (c < 0x20 || c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' ||
            c == '<' || c == '>' || c == '|')
            return false;
    }
    return true;
}

static std::string stripQuery(const std::string& url) {
    size_t p = url.find_first_of("?#");
    return p == std::string::npos ? url : url.substr(0, p);
}

static std::string lastSegment(const std::string& path) {
    size_t p = path.find_last_of('/');
    return p == std::string::npos ? path : path.substr(p + 1);
}

std::string resolveUrl(const std::string& base, const std::string& href) {
    if (href.find("://") != std::string::npos) return href;
    size_t schemeEnd = base.find("://");
    if (schemeEnd == std::string::npos) return href;
    if (href.compare(0, 2, "//") == 0) return base.substr(0, schemeEnd + 1) + href;
    size_t pathStart = base.find('/', schemeEnd + 3);
    std::string origin = pathStart == std::string::npos ? base : base.substr(0, pathStart);
    origin = stripQuery(origin);
    if (!href.empty() && href[0] == '/') return origin + href;
    std::string dir;
    if (pathStart == std::string::npos) {
        dir = origin + "/";
    } else {
        std::string path = stripQuery(base.substr(pathStart));
        dir = origin + path.substr(0, path.find_last_of('/') + 1);
    }
    std::string rel = href;
    while (rel.compare(0, 2, "./") == 0) rel = rel.substr(2);
    return dir + rel;
}

static std::string decodeEntities(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '&') {
            if (s.compare(i, 5, "&amp;") == 0) { out += '&'; i += 4; continue; }
            if (s.compare(i, 6, "&quot;") == 0) { out += '"'; i += 5; continue; }
            if (s.compare(i, 5, "&#39;") == 0) { out += '\''; i += 4; continue; }
            if (s.compare(i, 4, "&lt;") == 0) { out += '<'; i += 3; continue; }
            if (s.compare(i, 4, "&gt;") == 0) { out += '>'; i += 3; continue; }
        }
        out += s[i];
    }
    return out;
}

static bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

static void addUnique(std::vector<RemoteFile>& out, const RemoteFile& f) {
    for (const RemoteFile& e : out)
        if (util::toLower(e.name) == util::toLower(f.name)) return;
    out.push_back(f);
}

void parseDirectoryListing(const std::string& html, const std::string& baseUrl,
                           std::vector<RemoteFile>& out) {
    std::string lower = util::toLower(html);
    size_t pos = 0;
    while ((pos = lower.find("href", pos)) != std::string::npos) {
        size_t p = pos + 4;
        while (p < html.size() && isSpace(html[p])) p++;
        if (p >= html.size() || html[p] != '=') {
            pos = p;
            continue;
        }
        p++;
        while (p < html.size() && isSpace(html[p])) p++;
        if (p >= html.size()) break;
        std::string href;
        char q = html[p];
        if (q == '"' || q == '\'') {
            size_t e = html.find(q, p + 1);
            if (e == std::string::npos) break;
            href = html.substr(p + 1, e - p - 1);
            pos = e + 1;
        } else {
            size_t e = p;
            while (e < html.size() && !isSpace(html[e]) && html[e] != '>') e++;
            href = html.substr(p, e - p);
            pos = e;
        }
        href = decodeEntities(href);
        std::string path = stripQuery(href);
        if (!util::endsWithCI(path, ".pak")) continue;
        RemoteFile f;
        f.name = util::urlDecode(lastSegment(path));
        if (!isValidPakName(f.name)) continue;
        f.url = resolveUrl(baseUrl, href);
        addUnique(out, f);
    }
}

bool parseManifest(const std::string& json, const std::string& manifestUrl,
                   std::vector<RemoteFile>& out, std::string& err) {
    cJSON* root = cJSON_Parse(json.c_str());
    if (!root) {
        err = "manifest JSON non valido";
        return false;
    }
    const cJSON* files = cJSON_IsArray(root) ? root : cJSON_GetObjectItemCaseSensitive(root, "files");
    if (!cJSON_IsArray(files)) {
        cJSON_Delete(root);
        err = "il manifest deve contenere un elenco \"files\"";
        return false;
    }
    const cJSON* item;
    cJSON_ArrayForEach(item, files) {
        RemoteFile f;
        if (cJSON_IsString(item)) {
            f.name = item->valuestring;
        } else if (cJSON_IsObject(item)) {
            f.name = jsonString(item, "name");
            f.url = jsonString(item, "url");
            f.target = jsonString(item, "target");
            if (!f.target.empty() && !isValidPakName(f.target)) f.target.clear();
            const cJSON* size = cJSON_GetObjectItemCaseSensitive(item, "size");
            if (cJSON_IsNumber(size) && size->valuedouble > 0) {
                f.size = (uint64_t)size->valuedouble;
                f.sizeKnown = true;
            }
        } else {
            continue;
        }
        if (f.name.empty() && !f.url.empty()) f.name = util::urlDecode(lastSegment(stripQuery(f.url)));
        if (!isValidPakName(f.name)) continue;
        f.url = f.url.empty() ? resolveUrl(manifestUrl, util::urlEncode(f.name)) : resolveUrl(manifestUrl, f.url);
        addUnique(out, f);
    }
    cJSON_Delete(root);
    return true;
}

std::vector<std::string> parseOriginals(const std::string& text) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::string l = text.substr(start, end - start);
        start = end + 1;
        while (!l.empty() && (l.back() == '\r' || l.back() == ' ' || l.back() == '\t')) l.pop_back();
        size_t a = l.find_first_not_of(" \t");
        if (a == std::string::npos || l[a] == '#') continue;
        l = l.substr(a);
        if (l.size() >= 3 && (unsigned char)l[0] == 0xEF && (unsigned char)l[1] == 0xBB && (unsigned char)l[2] == 0xBF)
            l = l.substr(3);  // BOM di Blocco note
        size_t slash = l.find_last_of("/\\");
        if (slash != std::string::npos) l = l.substr(slash + 1);
        if (!isValidPakName(l)) continue;
        bool dup = false;
        for (const std::string& o : out) dup = dup || util::toLower(o) == util::toLower(l);
        if (!dup) out.push_back(l);
    }
    std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) {
        return util::toLower(a) < util::toLower(b);
    });
    return out;
}

// Livelli di un .pak: per ognuno nome (L112_RoadToNowhere) e identificativo (crash1/l112_.../l112_...)
static bool pakLevels(const std::string& path, std::vector<std::string>& names, std::vector<std::string>& ids);

bool pakLevelNames(const std::string& path, std::vector<std::string>& out) {
    std::vector<std::string> ids;
    return pakLevels(path, out, ids);
}

bool pakLevelIds(const std::string& path, std::vector<std::string>& out) {
    std::vector<std::string> names;
    return pakLevels(path, names, out);
}

std::string launchArguments(const std::string& pattern, const std::string& levelId) {
    std::string out = pattern;
    const std::string key = "{livello}";
    size_t pos;
    while ((pos = out.find(key)) != std::string::npos) out.replace(pos, key.size(), levelId);
    return out;
}

const char* const DIRECT_LAUNCH_PATCH_DIR = "/atmosphere/exefs_patches/nst_avvio_livello";

std::string debugXml(const std::string& levelId) {
    return "<?xml version=\"1.0\" encoding=\"ascii\"?>\n"
           "<!-- scritto da NST Pak Manager: avvio diretto nel livello -->\n"
           "<config>\n"
           "\t<MAP filename=\"" + levelId + "\"/>\n"
           "</config>\n";
}

std::string debugXmlLevel(const std::string& xml) {
    size_t p = xml.find("<MAP");
    while (p != std::string::npos) {
        size_t end = xml.find('>', p);
        if (end == std::string::npos) return "";
        std::string tag = xml.substr(p, end - p);
        size_t a = tag.find("filename");
        if (a != std::string::npos) {
            size_t q = tag.find_first_of("\"'", a);
            if (q != std::string::npos) {
                size_t q2 = tag.find(tag[q], q + 1);
                if (q2 != std::string::npos) return tag.substr(q + 1, q2 - q - 1);
            }
        }
        p = xml.find("<MAP", end);
    }
    return "";
}

static bool pakLevels(const std::string& path, std::vector<std::string>& out, std::vector<std::string>& ids) {
    out.clear();
    ids.clear();
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    unsigned char h[0x38];
    bool ok = fread(h, 1, sizeof(h), f) == sizeof(h) && memcmp(h, PAK_MAGIC, 4) == 0;
    uint32_t count = 0, tableSize = 0;
    uint64_t tableOff = 0, fileSize = 0;
    if (ok) {
        memcpy(&count, h + 0x0C, 4);
        memcpy(&tableOff, h + 0x28, 8);
        memcpy(&tableSize, h + 0x30, 4);
        ok = fseek(f, 0, SEEK_END) == 0;
        long end = ftell(f);
        fileSize = end > 0 ? (uint64_t)end : 0;
        ok = ok && count > 0 && tableSize <= 64u * 1024 * 1024 && (uint64_t)count * 4 <= tableSize &&
             tableOff + tableSize <= fileSize;
    }
    std::string table;
    if (ok) {
        table.resize(tableSize);
        ok = fseek(f, (long)tableOff, SEEK_SET) == 0 && fread(&table[0], 1, tableSize, f) == tableSize;
    }
    fclose(f);
    if (!ok) return false;

    static const std::string prefix = "packages/generated/maps/";
    static const std::string suffix = "_pkg.igz";
    for (uint32_t i = 0; i < count; i++) {
        uint32_t rel;
        memcpy(&rel, table.data() + 4 * i, 4);
        size_t a = rel;
        size_t b = a < table.size() ? table.find('\0', a) : std::string::npos;  // percorso completo
        size_t c = b == std::string::npos ? std::string::npos : table.find('\0', b + 1);  // percorso breve
        if (c == std::string::npos) return false;
        std::string shortPath = util::toLower(table.substr(b + 1, c - b - 1));
        std::string original = table.substr(b + 1, c - b - 1);
        if (shortPath.compare(0, prefix.size(), prefix) != 0 || !util::endsWithCI(shortPath, suffix)) continue;
        std::string rest = original.substr(prefix.size());  // <gioco>/<L>/<L>_pkg.igz
        size_t s1 = rest.find('/');
        size_t s2 = s1 == std::string::npos ? s1 : rest.find('/', s1 + 1);
        if (s2 == std::string::npos || rest.find('/', s2 + 1) != std::string::npos) continue;
        std::string file = rest.substr(s2 + 1);
        std::string level = file.substr(0, file.size() - suffix.size());
        out.push_back(level);
        ids.push_back(util::toLower(rest.substr(0, s2 + 1) + level));
    }
    return true;
}

bool loadOriginals(const std::string& path, std::vector<std::string>& out) {
    std::string text;
    if (!util::readFile(path, text)) return false;
    out = parseOriginals(text);
    return true;
}

static bool listHttp(const SourceConfig& src, std::vector<RemoteFile>& out, std::string& err) {
    std::string body, effective;
    long status = 0;
    if (!net::get(src.url, body, status, err, &effective)) return false;
    if (status >= 400) {
        char buf[64];
        snprintf(buf, sizeof(buf), "il server ha risposto HTTP %ld", status);
        err = buf;
        return false;
    }
    if (util::endsWithCI(stripQuery(src.url), ".json")) {
        if (!parseManifest(body, effective, out, err)) return false;
    } else {
        parseDirectoryListing(body, effective, out);
    }
    // Le dimensioni servono per controllare spazio libero e download completi
    std::vector<std::string> urls;
    std::vector<size_t> idx;
    for (size_t i = 0; i < out.size() && urls.size() < 300; i++) {
        if (out[i].sizeKnown) continue;
        urls.push_back(out[i].url);
        idx.push_back(i);
    }
    std::vector<int64_t> sizes = net::contentLengths(urls);
    for (size_t k = 0; k < idx.size(); k++) {
        if (sizes[k] > 0) {
            out[idx[k]].size = (uint64_t)sizes[k];
            out[idx[k]].sizeKnown = true;
        }
    }
    return true;
}

bool listSource(const SourceConfig& src, std::vector<RemoteFile>& out, std::string& err) {
    out.clear();
    bool ok = src.type == "mega" ? mega::list(src, out, err) : listHttp(src, out, err);
    if (!ok) return false;
    std::sort(out.begin(), out.end(), [](const RemoteFile& a, const RemoteFile& b) {
        return util::toLower(a.name) < util::toLower(b.name);
    });
    return true;
}

// ---------------------------------------------------------------- manager

std::string appDirFor(const std::string& root) { return root + "/switch/nst-pak-manager"; }

Manager::Manager(const std::string& root, const Config& cfg) {
    std::string dir = cfg.modDir;
    size_t p = dir.find("{title_id}");
    if (p != std::string::npos) dir.replace(p, 10, cfg.titleId);
    if (dir.empty() || dir[0] != '/') dir = "/" + dir;
    while (dir.size() > 1 && dir.back() == '/') dir.pop_back();
    root_ = root;
    modDir_ = root + dir;
    appDir_ = appDirFor(root);
    backupDir_ = appDir_ + "/backup/" + cfg.titleId;
    statePath_ = appDir_ + "/state-" + cfg.titleId + ".json";
}

std::string Manager::romfsDir() const {
    const std::string suffix = "/archives";
    if (modDir_.size() > suffix.size() && util::endsWithCI(modDir_, suffix))
        return modDir_.substr(0, modDir_.size() - suffix.size());
    return modDir_;
}

bool Manager::setDirectLaunch(const std::string& levelId, std::string& err) {
    if (!util::mkdirs(romfsDir()) || !util::writeFileAtomic(directLaunchPath(), debugXml(levelId))) {
        err = "impossibile scrivere " + directLaunchPath();
        return false;
    }
    return true;
}

void Manager::clearDirectLaunch() {
    if (util::fileExists(directLaunchPath())) util::removeFile(directLaunchPath());
}

std::string Manager::directLaunchLevel() const {
    std::string text;
    if (!util::readFile(directLaunchPath(), text)) return "";
    return debugXmlLevel(text);
}

bool Manager::directLaunchPatchInstalled() const {
    for (const std::string& f : util::listFiles(root_ + DIRECT_LAUNCH_PATCH_DIR))
        if (util::endsWithCI(f, ".ips")) return true;
    return false;
}

const InstalledFile* Manager::find(const std::string& name) const {
    for (const InstalledFile& f : installed_)
        if (f.name == name) return &f;
    return nullptr;
}

std::vector<const InstalledFile*> Manager::installedFrom(const std::string& remoteName) const {
    std::vector<const InstalledFile*> out;
    for (const InstalledFile& f : installed_)
        if (f.remote == remoteName) out.push_back(&f);
    return out;
}

std::string Manager::rememberedTarget(const std::string& remoteName) const {
    for (const auto& p : remembered_)
        if (p.first == remoteName) return p.second;
    return "";
}

std::string Manager::suggestTarget(const RemoteFile& file, const std::vector<std::string>& originals) const {
    std::string t = rememberedTarget(file.name);
    if (t.empty()) t = file.target;
    if (t.empty()) t = file.name;
    // Usa la grafia esatta dell'elenco degli originali, se c'e'
    for (const std::string& o : originals)
        if (util::toLower(o) == util::toLower(t)) return o;
    // Una scelta esplicita (precedente o dal manifest) vale anche se non e' in elenco
    if (t != file.name || !rememberedTarget(file.name).empty()) return t;
    return "";
}

bool Manager::existsInModDir(const std::string& name) const {
    return util::fileExists(util::joinPath(modDir_, name));
}

std::vector<std::string> Manager::externalFiles() const {
    std::vector<std::string> out;
    for (const std::string& n : util::listFiles(modDir_))
        if (util::endsWithCI(n, ".pak") && !find(n)) out.push_back(n);
    std::sort(out.begin(), out.end());
    return out;
}

int Manager::cleanupPartials() {
    int n = 0;
    for (const std::string& name : util::listFiles(modDir_))
        if (util::endsWithCI(name, ".pak.part") && util::removeFile(util::joinPath(modDir_, name))) n++;
    return n;
}

bool Manager::load(std::string& err, std::string& warning) {
    warning.clear();
    installed_.clear();
    remembered_.clear();
    if (!util::mkdirs(appDir_)) {
        err = "impossibile creare " + appDir_;
        return false;
    }
    std::string path = statePath_;
    if (!util::fileExists(path) && util::fileExists(path + ".tmp")) path += ".tmp";
    if (!util::fileExists(path)) return true;

    std::string data;
    cJSON* root = nullptr;
    if (util::readFile(path, data)) root = cJSON_Parse(data.c_str());
    const cJSON* list = root ? cJSON_GetObjectItemCaseSensitive(root, "installed") : nullptr;
    if (!root || !cJSON_IsArray(list)) {
        cJSON_Delete(root);
        std::string aside = statePath_ + ".corrotto";
        for (int i = 1; util::fileExists(aside); i++) aside = statePath_ + ".corrotto" + std::to_string(i);
        rename(path.c_str(), aside.c_str());
        warning = "stato illeggibile, spostato in " + aside +
                  ". I backup restano in " + backupDir_;
        return true;
    }
    const cJSON* item;
    cJSON_ArrayForEach(item, list) {
        InstalledFile f;
        f.name = jsonString(item, "name");
        if (!isValidPakName(f.name)) continue;
        f.remote = jsonString(item, "remote");
        if (f.remote.empty()) f.remote = f.name;
        f.source = jsonString(item, "source");
        f.backup = jsonString(item, "backup");
        const cJSON* size = cJSON_GetObjectItemCaseSensitive(item, "size");
        if (cJSON_IsNumber(size)) f.size = (uint64_t)size->valuedouble;
        installed_.push_back(f);
    }
    const cJSON* targets = cJSON_GetObjectItemCaseSensitive(root, "targets");
    const cJSON* t;
    cJSON_ArrayForEach(t, targets) {
        if (cJSON_IsString(t) && t->string && isValidPakName(t->valuestring))
            remembered_.push_back({t->string, t->valuestring});
    }
    cJSON_Delete(root);
    return true;
}

bool Manager::save(std::string& err) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "version", 1);
    cJSON* list = cJSON_AddArrayToObject(root, "installed");
    for (const InstalledFile& f : installed_) {
        cJSON* o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", f.name.c_str());
        cJSON_AddStringToObject(o, "remote", f.remote.c_str());
        cJSON_AddStringToObject(o, "source", f.source.c_str());
        cJSON_AddNumberToObject(o, "size", (double)f.size);
        cJSON_AddStringToObject(o, "backup", f.backup.c_str());
        cJSON_AddItemToArray(list, o);
    }
    cJSON* targets = cJSON_AddObjectToObject(root, "targets");
    for (const auto& p : remembered_) cJSON_AddStringToObject(targets, p.first.c_str(), p.second.c_str());
    char* text = cJSON_Print(root);
    cJSON_Delete(root);
    bool ok = text && util::mkdirs(appDir_) && util::writeFileAtomic(statePath_, text);
    cJSON_free(text);
    if (!ok) err = "impossibile salvare " + statePath_;
    return ok;
}

std::string Manager::uniqueBackupName(const std::string& name) const {
    std::string candidate = name;
    for (int i = 1; util::fileExists(util::joinPath(backupDir_, candidate)); i++)
        candidate = name + "." + std::to_string(i);
    return candidate;
}

static bool hasPakMagic(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    char magic[4] = {0};
    size_t n = fread(magic, 1, 4, f);
    fclose(f);
    return n == 4 && memcmp(magic, PAK_MAGIC, 4) == 0;
}

std::string Manager::autoTarget(const RemoteFile& file, const std::string& downloaded) const {
    // Un livello si installa solo con il nome che ha dentro (come lo cerca il gioco)
    std::vector<std::string> levels;
    if (pakLevelNames(downloaded, levels) && !levels.empty()) {
        std::string name = levels[0] + ".pak";
        for (const std::string& o : originals_)
            if (util::toLower(o) == util::toLower(name)) return o;  // grafia dell'originale
        return name;
    }
    std::string suggested = suggestTarget(file, originals_);
    return suggested.empty() ? file.name : suggested;
}

bool Manager::install(const RemoteFile& file, const std::string& wanted, const std::string& sourceName,
                      const net::Progress& progress, std::string& err, std::string* installedAs) {
    std::string target = wanted;
    if (!target.empty() && !isValidPakName(target)) {
        err = "nome dell'originale non valido: " + target;
        return false;
    }
    if (!util::mkdirs(modDir_) || !util::mkdirs(backupDir_)) {
        err = "impossibile creare le cartelle su SD";
        return false;
    }
    uint64_t freeBytes = 0;
    if (file.sizeKnown && util::freeSpace(modDir_, freeBytes) && freeBytes < file.size + FREE_SPACE_MARGIN) {
        err = "spazio insufficiente: servono " + util::formatSize(file.size) + ", liberi " +
              util::formatSize(freeBytes);
        return false;
    }

    // Nome automatico: il .part prende il nome del file remoto
    const std::string tmp = util::joinPath(modDir_, (target.empty() ? file.name : target) + ".part");
    util::removeFile(tmp);

    // 1. Scarica in un file temporaneo: se qualcosa va storto non si tocca nulla
    uint64_t written = 0;
    bool downloaded = file.megaNode.empty()
                          ? net::download(file.url, tmp, file.sizeKnown ? file.size : 0, progress, written, err)
                          : mega::download(file, tmp, progress, written, err);
    if (!downloaded) return false;
    if (file.sizeKnown && written != file.size) {
        util::removeFile(tmp);
        err = "download incompleto (" + util::formatSize(written) + " su " + util::formatSize(file.size) + ")";
        return false;
    }
    if (!hasPakMagic(tmp)) {
        util::removeFile(tmp);
        err = "il file scaricato non e' un .pak valido (link non diretto o pagina di errore?)";
        return false;
    }
    if (target.empty()) {
        target = autoTarget(file, tmp);
        if (!isValidPakName(target)) {
            util::removeFile(tmp);
            err = "nome non valido ricavato dal file: " + target;
            return false;
        }
    }
    if (installedAs) *installedAs = target;
    // Un livello si trova solo con il suo nome: rinominare il .pak non rinomina i file che contiene
    std::vector<std::string> levels;
    if (pakLevelNames(tmp, levels) && !levels.empty()) {
        std::string want = util::toLower(target.substr(0, target.size() - 4));
        bool match = false;
        for (const std::string& l : levels) match = match || util::toLower(l) == want;
        if (!match) {
            util::removeFile(tmp);
            err = "dentro c'e' il livello '" + levels[0] + "', non '" + target.substr(0, target.size() - 4) +
                  "': con questo nome il gioco non lo trova (schermo nero o crash). Nessuna modifica fatta.";
            return false;
        }
    }

    if (!place(tmp, target, file.name, sourceName, written, err)) return false;
    bool known = false;
    for (auto& p : remembered_)
        if (p.first == file.name) {
            p.second = target;
            known = true;
        }
    if (!known) remembered_.push_back({file.name, target});
    if (!save(err)) {
        err = "file installato, ma " + err;
        return false;
    }
    return true;
}

// Mette tmp al posto di target nella cartella mod (con il backup di un file non nostro)
bool Manager::place(const std::string& tmp, const std::string& target, const std::string& remoteName,
                    const std::string& sourceName, uint64_t written, std::string& err) {
    const std::string dest = util::joinPath(modDir_, target);
    if (!util::mkdirs(backupDir_)) {
        util::removeFile(tmp);
        err = "impossibile creare la cartella dei backup";
        return false;
    }
    // 2. Mette da parte il file esistente
    InstalledFile* entry = nullptr;
    for (InstalledFile& f : installed_)
        if (util::toLower(f.name) == util::toLower(target)) entry = &f;
    std::string backupName = entry ? entry->backup : "";
    bool newBackup = false;
    if (util::fileExists(dest)) {
        if (entry) {
            // E' una nostra installazione precedente: il backup originale e' gia' salvato
            if (!util::removeFile(dest)) {
                util::removeFile(tmp);
                err = "impossibile sostituire " + dest;
                return false;
            }
        } else {
            backupName = uniqueBackupName(target);
            if (!util::moveFile(dest, util::joinPath(backupDir_, backupName))) {
                util::removeFile(tmp);
                err = "impossibile fare il backup di " + target;
                return false;
            }
            newBackup = true;
        }
    }

    // 3. Mette al suo posto il file nuovo
    if (!util::moveFile(tmp, dest)) {
        if (newBackup) util::moveFile(util::joinPath(backupDir_, backupName), dest);
        util::removeFile(tmp);
        err = "impossibile spostare il file scaricato in " + modDir_;
        return false;
    }

    if (entry) {
        entry->remote = remoteName;
        entry->source = sourceName;
        entry->size = written;
    } else {
        InstalledFile f;
        f.name = target;
        f.remote = remoteName;
        f.source = sourceName;
        f.size = written;
        f.backup = backupName;
        installed_.push_back(f);
    }
    return true;
}

std::string Manager::originalUpdatePath() const { return appDir_ + "/originali/update.pak"; }

bool Manager::registerLevel(const std::string& pakName, bool& registered, std::string& err) {
    registered = false;
    pak::Archive level;
    if (!pak::read(util::joinPath(modDir_, pakName), level, err)) return false;
    std::vector<const pak::Entry*> files;
    for (const pak::Entry& e : level.entries)
        if (util::toLower(e.path).compare(0, 7, "update/") == 0 && e.path.size() > 7) files.push_back(&e);
    if (files.empty()) return true;  // livello con il nome di uno originale: niente da registrare

    pak::Archive update;
    uint64_t baseSize = 0;
    const std::string base = originalUpdatePath();
    if (!util::fileExists(base)) {
        err = "per un livello nuovo serve una copia dell'update.pak originale del gioco in " + base +
              " (dal dump RomFS: archives/update.pak). Se il gioco non ne ha uno, crea li' un file vuoto.";
        return false;
    }
    if (util::fileSize(base, baseSize) && baseSize > 0) {
        if (!pak::read(base, update, err)) return false;
    } else {
        update.version = level.version;  // il gioco non ha un update.pak: solo la registrazione
    }
    for (const pak::Entry* e : files) {
        std::string data;
        if (!pak::extract(level, *e, data, err)) return false;
        pak::put(update, e->path.substr(7), data);
    }

    const std::string tmp = util::joinPath(modDir_, "update.pak.part");
    util::removeFile(tmp);
    if (!pak::write(update, tmp, err)) return false;
    uint64_t written = 0;
    util::fileSize(tmp, written);
    if (!place(tmp, "update.pak", "registrazione di " + pakName, "generato", written, err)) return false;
    if (!save(err)) {
        err = "update.pak creato, ma " + err;
        return false;
    }
    registered = true;
    return true;
}

bool Manager::restore(const std::string& name, std::string& err, std::string& note) {
    note.clear();
    size_t idx = installed_.size();
    for (size_t i = 0; i < installed_.size(); i++)
        if (installed_[i].name == name) idx = i;
    if (idx == installed_.size()) {
        err = name + " non e' stato installato da questa app";
        return false;
    }
    const InstalledFile& entry = installed_[idx];
    const std::string dest = util::joinPath(modDir_, name);
    if (!util::removeFile(dest)) {
        err = "impossibile eliminare " + dest;
        return false;
    }
    if (!entry.backup.empty()) {
        std::string backupPath = util::joinPath(backupDir_, entry.backup);
        if (util::fileExists(backupPath)) {
            if (!util::moveFile(backupPath, dest)) {
                err = "impossibile rimettere il backup " + backupPath;
                return false;
            }
        } else {
            note = "backup di " + name + " non trovato: file solo rimosso";
        }
    }
    installed_.erase(installed_.begin() + (long)idx);
    return save(err);
}
