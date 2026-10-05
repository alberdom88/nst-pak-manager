// mega.cpp - cartelle condivise di MEGA
#include "mega.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <set>
#include <thread>

#include "cJSON.h"
#include "core.hpp"
#include "util.hpp"

#ifdef __SWITCH__
#include <switch.h>
#endif

namespace mega {

const char* const DEFAULT_API = "https://g.api.mega.co.nz";

// ---------------------------------------------------------------- link

static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    size_t b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

static std::string until(const std::string& s, size_t from, const char* stops) {
    size_t e = s.find_first_of(stops, from);
    return s.substr(from, e == std::string::npos ? std::string::npos : e - from);
}

bool parseLink(const std::string& text, Link& out, std::string& err) {
    out = Link();
    std::string s = trim(text);
    std::string keyText;
    size_t p;
    if ((p = s.find("#F!")) != std::string::npos) {  // formato vecchio: #F!cartella!chiave[!sottocartella]
        out.folder = until(s, p + 3, "!");
        size_t k = s.find('!', p + 3);
        if (k != std::string::npos) {
            keyText = until(s, k + 1, "!?/");
            size_t sub = s.find('!', k + 1);
            if (sub != std::string::npos) out.sub = until(s, sub + 1, "!?/");
        }
    } else if ((p = s.find("/folder/")) != std::string::npos) {  // formato attuale
        out.folder = until(s, p + 8, "#?/");
        size_t h = s.find('#', p);
        if (h != std::string::npos) {
            keyText = until(s, h + 1, "/?");
            size_t sub = s.find("/folder/", h);
            if (sub != std::string::npos) out.sub = until(s, sub + 8, "/?#");
        }
    } else if (s.find("/file/") != std::string::npos || s.find("#!") != std::string::npos) {
        err = "e' il link di un singolo file: serve il link della cartella (mega.nz/folder/...)";
        return false;
    } else if ((p = s.find('#')) != std::string::npos && s.find('/') == std::string::npos) {
        out.folder = s.substr(0, p);  // solo "cartella#chiave"
        keyText = s.substr(p + 1);
    } else {
        err = "link MEGA non riconosciuto (atteso https://mega.nz/folder/...#...)";
        return false;
    }
    out.key = b64decode(keyText);
    if (out.folder.empty()) {
        err = "nel link MEGA manca l'identificativo della cartella";
        return false;
    }
    if (out.key.size() != 16) {
        err = "nel link MEGA manca la chiave della cartella (la parte dopo #)";
        return false;
    }
    return true;
}

std::string b64decode(const std::string& s) {
    std::string out;
    uint32_t acc = 0;
    int bits = 0;
    for (char c : s) {
        int v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '-' || c == '+') v = 62;
        else if (c == '_' || c == '/') v = 63;
        else if (c == '=') break;
        else return "";
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back((char)((acc >> bits) & 0xFF));
        }
    }
    return out;
}

// ---------------------------------------------------------------- AES-128

static uint8_t g_sbox[256], g_inv[256];
static bool g_tablesReady = false;

static inline uint8_t xt(uint8_t x) { return (uint8_t)((x << 1) ^ ((x & 0x80) ? 0x1B : 0)); }

static uint8_t gmul(uint8_t a, uint8_t b) {
    uint8_t r = 0;
    while (b) {
        if (b & 1) r ^= a;
        a = xt(a);
        b >>= 1;
    }
    return r;
}

static void initTables() {
    if (g_tablesReady) return;
    for (int x = 0; x < 256; x++) {
        uint8_t inv = 0;
        for (int y = 1; y < 256 && x; y++)
            if (gmul((uint8_t)x, (uint8_t)y) == 1) {
                inv = (uint8_t)y;
                break;
            }
        uint8_t s = inv;
        for (int i = 1; i <= 4; i++) s ^= (uint8_t)((inv << i) | (inv >> (8 - i)));
        s ^= 0x63;
        g_sbox[x] = s;
        g_inv[s] = (uint8_t)x;
    }
    g_tablesReady = true;
}

