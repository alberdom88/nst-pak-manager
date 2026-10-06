// pak.cpp - archivi .pak (igArchive)
#include "pak.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace pak {

const char* const SWITCH_ROOT = "temporary/mack/data/nx/output/";

static const uint32_t SIGNATURE = 0x1A414749;
static const uint32_t HEADER_SIZE = 0x38;
static const uint32_t SECTOR = 0x800;
static const uint32_t SMALL = 0x7F, MEDIUM = 0x7FFF;

static uint32_t rd32(const unsigned char* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint64_t rd64(const unsigned char* p) { return rd32(p) | ((uint64_t)rd32(p + 4) << 32); }
static void wr32(std::string& s, uint32_t v) {
    char b[4] = {(char)(v & 0xFF), (char)((v >> 8) & 0xFF), (char)((v >> 16) & 0xFF), (char)(v >> 24)};
    s.append(b, 4);
}
static void wr16(std::string& s, uint16_t v) {
    char b[2] = {(char)(v & 0xFF), (char)(v >> 8)};
    s.append(b, 2);
}
static uint64_t alignUp(uint64_t v, uint64_t a) { return (v + a - 1) / a * a; }

static std::string lower(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c == '\\') c = '/';
    }
    return s;
}

uint32_t hashPath(const std::string& path) {
    uint32_t h = 0x811C9DC5;
    for (char c : lower(path)) h = (h ^ (unsigned char)c) * 0x1000193u;
    return h;
}

// Classe della tabella dei blocchi: 0 piccola (u8), 1 media (u16), 2 grande (u32)
static int sizeClass(int32_t size) {
    if ((uint32_t)size <= SMALL * SECTOR) return 0;
    if ((uint32_t)size <= MEDIUM * SECTOR) return 1;
    return 2;
}

struct File {
    FILE* f = nullptr;
    ~File() {
        if (f) fclose(f);
    }
};

static bool readAt(FILE* f, uint64_t offset, void* buf, size_t n) {
    return fseek(f, (long)offset, SEEK_SET) == 0 && fread(buf, 1, n, f) == n;
}

