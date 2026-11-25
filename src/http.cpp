// SPDX-License-Identifier: MIT
#include "okou/core.hpp"
#include "okou/diagnostics.hpp"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <curl/curl.h>
#include <limits>
#include <openssl/rand.h>

namespace okou {
namespace {
constexpr std::size_t max_headers = 64 * 1024;
struct CurlGlobal {
    CurlGlobal() {
        if (curl_global_init(CURL_GLOBAL_DEFAULT))
            throw HttpError(0, "HTTP initialization failed");
    }
    ~CurlGlobal() {
        curl_global_cleanup();
    }
};
struct Transfer {
    HttpResponse response;
    std::FILE *output = nullptr;
    std::size_t body_limit = 0;
    std::size_t body_bytes = 0;
    std::size_t header_bytes = 0;
    bool overflow = false;
    bool output_error = false;
};
std::size_t receive(char *p, std::size_t size, std::size_t n, void *arg) noexcept {
    auto &t = *static_cast<Transfer *>(arg);
    if (size && n > std::numeric_limits<std::size_t>::max() / size)
        return 0;
    auto bytes = size * n;
    if (bytes > t.body_limit - t.body_bytes) {
        t.overflow = true;
        return 0;
    }
    t.body_bytes += bytes;
    if (t.output) {
        const auto written = std::fwrite(p, 1, bytes, t.output);
        t.output_error = written != bytes;
        return written;
    }
    try {
        t.response.body.append(p, bytes);
        return bytes;
    } catch (...) {
        return 0;
    }
}
std::size_t header(char *p, std::size_t size, std::size_t n, void *arg) noexcept {
    auto &t = *static_cast<Transfer *>(arg);
    if (size && n > std::numeric_limits<std::size_t>::max() / size)
        return 0;
    auto bytes = size * n;
    if (bytes > max_headers - t.header_bytes) {
        t.overflow = true;
        return 0;
    }
    t.header_bytes += bytes;
    try {
        std::string line(p, bytes);
        auto colon = line.find(':');
        if (colon != std::string::npos) {
            auto key = line.substr(0, colon), value = line.substr(colon + 1);
            std::transform(key.begin(), key.end(), key.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            auto start = value.find_first_not_of(" \t\r\n"),
                 end = value.find_last_not_of(" \t\r\n");
            t.response.headers[key] =
                start == std::string::npos ? "" : value.substr(start, end - start + 1);
        } else if (line.rfind("HTTP/", 0) == 0)
            t.response.headers.clear();
        return bytes;
    } catch (...) {
        return 0;
    }
}
int progress(void *p, curl_off_t, curl_off_t, curl_off_t, curl_off_t) noexcept {
    return p && static_cast<std::atomic_bool *>(p)->load() ? 1 : 0;
}
bool bad_header(const std::string &s) {
    return s.find_first_of("\r\n") != std::string::npos || s.find('\0') != std::string::npos;
}
std::string request_id() {
    unsigned char b[16];
    if (RAND_bytes(b, 16) != 1)
        throw HttpError(0, "Request identity generation failed");
    b[6] = (b[6] & 15) | 64;
    b[8] = (b[8] & 63) | 128;
    const char *hex = "0123456789abcdef";
    std::string out;
    for (int i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10)
            out += '-';
        out += hex[b[i] >> 4];
        out += hex[b[i] & 15];
    }
    return out;
}
} // namespace
HttpError::HttpError(long s, std::string message, bool u, int code, std::string_view oauth)
    : std::runtime_error(std::move(message)), status(s), uncertain(u), curl_code(code),
      oauth_error(diagnostics::oauth_error_code(oauth)) {}
Json HttpResponse::json() const {
    try {
        return Json::parse(body);
    } catch (...) {
        throw HttpError(status, "Invalid JSON response");
    }
}
Http::Http(std::atomic_bool *cancelled) : cancelled_(cancelled) {}
std::string Http::encode(const std::string &value) {
    const char *hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : value) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~')
            out += char(c);
        else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}