Aes128::Aes128(const void* key16) {
    initTables();
    static const uint8_t RCON[10] = {0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1B, 0x36};
    memcpy(rk_, key16, 16);
    for (int i = 4; i < 44; i++) {
        uint8_t t[4];
        memcpy(t, rk_ + 4 * (i - 1), 4);
        if (i % 4 == 0) {
            uint8_t u = t[0];
            t[0] = (uint8_t)(g_sbox[t[1]] ^ RCON[i / 4 - 1]);
            t[1] = g_sbox[t[2]];
            t[2] = g_sbox[t[3]];
            t[3] = g_sbox[u];
        }
        for (int j = 0; j < 4; j++) rk_[4 * i + j] = (uint8_t)(rk_[4 * (i - 4) + j] ^ t[j]);
    }
}

void Aes128::encrypt(const uint8_t in[16], uint8_t out[16]) const {
    uint8_t s[16], t[16];
    for (int i = 0; i < 16; i++) s[i] = (uint8_t)(in[i] ^ rk_[i]);
    for (int round = 1; round <= 10; round++) {
        for (int c = 0; c < 4; c++)  // SubBytes + ShiftRows
            for (int r = 0; r < 4; r++) t[r + 4 * c] = g_sbox[s[r + 4 * ((c + r) % 4)]];
        if (round < 10) {
            for (int c = 0; c < 4; c++) {  // MixColumns
                uint8_t* a = t + 4 * c;
                uint8_t a0 = a[0], a1 = a[1], a2 = a[2], a3 = a[3];
                uint8_t all = (uint8_t)(a0 ^ a1 ^ a2 ^ a3);
                a[0] = (uint8_t)(a0 ^ all ^ xt((uint8_t)(a0 ^ a1)));
                a[1] = (uint8_t)(a1 ^ all ^ xt((uint8_t)(a1 ^ a2)));
                a[2] = (uint8_t)(a2 ^ all ^ xt((uint8_t)(a2 ^ a3)));
                a[3] = (uint8_t)(a3 ^ all ^ xt((uint8_t)(a3 ^ a0)));
            }
        }
        for (int i = 0; i < 16; i++) s[i] = (uint8_t)(t[i] ^ rk_[16 * round + i]);
    }
    memcpy(out, s, 16);
}

void Aes128::decrypt(const uint8_t in[16], uint8_t out[16]) const {
    uint8_t s[16], t[16];
    for (int i = 0; i < 16; i++) s[i] = (uint8_t)(in[i] ^ rk_[160 + i]);
    for (int round = 9; round >= 0; round--) {
        for (int c = 0; c < 4; c++)  // InvShiftRows + InvSubBytes
            for (int r = 0; r < 4; r++) t[r + 4 * ((c + r) % 4)] = g_inv[s[r + 4 * c]];
        for (int i = 0; i < 16; i++) t[i] = (uint8_t)(t[i] ^ rk_[16 * round + i]);
        if (round > 0) {
            for (int c = 0; c < 4; c++) {  // InvMixColumns
                uint8_t* a = t + 4 * c;
                uint8_t a0 = a[0], a1 = a[1], a2 = a[2], a3 = a[3];
                a[0] = (uint8_t)(gmul(a0, 14) ^ gmul(a1, 11) ^ gmul(a2, 13) ^ gmul(a3, 9));
                a[1] = (uint8_t)(gmul(a0, 9) ^ gmul(a1, 14) ^ gmul(a2, 11) ^ gmul(a3, 13));
                a[2] = (uint8_t)(gmul(a0, 13) ^ gmul(a1, 9) ^ gmul(a2, 14) ^ gmul(a3, 11));
                a[3] = (uint8_t)(gmul(a0, 11) ^ gmul(a1, 13) ^ gmul(a2, 9) ^ gmul(a3, 14));
            }
        }
        memcpy(s, t, 16);
    }
    memcpy(out, s, 16);
}