bool read(const std::string& path, Archive& out, std::string& err) {
    out = Archive();
    out.path = path;
    File file;
    file.f = fopen(path.c_str(), "rb");
    if (!file.f) {
        err = "impossibile aprire " + path;
        return false;
    }
    fseek(file.f, 0, SEEK_END);
    long endPos = ftell(file.f);
    uint64_t fileSize = endPos > 0 ? (uint64_t)endPos : 0;

    unsigned char h[HEADER_SIZE];
    if (!readAt(file.f, 0, h, sizeof(h)) || rd32(h) != SIGNATURE) {
        err = path + " non e' un archivio .pak";
        return false;
    }
    out.version = rd32(h + 4);
    uint32_t n = rd32(h + 0x0C), nl = rd32(h + 0x1C), nm = rd32(h + 0x20), ns = rd32(h + 0x24);
    uint64_t ptOff = rd64(h + 0x28);
    uint32_t ptSize = rd32(h + 0x30);
    uint64_t tocBytes = (uint64_t)n * 20 + (uint64_t)nl * 4 + (uint64_t)nm * 2 + ns;
    if (n > 1000000 || HEADER_SIZE + tocBytes > fileSize || ptOff + ptSize > fileSize || (uint64_t)n * 4 > ptSize) {
        err = path + ": intestazione dell'archivio non valida";
        return false;
    }
    std::string toc((size_t)tocBytes, '\0'), pt(ptSize, '\0');
    if (!readAt(file.f, HEADER_SIZE, &toc[0], toc.size()) || (ptSize && !readAt(file.f, ptOff, &pt[0], ptSize))) {
        err = path + ": lettura non riuscita";
        return false;
    }
    const unsigned char* t = (const unsigned char*)toc.data();
    const unsigned char* ids = t;
    const unsigned char* infos = t + 4 * n;
    const unsigned char* large = infos + 16 * n;
    const unsigned char* medium = large + 4 * nl;
    const unsigned char* small = medium + 2 * nm;
    const int lzmaHeader = out.version == 12 ? 4 : 2;

    for (uint32_t i = 0; i < n; i++) {
        Entry e;
        e.hash = rd32(ids + 4 * i);
        const unsigned char* info = infos + 16 * i;
        uint32_t offset = rd32(info), ordinal = rd32(info + 4), blockIndex = rd32(info + 12);
        e.size = (int32_t)rd32(info + 8);
        e.offset = (ordinal & 1) ? (uint64_t)offset + 0x100000000ull : offset;

        uint32_t rel = rd32((const unsigned char*)pt.data() + 4 * i);
        size_t a = rel, b = a < pt.size() ? pt.find('\0', a) : std::string::npos;
        size_t c = b == std::string::npos ? std::string::npos : pt.find('\0', b + 1);
        if (c == std::string::npos || e.size < 0) {
            err = path + ": elenco dei percorsi non valido";
            return false;
        }
        e.fullPath = pt.substr(a, b - a);
        e.path = pt.substr(b + 1, c - b - 1);

        if (blockIndex == 0xFFFFFFFFu) {
            e.compression = 0;
            e.stored = (uint64_t)e.size;
        } else {
            e.compression = blockIndex >> 28;
            uint32_t start = blockIndex & 0x0FFFFFFF;
            uint32_t count = ((uint32_t)e.size + 0x7FFF) >> 15;
            int cls = sizeClass(e.size);
            uint32_t tableCount = cls == 0 ? ns : cls == 1 ? nm : nl;
            if ((uint64_t)start + count > tableCount || count == 0) {
                err = path + ": tabella dei blocchi non valida per " + e.path;
                return false;
            }
            for (uint32_t k = 0; k < count; k++) {
                uint32_t v = cls == 0 ? small[start + k] : cls == 1 ? (uint32_t)(medium[2 * (start + k)] | (medium[2 * (start + k) + 1] << 8))
                                                         : rd32(large + 4 * (start + k));
                e.blocks.push_back(v);
            }
            // Byte occupati: fine dell'ultimo blocco
            uint32_t mask = cls == 0 ? SMALL : cls == 1 ? MEDIUM : 0x7FFFFFFFu;
            int shift = cls == 0 ? 7 : cls == 1 ? 15 : 31;
            uint32_t last = e.blocks.back();
            uint64_t lastOff = (uint64_t)(last & mask) * SECTOR;
            uint32_t lastSize = ((uint32_t)e.size < count * 0x8000u) ? ((uint32_t)e.size & 0x7FFF) : 0x8000u;
            uint64_t bytes = lastSize;
            if ((last >> shift) & 1) {
                unsigned char hb[4] = {0, 0, 0, 0};
                if (e.compression == 2) {
                    if (!readAt(file.f, e.offset + lastOff, hb, (size_t)lzmaHeader)) {
                        err = path + ": dati non leggibili per " + e.path;
                        return false;
                    }
                    uint32_t cs = lzmaHeader == 4 ? rd32(hb) : (uint32_t)(hb[0] | (hb[1] << 8));
                    bytes = (uint64_t)lzmaHeader + 5 + cs;
                } else if (e.compression == 1) {
                    if (!readAt(file.f, e.offset + lastOff, hb, 2)) {
                        err = path + ": dati non leggibili per " + e.path;
                        return false;
                    }
                    bytes = 2 + (uint64_t)(hb[0] | (hb[1] << 8));
                } else {
                    char buf[48];
                    snprintf(buf, sizeof(buf), "compressione %u non supportata", e.compression);
                    err = path + ": " + buf + " (" + e.path + ")";
                    return false;
                }
            }
            e.stored = lastOff + bytes;
        }
        if (e.offset + e.stored > fileSize) {
            err = path + ": dati fuori dall'archivio per " + e.path;
            return false;
        }
        out.entries.push_back(e);
    }
    return true;
}