HttpResponse Http::request(const std::string &method, const std::string &url, std::string_view body,
                           const std::string &bearer,
                           const std::map<std::string, std::string> &headers, std::FILE *output,
                           std::size_t body_limit) {
    static CurlGlobal global;
    if (cancelled_ && cancelled_->load())
        throw HttpError(0, "Request cancelled");
    if (method.empty() || !std::all_of(method.begin(), method.end(),
                                       [](unsigned char c) { return c >= 'A' && c <= 'Z'; }))
        throw HttpError(0, "Invalid HTTP method");
    const bool writing = method != "GET" && method != "HEAD" && method != "OPTIONS";
    std::unique_ptr<CURLU, decltype(&curl_url_cleanup)> parsed(curl_url(), curl_url_cleanup);
    if (!parsed || url.find('\0') != std::string::npos ||
        curl_url_set(parsed.get(), CURLUPART_URL, url.c_str(), 0))
        throw HttpError(0, "Invalid request URL");
    auto part = [&](CURLUPart key) {
        char *raw = nullptr;
        std::string s;
        if (!curl_url_get(parsed.get(), key, &raw, 0)) {
            s = raw;
            curl_free(raw);
        }
        return s;
    };
    auto scheme = part(CURLUPART_SCHEME), host = part(CURLUPART_HOST);
    const char *fixture = std::getenv("OKOU_ZERO_TEST_ALLOW_LOOPBACK_HTTP");
    bool loopback = host == "127.0.0.1" || host == "[::1]" || host == "::1";
    if ((scheme != "https" &&
         !(scheme == "http" && loopback && fixture && std::string(fixture) == "1")) ||
        !part(CURLUPART_USER).empty() || !part(CURLUPART_PASSWORD).empty() ||
        !part(CURLUPART_FRAGMENT).empty())
        throw HttpError(0, "HTTPS URL without credentials required");
    if (bad_header(bearer) || bearer.size() > 64 * 1024)
        throw HttpError(0, "Invalid authorization header");
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), curl_easy_cleanup);
    if (!curl)
        throw HttpError(0, "HTTP initialization failed");
    Transfer transfer;
    transfer.output = output;
    transfer.body_limit = body_limit;
    curl_slist *list = nullptr;
    struct ListGuard {
        curl_slist *&list;
        ~ListGuard() {
            curl_slist_free_all(list);
        }
    } guard{list};
    auto add = [&](const std::string &s) {
        auto next = curl_slist_append(list, s.c_str());
        if (!next)
            throw HttpError(0, "HTTP header allocation failed");
        list = next;
    };
    add("Accept: application/json");
    if (!bearer.empty()) {
        add("Authorization: Bearer " + bearer);
        add("X-Client-Type: OkouZero");
        add("X-Client-Version: " OKOU_ZERO_VERSION);
        add("X-Client-Request-Id: " + request_id());
    }
    for (const auto &[key, value] : headers) {
        const bool token_name =
            !key.empty() && std::all_of(key.begin(), key.end(), [](unsigned char c) {
                return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                       std::string("!#$%&'*+-.^_`|~").find(static_cast<char>(c)) !=
                           std::string::npos;
            });
        if (!token_name || bad_header(value))
            throw HttpError(0, "Invalid request header");
        std::string lower = key;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        if (lower == "authorization" || lower == "host" || lower == "x-client-type" ||
            lower == "x-client-request-id" || lower == "x-client-version")
            throw HttpError(0, "Reserved request header");
        add(key + ": " + value);
    }
    curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_CUSTOMREQUEST, method.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, list);
    curl_easy_setopt(curl.get(), CURLOPT_USERAGENT, "OkouZero/" OKOU_ZERO_VERSION);
    curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYHOST, 2L);
    // Explicit test trust is scoped to numeric loopback HTTPS only. Never
    // disable verification, and never apply fixture CA files to public hosts.
    if (scheme == "https" && loopback) {
        const char *ca = std::getenv("OKOU_ZERO_TEST_CA_FILE");
        if (ca && *ca)
            curl_easy_setopt(curl.get(), CURLOPT_CAINFO, ca);
    }
    curl_easy_setopt(curl.get(), CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);
    curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl.get(), CURLOPT_MAXREDIRS, 0L);
    curl_easy_setopt(curl.get(), CURLOPT_PROTOCOLS_STR, "https,http");
    curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT_MS, 10000L);
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT_MS, 60000L);
    curl_easy_setopt(curl.get(), CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_LOW_SPEED_TIME, 20L);
    curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, receive);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &transfer);
    curl_easy_setopt(curl.get(), CURLOPT_HEADERFUNCTION, header);
    curl_easy_setopt(curl.get(), CURLOPT_HEADERDATA, &transfer);
    curl_easy_setopt(curl.get(), CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl.get(), CURLOPT_XFERINFOFUNCTION, progress);
    curl_easy_setopt(curl.get(), CURLOPT_XFERINFODATA, cancelled_);
    if (writing || !body.empty()) {
        curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, body.empty() ? "" : body.data());
        curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE_LARGE,
                         static_cast<curl_off_t>(body.size()));
    }
    if (method == "HEAD")
        curl_easy_setopt(curl.get(), CURLOPT_NOBODY, 1L);
    const auto result = curl_easy_perform(curl.get());
    curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &transfer.response.status);
    if (result != CURLE_OK) {
        const char *reason = transfer.overflow                     ? "Response exceeds size limit"
                             : transfer.output_error               ? "Cannot spool HTTP response"
                             : result == CURLE_ABORTED_BY_CALLBACK ? "Request cancelled"
                             : result == CURLE_OPERATION_TIMEDOUT  ? "Request timed out"
                                                                  : "Network or TLS request failed";
        throw HttpError(transfer.response.status, reason, writing, static_cast<int>(result));
    }
    if (transfer.response.status >= 500)
        throw HttpError(transfer.response.status, "Server request failed", writing, 0,
                        diagnostics::response_oauth_error(transfer.response.body));
    // HTTP 4xx is returned so OAuth can inspect the standard error field. Callers
    // must check status before interpreting success payloads. No request is retried.
    return std::move(transfer.response);
}
} // namespace okou