#ifdef __SWITCH__
// Sulla Switch il CTR usa le istruzioni AES della CPU (libnx): molto piu' veloce
struct CtrImpl {
    Aes128CtrContext ctx;
};

Ctr::Ctr(const void* key16, const void* iv16) {
    CtrImpl* c = new CtrImpl;
    aes128CtrContextCreate(&c->ctx, key16, iv16);
    impl_ = c;
}

Ctr::~Ctr() { delete (CtrImpl*)impl_; }

void Ctr::crypt(uint8_t* data, size_t n) { aes128CtrCrypt(&((CtrImpl*)impl_)->ctx, data, data, n); }
#else
struct CtrImpl {
    Aes128 aes;
    uint8_t ctr[16];
    uint8_t stream[16];
    size_t used = 16;
    explicit CtrImpl(const void* key) : aes(key) {}
};

Ctr::Ctr(const void* key16, const void* iv16) {
    CtrImpl* c = new CtrImpl(key16);
    memcpy(c->ctr, iv16, 16);
    impl_ = c;
}

Ctr::~Ctr() { delete (CtrImpl*)impl_; }

void Ctr::crypt(uint8_t* data, size_t n) {
    CtrImpl* c = (CtrImpl*)impl_;
    for (size_t i = 0; i < n; i++) {
        if (c->used == 16) {
            c->aes.encrypt(c->ctr, c->stream);
            for (int k = 15; k >= 0 && ++c->ctr[k] == 0; k--) {
            }
            c->used = 0;
        }
        data[i] ^= c->stream[c->used++];
    }
}
#endif

// ---------------------------------------------------------------- chiavi e attributi

std::string decryptNodeKey(const std::string& k, const std::string& folderKey, const std::string& folderHandle) {
    // "handle:chiave" oppure piu' voci separate da '/'
    std::string chosen;
    size_t start = 0;
    while (start <= k.size()) {
        size_t end = k.find('/', start);
        std::string item = k.substr(start, end == std::string::npos ? std::string::npos : end - start);
        size_t colon = item.find(':');
        std::string handle = colon == std::string::npos ? "" : item.substr(0, colon);
        std::string key = colon == std::string::npos ? item : item.substr(colon + 1);
        if (chosen.empty() || handle == folderHandle) chosen = key;
        if (handle == folderHandle) break;
        if (end == std::string::npos) break;
        start = end + 1;
    }
    std::string enc = b64decode(chosen);
    if (enc.empty() || enc.size() % 16 != 0 || folderKey.size() != 16) return "";
    Aes128 aes(folderKey.data());
    std::string out(enc.size(), '\0');
    for (size_t i = 0; i < enc.size(); i += 16)
        aes.decrypt((const uint8_t*)enc.data() + i, (uint8_t*)&out[i]);
    return out;
}

std::string fileAesKey(const std::string& nodeKey) {
    if (nodeKey.size() != 32) return nodeKey.size() == 16 ? nodeKey : "";
    std::string k(16, '\0');
    for (int i = 0; i < 16; i++) k[i] = (char)(nodeKey[i] ^ nodeKey[i + 16]);
    return k;
}

std::string fileIv(const std::string& nodeKey) {
    std::string iv(16, '\0');
    if (nodeKey.size() == 32) memcpy(&iv[0], nodeKey.data() + 16, 8);
    return iv;
}

std::string decryptName(const std::string& attrB64, const std::string& aesKey) {
    std::string enc = b64decode(attrB64);
    if (enc.empty() || enc.size() % 16 != 0 || aesKey.size() != 16) return "";
    Aes128 aes(aesKey.data());
    std::string plain(enc.size(), '\0');
    uint8_t prev[16] = {0};
    for (size_t i = 0; i < enc.size(); i += 16) {  // CBC con IV a zero
        uint8_t block[16];
        aes.decrypt((const uint8_t*)enc.data() + i, block);
        for (int j = 0; j < 16; j++) plain[i + j] = (char)(block[j] ^ prev[j]);
        memcpy(prev, enc.data() + i, 16);
    }
    if (plain.compare(0, 5, "MEGA{") != 0) return "";
    size_t end = plain.rfind('}');
    if (end == std::string::npos) return "";
    std::string json = plain.substr(4, end - 3);
    cJSON* root = cJSON_Parse(json.c_str());
    if (!root) return "";
    const cJSON* n = cJSON_GetObjectItemCaseSensitive(root, "n");
    std::string name = (cJSON_IsString(n) && n->valuestring) ? n->valuestring : "";
    cJSON_Delete(root);
    return name;
}

