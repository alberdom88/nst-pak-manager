// core.cpp
#include "core.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "cJSON.h"
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
        "      \"name\": \"Google Drive\",\n"
        "      \"type\": \"gdrive\",\n"
        "      \"folder\": \"https://drive.google.com/drive/folders/ID_DELLA_CARTELLA\",\n"
        "      \"api_key\": \"LA_TUA_CHIAVE_API\"\n"
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

    const cJSON* sources = cJSON_GetObjectItemCaseSensitive(root, "sources");
    const cJSON* s;
    cJSON_ArrayForEach(s, sources) {
        if (!cJSON_IsObject(s)) continue;
        SourceConfig sc;
        sc.type = util::toLower(jsonString(s, "type"));
        sc.url = jsonString(s, "url");
        sc.folder = jsonString(s, "folder");
        sc.apiKey = jsonString(s, "api_key");
        std::string apiBase = jsonString(s, "api_base");
        if (!apiBase.empty()) sc.apiBase = apiBase;
        if (sc.type.empty()) sc.type = sc.folder.empty() ? "http" : "gdrive";
        sc.name = jsonString(s, "name");
        if (sc.name.empty()) sc.name = sc.type == "gdrive" ? "Google Drive" : sc.url;
        if (sc.type != "http" && sc.type != "gdrive") {
            cJSON_Delete(root);
            err = "sorgente '" + sc.name + "': type deve essere \"http\" o \"gdrive\"";
            return false;
        }
        if (sc.type == "http" && sc.url.empty()) {
            cJSON_Delete(root);
            err = "sorgente '" + sc.name + "': manca \"url\"";
            return false;
        }
        if (sc.type == "gdrive" && (sc.folder.empty() || sc.apiKey.empty())) {
            cJSON_Delete(root);
            err = "sorgente '" + sc.name + "': servono \"folder\" e \"api_key\"";
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

std::string driveFolderId(const std::string& s) {
    size_t p = s.find("folders/");
    if (p != std::string::npos) {
        std::string rest = s.substr(p + 8);
        return rest.substr(0, rest.find_first_of("/?#&"));
    }
    p = s.find("id=");
    if (p != std::string::npos) {
        std::string rest = s.substr(p + 3);
        return rest.substr(0, rest.find_first_of("&#"));
    }
    size_t a = s.find_first_not_of(" \t");
    size_t b = s.find_last_not_of(" \t");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
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

static bool listDrive(const SourceConfig& src, std::vector<RemoteFile>& out, std::string& err) {
    std::string id = driveFolderId(src.folder);
    if (id.empty()) {
        err = "ID della cartella Drive non valido";
        return false;
    }
    std::string pageToken;
    for (int page = 0; page < 50; page++) {
        std::string url = src.apiBase + "/drive/v3/files?q=" +
                          util::urlEncode("'" + id + "' in parents and trashed = false") +
                          "&fields=" + util::urlEncode("nextPageToken,files(id,name,size,mimeType)") +
                          "&pageSize=1000&supportsAllDrives=true&includeItemsFromAllDrives=true&key=" +
                          util::urlEncode(src.apiKey);
        if (!pageToken.empty()) url += "&pageToken=" + util::urlEncode(pageToken);

        std::string body;
        long status = 0;
        if (!net::get(url, body, status, err)) return false;
        cJSON* root = cJSON_Parse(body.c_str());
        if (!root) {
            err = "risposta di Google Drive non valida";
            return false;
        }
        const cJSON* error = cJSON_GetObjectItemCaseSensitive(root, "error");
        if (error || status >= 400) {
            std::string msg = error ? jsonString(error, "message") : "";
            char buf[64];
            snprintf(buf, sizeof(buf), "Google Drive HTTP %ld", status);
            err = std::string(buf) + (msg.empty() ? "" : ": " + msg);
            cJSON_Delete(root);
            return false;
        }
        const cJSON* files = cJSON_GetObjectItemCaseSensitive(root, "files");
        const cJSON* f;
        cJSON_ArrayForEach(f, files) {
            if (jsonString(f, "mimeType") == "application/vnd.google-apps.folder") continue;
            RemoteFile rf;
            rf.name = jsonString(f, "name");
            if (!isValidPakName(rf.name)) continue;
            std::string fid = jsonString(f, "id");
            if (fid.empty()) continue;
            rf.url = src.apiBase + "/drive/v3/files/" + util::urlEncode(fid) +
                     "?alt=media&supportsAllDrives=true&key=" + util::urlEncode(src.apiKey);
            std::string size = jsonString(f, "size");
            if (!size.empty()) {
                rf.size = strtoull(size.c_str(), nullptr, 10);
                rf.sizeKnown = rf.size > 0;
            }
            addUnique(out, rf);
        }
        pageToken = jsonString(root, "nextPageToken");
        cJSON_Delete(root);
        if (pageToken.empty()) return true;
    }
    return true;
}

bool listSource(const SourceConfig& src, std::vector<RemoteFile>& out, std::string& err) {
    out.clear();
    bool ok = src.type == "gdrive" ? listDrive(src, out, err) : listHttp(src, out, err);
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
    modDir_ = root + dir;
    appDir_ = appDirFor(root);
    backupDir_ = appDir_ + "/backup/" + cfg.titleId;
    statePath_ = appDir_ + "/state-" + cfg.titleId + ".json";
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

bool Manager::install(const RemoteFile& file, const std::string& target, const std::string& sourceName,
                      const net::Progress& progress, std::string& err) {
    if (!isValidPakName(target)) {
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

    const std::string dest = util::joinPath(modDir_, target);
    const std::string tmp = dest + ".part";
    util::removeFile(tmp);

    // 1. Scarica in un file temporaneo: se qualcosa va storto non si tocca nulla
    uint64_t written = 0;
    if (!net::download(file.url, tmp, file.sizeKnown ? file.size : 0, progress, written, err))
        return false;
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
        entry->remote = file.name;
        entry->source = sourceName;
        entry->size = written;
    } else {
        InstalledFile f;
        f.name = target;
        f.remote = file.name;
        f.source = sourceName;
        f.size = written;
        f.backup = backupName;
        installed_.push_back(f);
    }
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
