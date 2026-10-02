// util.cpp
#include "util.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifdef __SWITCH__
#include <switch.h>
#else
#include <sys/statvfs.h>
#endif

namespace util {

std::string joinPath(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    bool aSlash = a.back() == '/';
    bool bSlash = b.front() == '/';
    if (aSlash && bSlash) return a + b.substr(1);
    if (aSlash || bSlash) return a + b;
    return a + "/" + b;
}

bool fileExists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

bool dirExists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool fileSize(const std::string& path, uint64_t& out) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return false;
    out = (uint64_t)st.st_size;
    return true;
}

bool mkdirs(const std::string& path) {
    if (path.empty()) return false;
    if (dirExists(path)) return true;
    // Salta l'eventuale prefisso dispositivo ("sdmc:") e la radice
    size_t pos = path.find('/');
    if (pos == std::string::npos) return false;
    while (true) {
        pos = path.find('/', pos + 1);
        std::string part = path.substr(0, pos);
        if (!part.empty() && !dirExists(part)) {
            if (mkdir(part.c_str(), 0777) != 0 && errno != EEXIST) return false;
        }
        if (pos == std::string::npos) break;
    }
    return dirExists(path);
}

bool removeFile(const std::string& path) {
    if (!fileExists(path)) return true;
    return remove(path.c_str()) == 0;
}

bool moveFile(const std::string& from, const std::string& to) {
    if (fileExists(to)) return false;
    return rename(from.c_str(), to.c_str()) == 0;
}

bool readFile(const std::string& path, std::string& out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    out.clear();
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    bool ok = !ferror(f);
    fclose(f);
    return ok;
}

bool writeFileAtomic(const std::string& path, const std::string& data) {
    std::string tmp = path + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return false;
    bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
    ok = (fclose(f) == 0) && ok;
    if (!ok) {
        remove(tmp.c_str());
        return false;
    }
    // Sulla Switch rename() non sovrascrive: prima si elimina il vecchio file
    if (fileExists(path) && remove(path.c_str()) != 0) {
        remove(tmp.c_str());
        return false;
    }
    return rename(tmp.c_str(), path.c_str()) == 0;
}

std::vector<std::string> listFiles(const std::string& dir) {
    std::vector<std::string> out;
    DIR* d = opendir(dir.c_str());
    if (!d) return out;
    struct dirent* e;
    while ((e = readdir(d)) != nullptr) {
        std::string name = e->d_name;
        if (name == "." || name == "..") continue;
        if (fileExists(joinPath(dir, name))) out.push_back(name);
    }
    closedir(d);
    return out;
}

bool freeSpace(const std::string& path, uint64_t& out) {
#ifdef __SWITCH__
    (void)path;
    FsFileSystem sd;
    if (R_FAILED(fsOpenSdCardFileSystem(&sd))) return false;
    s64 free = 0;
    Result rc = fsFsGetFreeSpace(&sd, "/", &free);
    fsFsClose(&sd);
    if (R_FAILED(rc)) return false;
    out = (uint64_t)free;
    return true;
#else
    struct statvfs st;
    if (statvfs(path.c_str(), &st) != 0) return false;
    out = (uint64_t)st.f_bavail * (uint64_t)st.f_frsize;
    return true;
#endif
}

std::string toLower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return s;
}

bool endsWithCI(const std::string& s, const std::string& suffix) {
    if (s.size() < suffix.size()) return false;
    return toLower(s.substr(s.size() - suffix.size())) == toLower(suffix);
}

std::string urlEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            out += (char)c;
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

static int hexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string urlDecode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size() && hexVal(s[i + 1]) >= 0 && hexVal(s[i + 2]) >= 0) {
            out += (char)(hexVal(s[i + 1]) * 16 + hexVal(s[i + 2]));
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

std::string formatSize(uint64_t bytes) {
    char buf[32];
    if (bytes < 1024)
        snprintf(buf, sizeof(buf), "%u B", (unsigned)bytes);
    else if (bytes < 1024ull * 1024)
        snprintf(buf, sizeof(buf), "%.1f KB", bytes / 1024.0);
    else if (bytes < 1024ull * 1024 * 1024)
        snprintf(buf, sizeof(buf), "%.1f MB", bytes / (1024.0 * 1024.0));
    else
        snprintf(buf, sizeof(buf), "%.2f GB", bytes / (1024.0 * 1024.0 * 1024.0));
    return buf;
}

}  // namespace util