// ---------------------------------------------------------------- API

static std::string errorText(int code) {
    switch (code) {
        case -2: return "richiesta non valida";
        case -3: return "server MEGA occupato, riprova";
        case -4: return "troppe richieste, riprova tra poco";
        case -9: return "cartella non trovata (link sbagliato o cartella rimossa)";
        case -11: return "accesso negato alla cartella";
        case -16: return "cartella bloccata da MEGA";
        case -17: return "quota di MEGA superata, riprova piu' tardi";
        case -18: return "MEGA momentaneamente non disponibile, riprova";
        default: {
            char buf[48];
            snprintf(buf, sizeof(buf), "errore MEGA %d", code);
            return buf;
        }
    }
}

// Esegue un comando dell'API. In caso di successo ritorna la risposta (oggetto JSON) da
// liberare con cJSON_Delete; ritenta quando il server e' occupato.
static cJSON* apiCall(const std::string& api, const std::string& folder, const std::string& command,
                      std::string& err) {
    static uint32_t seq = (uint32_t)time(nullptr);
    int delayMs = 250;
    for (int attempt = 0; attempt < 6; attempt++) {
        char id[16];
        snprintf(id, sizeof(id), "%u", (unsigned)(seq++ % 1000000000u));
        std::string url = api + "/cs?id=" + id + "&n=" + util::urlEncode(folder);
        std::string body;
        long status = 0;
        if (!net::post(url, "[" + command + "]", body, status, err)) return nullptr;
        int code = 0;
        cJSON* root = status < 500 ? cJSON_Parse(body.c_str()) : nullptr;
        if (root && cJSON_IsNumber(root)) {
            code = root->valueint;
        } else if (root && cJSON_IsArray(root) && cJSON_GetArraySize(root) > 0) {
            cJSON* first = cJSON_DetachItemFromArray(root, 0);
            cJSON_Delete(root);
            root = first;
            if (cJSON_IsNumber(root)) code = root->valueint;
            const cJSON* e = cJSON_GetObjectItemCaseSensitive(root, "e");
            if (cJSON_IsNumber(e) && e->valueint < 0) code = e->valueint;
            if (code == 0 && cJSON_IsObject(root)) return root;
        } else if (status >= 500) {
            code = -3;
        } else {
            cJSON_Delete(root);
            char buf[64];
            snprintf(buf, sizeof(buf), "risposta di MEGA non valida (HTTP %ld)", status);
            err = buf;
            return nullptr;
        }
        cJSON_Delete(root);
        if (code != -3 && code != -4 && code != -18) {
            err = errorText(code);
            return nullptr;
        }
        err = errorText(code);
        std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
        delayMs *= 2;
    }
    return nullptr;
}

static std::string str(const cJSON* obj, const char* key) {
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return (cJSON_IsString(v) && v->valuestring) ? v->valuestring : "";
}