bool extract(const Archive& a, const Entry& e, std::string& out, std::string& err) {
    if (e.inMemory) {
        out = e.data;
        return true;
    }
    if (e.compression != 0) {
        err = e.path + " e' compresso: rigeneralo con il convertitore aggiornato";
        return false;
    }
    File file;
    file.f = fopen(a.path.c_str(), "rb");
    out.assign((size_t)e.size, '\0');
    if (!file.f || (e.size && !readAt(file.f, e.offset, &out[0], out.size()))) {
        err = "impossibile leggere " + e.path + " da " + a.path;
        return false;
    }
    return true;
}

void put(Archive& a, const std::string& path, const std::string& data) {
    std::string root = SWITCH_ROOT;
    for (const Entry& e : a.entries) {
        if (e.fullPath.size() > e.path.size() && lower(e.fullPath).compare(e.fullPath.size() - e.path.size(), e.path.size(), lower(e.path)) == 0) {
            root = e.fullPath.substr(0, e.fullPath.size() - e.path.size());
            break;
        }
    }
    std::string key = lower(path);
    a.entries.erase(std::remove_if(a.entries.begin(), a.entries.end(), [&](const Entry& e) { return lower(e.path) == key; }),
                    a.entries.end());
    Entry e;
    e.path = path;
    e.fullPath = root + path;
    e.hash = hashPath(path);
    e.size = (int32_t)data.size();
    e.stored = data.size();
    e.inMemory = true;
    e.data = data;
    a.entries.push_back(e);
}

// Ricerca del gioco: binaria in una finestra attorno a hash / divisore (come Crash NST Maker)
static bool findable(const std::vector<uint32_t>& hashes, uint32_t divider, uint32_t margin, uint32_t target) {
    int64_t n = (int64_t)hashes.size();
    int64_t q = target / divider;
    int64_t start = std::max<int64_t>(0, q - margin), end = std::min<int64_t>(n - 1, q + margin + 1);
    if (start >= n || end < 0) return false;
    while (start <= end) {
        int64_t mid = start + (end - start) / 2;
        if (hashes[mid] == target) return true;
        if (hashes[mid] < target) start = mid + 1;
        else end = mid - 1;
    }
    return false;
}

