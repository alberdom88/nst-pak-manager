// net.cpp
#include "net.hpp"

#include <cstdio>
#include <cstdlib>
#include <curl/curl.h>

#include "util.hpp"

namespace net {

static std::string g_caFile;
static const char* USER_AGENT = "nst-pak-manager/1.0";
static const size_t MAX_BODY = 16 * 1024 * 1024;

bool init() { return curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK; }
void shutdown() { curl_global_cleanup(); }
void setCaFile(const std::string& path) { g_caFile = path; }

static void commonOptions(CURL* c, const std::string& url, char* errbuf) {
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_USERAGENT, USER_AGENT);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 20L);
    // Interrompe se per 60 secondi arriva meno di 1 byte/s
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 60L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_ERRORBUFFER, errbuf);
    if (!g_caFile.empty() && util::fileExists(g_caFile))
        curl_easy_setopt(c, CURLOPT_CAINFO, g_caFile.c_str());
}

static std::string describe(CURLcode rc, const char* errbuf, long status) {
    if (rc == CURLE_HTTP_RETURNED_ERROR || (rc == CURLE_OK && status >= 400)) {
        char buf[64];
        snprintf(buf, sizeof(buf), "il server ha risposto HTTP %ld", status);
        std::string s = buf;
        if (status == 401 || status == 403) s += " (accesso negato: controlla permessi o chiave API)";
        if (status == 404) s += " (file o cartella non trovati)";
        return s;
    }
    std::string s = curl_easy_strerror(rc);
    if (errbuf && errbuf[0]) s += std::string(": ") + errbuf;
    if (rc == CURLE_PEER_FAILED_VERIFICATION || rc == CURLE_SSL_CACERT_BADFILE ||
        rc == CURLE_SSL_CONNECT_ERROR)
        s += " (problema certificati: prova a mettere cacert.pem nella cartella dell'app)";
    return s;
}

static size_t writeString(char* ptr, size_t size, size_t nmemb, void* user) {
    std::string* s = (std::string*)user;
    size_t n = size * nmemb;
    if (s->size() + n > MAX_BODY) return 0;
    s->append(ptr, n);
    return n;
}

bool get(const std::string& url, std::string& body, long& status, std::string& err,
         std::string* effectiveUrl) {
    body.clear();
    status = 0;
    CURL* c = curl_easy_init();
    if (!c) {
        err = "impossibile inizializzare curl";
        return false;
    }
    char errbuf[CURL_ERROR_SIZE] = {0};
    commonOptions(c, url, errbuf);
    curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 120L);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, writeString);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &body);
    CURLcode rc = curl_easy_perform(c);
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    if (effectiveUrl) {
        char* eff = nullptr;
        curl_easy_getinfo(c, CURLINFO_EFFECTIVE_URL, &eff);
        *effectiveUrl = eff ? eff : url;
    }
    curl_easy_cleanup(c);
    if (rc != CURLE_OK) {
        err = (rc == CURLE_WRITE_ERROR) ? "risposta troppo grande" : describe(rc, errbuf, status);
        return false;
    }
    return true;
}

static size_t discard(char*, size_t size, size_t nmemb, void*) { return size * nmemb; }

std::vector<int64_t> contentLengths(const std::vector<std::string>& urls) {
    std::vector<int64_t> out(urls.size(), -1);
    CURL* c = curl_easy_init();
    if (!c) return out;
    char errbuf[CURL_ERROR_SIZE] = {0};
    for (size_t i = 0; i < urls.size(); i++) {
        commonOptions(c, urls[i], errbuf);
        curl_easy_setopt(c, CURLOPT_NOBODY, 1L);
        curl_easy_setopt(c, CURLOPT_TIMEOUT, 15L);
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, discard);
        CURLcode rc = curl_easy_perform(c);
        if (rc == CURLE_COULDNT_CONNECT || rc == CURLE_OPERATION_TIMEDOUT ||
            rc == CURLE_COULDNT_RESOLVE_HOST)
            break;
        long status = 0;
        curl_off_t len = -1;
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
        curl_easy_getinfo(c, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &len);
        if (rc == CURLE_OK && status >= 200 && status < 300 && len > 0) out[i] = (int64_t)len;
    }
    curl_easy_cleanup(c);
    return out;
}

struct DownloadCtx {
    FILE* f = nullptr;
    uint64_t written = 0;
    uint64_t expected = 0;
    bool writeError = false;
    bool cancelled = false;
    const Progress* progress = nullptr;
};

static size_t writeFile(char* ptr, size_t size, size_t nmemb, void* user) {
    DownloadCtx* ctx = (DownloadCtx*)user;
    size_t n = size * nmemb;
    if (fwrite(ptr, 1, n, ctx->f) != n) {
        ctx->writeError = true;
        return 0;
    }
    ctx->written += n;
    return n;
}

static int onProgress(void* user, curl_off_t dltotal, curl_off_t dlnow, curl_off_t, curl_off_t) {
    DownloadCtx* ctx = (DownloadCtx*)user;
    if (!ctx->progress || !*ctx->progress) return 0;
    uint64_t total = dltotal > 0 ? (uint64_t)dltotal : ctx->expected;
    if (!(*ctx->progress)((uint64_t)dlnow, total)) {
        ctx->cancelled = true;
        return 1;
    }
    return 0;
}

bool download(const std::string& url, const std::string& path, uint64_t expectedSize,
              const Progress& progress, uint64_t& written, std::string& err) {
    written = 0;
    CURL* c = curl_easy_init();
    if (!c) {
        err = "impossibile inizializzare curl";
        return false;
    }
    DownloadCtx ctx;
    ctx.expected = expectedSize;
    ctx.progress = &progress;
    ctx.f = fopen(path.c_str(), "wb");
    if (!ctx.f) {
        curl_easy_cleanup(c);
        err = "impossibile creare il file su SD: " + path;
        return false;
    }
    // Buffer grande: le scritture piccole sulla SD della Switch sono lente
    const size_t bufSize = 1024 * 1024;
    char* buf = (char*)malloc(bufSize);
    if (buf) setvbuf(ctx.f, buf, _IOFBF, bufSize);

    char errbuf[CURL_ERROR_SIZE] = {0};
    commonOptions(c, url, errbuf);
    curl_easy_setopt(c, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(c, CURLOPT_BUFFERSIZE, 256L * 1024L);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, writeFile);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, onProgress);
    curl_easy_setopt(c, CURLOPT_XFERINFODATA, &ctx);

    CURLcode rc = curl_easy_perform(c);
    long status = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(c);

    bool closeOk = fclose(ctx.f) == 0;
    free(buf);
    written = ctx.written;

    if (rc != CURLE_OK || !closeOk) {
        if (ctx.cancelled)
            err = "annullato";
        else if (ctx.writeError || !closeOk)
            err = "errore di scrittura su SD (spazio esaurito?)";
        else
            err = describe(rc, errbuf, status);
        remove(path.c_str());
        return false;
    }
    return true;
}

}  // namespace net