bool list(const SourceConfig& src, std::vector<RemoteFile>& out, std::string& err) {
    Link link;
    if (!parseLink(src.url, link, err)) return false;
    const std::string api = src.apiBase.empty() ? DEFAULT_API : src.apiBase;
    cJSON* resp = apiCall(api, link.folder, "{\"a\":\"f\",\"c\":1,\"ca\":1,\"r\":1}", err);
    if (!resp) return false;

    struct Node {
        std::string h, p, a, k;
        int t = -1;
        uint64_t s = 0;
    };
    std::vector<Node> nodes;
    std::set<std::string> handles;
    const cJSON* f;
    cJSON_ArrayForEach(f, cJSON_GetObjectItemCaseSensitive(resp, "f")) {
        Node n;
        n.h = str(f, "h");
        n.p = str(f, "p");
        n.a = str(f, "a");
        n.k = str(f, "k");
        const cJSON* t = cJSON_GetObjectItemCaseSensitive(f, "t");
        const cJSON* s = cJSON_GetObjectItemCaseSensitive(f, "s");
        n.t = cJSON_IsNumber(t) ? t->valueint : -1;
        n.s = cJSON_IsNumber(s) && s->valuedouble > 0 ? (uint64_t)s->valuedouble : 0;
        if (n.h.empty()) continue;
        handles.insert(n.h);
        nodes.push_back(n);
    }
    cJSON_Delete(resp);

    // Radice condivisa: la cartella il cui genitore non fa parte dell'elenco
    std::string top;
    for (const Node& n : nodes)
        if (n.t == 1 && !handles.count(n.p)) {
            top = n.h;
            break;
        }
    std::string root = link.sub.empty() ? top : link.sub;
    if (root.empty() || !handles.count(root)) {
        err = link.sub.empty() ? "cartella MEGA vuota o non leggibile" : "sottocartella del link non trovata";
        return false;
    }

    int files = 0, unreadable = 0;
    for (const Node& n : nodes) {
        if (n.t != 0 || n.p != root) continue;
        files++;
        std::string key = decryptNodeKey(n.k, link.key, top);
        std::string name = key.size() == 32 ? decryptName(n.a, fileAesKey(key)) : "";
        if (name.empty()) {
            unreadable++;
            continue;
        }
        if (!isValidPakName(name)) continue;
        bool dup = false;
        for (const RemoteFile& e : out) dup = dup || util::toLower(e.name) == util::toLower(name);
        if (dup) continue;
        RemoteFile rf;
        rf.name = name;
        rf.url = "mega:" + n.h;
        rf.size = n.s;
        rf.sizeKnown = n.s > 0;
        rf.megaApi = api;
        rf.megaFolder = link.folder;
        rf.megaNode = n.h;
        rf.megaKey = key;
        out.push_back(rf);
    }
    if (files > 0 && unreadable == files) {
        err = "impossibile decifrare i nomi dei file: la chiave nel link (dopo #) e' sbagliata o incompleta";
        out.clear();
        return false;
    }
    return true;
}

bool download(const RemoteFile& file, const std::string& path, const net::Progress& progress,
              uint64_t& written, std::string& err) {
    written = 0;
    if (file.megaKey.size() != 32 || file.megaNode.empty()) {
        err = "dati del file MEGA incompleti: ricarica l'elenco";
        return false;
    }
    cJSON* resp = apiCall(file.megaApi, file.megaFolder, "{\"a\":\"g\",\"g\":1,\"n\":\"" + file.megaNode + "\"}", err);
    if (!resp) return false;
    std::string url = str(resp, "g");
    const cJSON* s = cJSON_GetObjectItemCaseSensitive(resp, "s");
    uint64_t size = cJSON_IsNumber(s) && s->valuedouble > 0 ? (uint64_t)s->valuedouble : file.size;
    cJSON_Delete(resp);
    if (url.empty()) {
        err = "MEGA non ha fornito l'indirizzo del file";
        return false;
    }
    std::string key = fileAesKey(file.megaKey), iv = fileIv(file.megaKey);
    Ctr ctr(key.data(), iv.data());
    net::Transform decrypt = [&ctr](char* data, size_t n) { ctr.crypt((uint8_t*)data, n); };
    if (!net::download(url, path, size, progress, written, err, &decrypt)) return false;
    if (size && written != size) {
        util::removeFile(path);
        err = "download incompleto (" + util::formatSize(written) + " su " + util::formatSize(size) + ")";
        return false;
    }
    return true;
}

}  // namespace mega