bool write(const Archive& src, const std::string& outPath, std::string& err) {
    std::vector<const Entry*> files;
    for (const Entry& e : src.entries) files.push_back(&e);
    std::sort(files.begin(), files.end(), [](const Entry* x, const Entry* y) {
        return x->hash != y->hash ? x->hash < y->hash : lower(x->path) < lower(y->path);
    });
    const uint32_t n = (uint32_t)files.size();

    std::vector<uint32_t> hashes;
    for (const Entry* e : files) hashes.push_back(e->hash);
    uint32_t divider = n ? 0xFFFFFFFFu / n : 0, margin = 0;
    for (uint32_t m = 0; m < n; m++) {
        bool all = true;
        for (uint32_t h : hashes) {
            if (!findable(hashes, divider, m, h)) {
                all = false;
                break;
            }
        }
        if (all) {
            margin = m;
            break;
        }
    }

    // Tabelle dei blocchi e posizione dei dati
    std::vector<uint32_t> large, medium, small, blockIndex(n);
    for (uint32_t i = 0; i < n; i++) {
        const Entry* e = files[i];
        if (e->compression == 0) {
            blockIndex[i] = 0xFFFFFFFFu;
            continue;
        }
        int cls = sizeClass(e->size);
        std::vector<uint32_t>& table = cls == 0 ? small : cls == 1 ? medium : large;
        uint32_t start = (uint32_t)table.size();
        for (uint32_t v : e->blocks) table.push_back(v);
        table.push_back((uint32_t)((e->stored + SECTOR - 1) / SECTOR));  // settori usati in tutto
        blockIndex[i] = (e->compression << 28) | start;
    }
    uint64_t tocSize = (uint64_t)n * 20 + large.size() * 4 + medium.size() * 2 + small.size();
    uint64_t filesOffset = alignUp(HEADER_SIZE + tocSize, SECTOR);
    std::vector<uint64_t> offsets(n);
    uint64_t pos = filesOffset;
    for (uint32_t i = 0; i < n; i++) {
        offsets[i] = pos;
        pos += alignUp(files[i]->stored, SECTOR);
    }
    const uint64_t pathsStart = pos;

    std::string paths;
    paths.resize((size_t)n * 4);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t rel = (uint32_t)paths.size();
        memcpy(&paths[4 * i], &rel, 4);  // little endian su Switch e PC
        paths += files[i]->fullPath;
        paths.push_back('\0');
        paths += files[i]->path;
        paths.push_back('\0');
        wr32(paths, 0);
    }

    std::string head;
    wr32(head, SIGNATURE);
    wr32(head, src.version ? src.version : 12);
    wr32(head, (uint32_t)tocSize);
    wr32(head, n);
    wr32(head, SECTOR);
    wr32(head, divider);
    wr32(head, margin);
    wr32(head, (uint32_t)large.size());
    wr32(head, (uint32_t)medium.size());
    wr32(head, (uint32_t)small.size());
    wr32(head, (uint32_t)(pathsStart & 0xFFFFFFFFu));
    wr32(head, (uint32_t)(pathsStart >> 32));
    wr32(head, (uint32_t)paths.size());
    wr32(head, 1);
    for (uint32_t h : hashes) wr32(head, h);
    for (uint32_t i = 0; i < n; i++) {
        bool high = offsets[i] > 0xFFFFFFFFull;
        wr32(head, (uint32_t)(high ? offsets[i] - 0x100000000ull : offsets[i]));
        wr32(head, (uint32_t)(((int32_t)i - 1) * 256) | (high ? 1u : 0u));
        wr32(head, (uint32_t)files[i]->size);
        wr32(head, blockIndex[i]);
    }
    for (uint32_t v : large) wr32(head, v);
    for (uint32_t v : medium) wr16(head, (uint16_t)v);
    for (uint32_t v : small) head.push_back((char)(v & 0xFF));
    head.resize((size_t)filesOffset, '\0');

    File in, out;
    out.f = fopen(outPath.c_str(), "wb");
    if (!out.f) {
        err = "impossibile creare " + outPath;
        return false;
    }
    std::vector<char> buf(1024 * 1024);
    setvbuf(out.f, nullptr, _IOFBF, 1024 * 1024);
    bool ok = fwrite(head.data(), 1, head.size(), out.f) == head.size();
    for (uint32_t i = 0; ok && i < n; i++) {
        const Entry* e = files[i];
        if (e->inMemory) {
            ok = fwrite(e->data.data(), 1, e->data.size(), out.f) == e->data.size();
        } else {
            if (!in.f) in.f = fopen(src.path.c_str(), "rb");
            uint64_t left = e->stored, at = e->offset;
            ok = in.f != nullptr;
            while (ok && left > 0) {
                size_t chunk = (size_t)std::min<uint64_t>(left, buf.size());
                ok = readAt(in.f, at, buf.data(), chunk) && fwrite(buf.data(), 1, chunk, out.f) == chunk;
                left -= chunk;
                at += chunk;
            }
        }
        uint64_t pad = alignUp(e->stored, SECTOR) - e->stored;
        static const char zeros[SECTOR] = {0};
        if (ok && pad) ok = fwrite(zeros, 1, (size_t)pad, out.f) == pad;
    }
    if (ok) ok = fwrite(paths.data(), 1, paths.size(), out.f) == paths.size();
    if (fclose(out.f) != 0) ok = false;
    out.f = nullptr;
    if (!ok) {
        remove(outPath.c_str());
        err = "errore di scrittura di " + outPath + " (spazio esaurito?)";
        return false;
    }
    return true;
}

}  // namespace pak
