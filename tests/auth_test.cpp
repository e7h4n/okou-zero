// SPDX-License-Identifier: MIT
#include "diagnostic_capture.hpp"
#include "okou/core.hpp"
#include <ctime>
#include <curl/curl.h>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <netinet/in.h>
#include <openssl/evp.h>
#include <optional>
#include <sys/socket.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x))                                                                                  \
            throw std::runtime_error("check failed: " #x);                                         \
    } while (false)
namespace {
using okou::Json;
struct Reply {
    int status;
    Json body;
    std::optional<std::string> raw = {};
    bool disconnect = false;
};
class Fixture {
    int fd_;
    std::thread thread_;
    std::vector<Reply> replies_;
    std::mutex mutex_;
    std::vector<std::string> requests_;

  public:
    std::string base;
    Fixture(std::vector<Reply> replies) : replies_(std::move(replies)) {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        CHECK(fd_ >= 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        CHECK(::bind(fd_, reinterpret_cast<sockaddr *>(&addr), sizeof addr) == 0);
        CHECK(::listen(fd_, 16) == 0);
        socklen_t len = sizeof addr;
        CHECK(::getsockname(fd_, reinterpret_cast<sockaddr *>(&addr), &len) == 0);
        base = "http://127.0.0.1:" + std::to_string(ntohs(addr.sin_port));
        thread_ = std::thread([this] {
            for (const auto &r : replies_) {
                int c = ::accept(fd_, nullptr, nullptr);
                if (c < 0)
                    break;
                timeval timeout{3, 0};
                ::setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
                std::string request;
                char bytes[4096];
                std::size_t end = std::string::npos, size = 0;
                while (true) {
                    auto n = ::recv(c, bytes, sizeof bytes, 0);
                    if (n <= 0)
                        break;
                    request.append(bytes, n);
                    end = request.find("\r\n\r\n");
                    if (end != std::string::npos) {
                        auto p = request.find("Content-Length:");
                        if (p != std::string::npos) {
                            try {
                                size = std::stoul(request.substr(p + 15));
                            } catch (...) {
                            }
                        }
                        if (request.size() >= end + 4 + size)
                            break;
                    }
                }
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    requests_.push_back(request);
                }
                if (r.disconnect) {
                    ::close(c);
                    continue;
                }
                auto body = r.raw ? *r.raw : r.body.dump();
                for (std::size_t p = 0; (p = body.find("$BASE", p)) != std::string::npos;
                     p += base.size())
                    body.replace(p, 5, base);
                auto response =
                    "HTTP/1.1 " + std::to_string(r.status) +
                    " Fixture\r\nConnection: close\r\nContent-Type: "
                    "application/json\r\nX-Private: HEADER_SENTINEL\r\nContent-Length: " +
                    std::to_string(body.size()) + "\r\n\r\n" + body;
                for (std::size_t p = 0; p < response.size();) {
                    auto n = ::send(c, response.data() + p, response.size() - p, MSG_NOSIGNAL);
                    if (n <= 0)
                        break;
                    p += static_cast<std::size_t>(n);
                }
                ::close(c);
            }
        });
    }
    std::vector<std::string> requests() {
        std::lock_guard<std::mutex> lock(mutex_);
        return requests_;
    }
    ~Fixture() {
        ::shutdown(fd_, SHUT_RDWR);
        ::close(fd_);
        if (thread_.joinable())
            thread_.join();
    }
};
Json metadata() {
    return {{"issuer", "$BASE"},
            {"device_authorization_endpoint", "$BASE/device"},
            {"token_endpoint", "$BASE/token"},
            {"revocation_endpoint", "$BASE/revoke"},
            {"token_endpoint_auth_methods_supported", {"none"}},
            {"grant_types_supported",
             {"urn:ietf:params:oauth:grant-type:device_code", "refresh_token"}}};
}
Json device() {
    return {{"device_code", "private-device"},
            {"user_code", "SAFE-CODE"},
            {"verification_uri", "$BASE/activate"},
            {"verification_uri_complete", "$BASE/activate?user_code=SAFE-CODE"},
            {"expires_in", 600},
            {"interval", 7}};
}
std::string b64(const std::string &data) {
    std::string result(4 * ((data.size() + 2) / 3), '\0');
    EVP_EncodeBlock(reinterpret_cast<unsigned char *>(result.data()),
                    reinterpret_cast<const unsigned char *>(data.data()),
                    static_cast<int>(data.size()));
    while (!result.empty() && result.back() == '=')
        result.pop_back();
    for (char &c : result) {
        if (c == '+')
            c = '-';
        if (c == '/')
            c = '_';
    }
    return result;
}
Json token(const std::string &org = "org_test", const std::string &refresh = "refresh-old",
           const std::string &client = "public-test") {
    Json claims = {{"sub", "user_test"}, {"org_id", org}, {"exp", std::time(nullptr) + 3600}};
    if (!client.empty())
        claims["client_id"] = client;
    Json t = {{"access_token",
               b64("{\"typ\":\"at+jwt\"}") + "." + b64(claims.dump()) + ".fake-signature"},
              {"token_type", "Bearer"},
              {"expires_in", 3000}};
    if (!refresh.empty())
        t["refresh_token"] = refresh;
    return t;
}
template <class F> std::string fails(F f) {
    try {
        f();
    } catch (const std::exception &e) {
        return e.what();
    }
    throw std::runtime_error("expected failure");
}
okou::Config config(Fixture &f, const std::string &dir) {
    okou::Config c;
    c.api_origin = f.base;
    c.oauth_issuer = f.base;
    c.oauth_client_id = "public-test";
    c.oauth_resource =
        "https://api.okou.ai/mcp"; // Fixed wire fixture, independent of build profile.
    c.data_dir = dir;
    return c;
}
void put(const std::string &path, const std::string &value, mode_t mode = 0600) {
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, mode);
    CHECK(fd >= 0);
    CHECK(::write(fd, value.data(), value.size()) == static_cast<ssize_t>(value.size()));
    CHECK(::close(fd) == 0);
    CHECK(::chmod(path.c_str(), mode) == 0);
}
Json record(const std::string &path) {
    std::ifstream in(path);
    CHECK(in.good());
    Json j;
    in >> j;
    return j;
}
void diagnostic_record(const std::string &line, const char *stage, const char *category,
                       const char *reason, long status, int curl, const char *oauth,
                       bool uncertain = false) {
    CHECK(!line.empty() && line.size() < 768 && line.back() == '\n');
    CHECK(line.find('\n') == line.size() - 1);
    for (const auto *secret : {"SENTINEL", "private-device", "SAFE-CODE", "public-test",
                               "user_test", "org_test", "127.0.0.1", "https://", "tokens.json"})
        CHECK(line.find(secret) == std::string::npos);
    const Json expected = {
        {"schema", 1},           {"component", "okou-zero"}, {"version", OKOU_ZERO_VERSION},
        {"stage", stage},        {"category", category},     {"reason", reason},
        {"http_status", status}, {"curl_code", curl},        {"oauth_error", oauth},
        {"uncertain", uncertain}};
    CHECK(Json::parse(line) == expected);
}
void diagnostic_tests(const std::string &root, okou::Http &http) {
    {
        auto d = device();
        d["untrusted"] = "BODY_SENTINEL";
        Fixture f({{200, metadata()},
                   {200, d},
                   {400, {{"error", "authorization_pending"}, {"error_description", "SENTINEL"}}},
                   {400, {{"error", "slow_down"}}},
                   {403, {{"error", "access_denied"}, {"error_description", "SENTINEL"}}},
                   {400, {{"error", "expired_token"}}},
                   {200, token()}});
        okou::OAuth auth(config(f, root + "/diag-terminal"), http);
        DiagnosticCapture capture;
        auto code = auth.begin();
        CHECK(capture.take().empty());
        CHECK(auth.poll(code).state == okou::PollState::pending);
        CHECK(capture.take().empty());
        CHECK(auth.poll(code).state == okou::PollState::slow_down);
        CHECK(capture.take().empty());
        CHECK(auth.poll(code).state == okou::PollState::denied);
        diagnostic_record(capture.take(), "token_polling", "http", "polling_rejected", 403, 0,
                          "access_denied");
        CHECK(auth.poll(code).state == okou::PollState::expired);
        diagnostic_record(capture.take(), "token_polling", "http", "polling_rejected", 400, 0,
                          "expired_token");
        CHECK(auth.poll(code).state == okou::PollState::authorized);
        CHECK(capture.take().empty());
    }
    {
        Fixture f({{200, metadata()},
                   {400,
                    {{"error", "invalid_client"},
                     {"error_description", "BODY_SENTINEL"},
                     {"access_token", "TOKEN_SENTINEL"},
                     {"id_token", "ID_SENTINEL"}}}});
        auto c = config(f, root + "/diag-device");
        c.oauth_client_id = "CLIENT_SENTINEL";
        c.oauth_resource = "https://resource-SENTINEL.example/mcp";
        okou::OAuth auth(c, http);
        DiagnosticCapture capture;
        CHECK(!fails([&] { auth.begin(); }).empty());
        diagnostic_record(capture.take(), "device_authorization", "http", "oauth_rejected", 400, 0,
                          "invalid_client");
    }
    for (const auto &value : {Json("unknown_SENTINEL"), Json("invalid_grant\nSENTINEL"),
                              Json({{"SENTINEL", true}}), Json(nullptr)}) {
        Fixture f({{200, metadata()},
                   {200, device()},
                   {400, {{"error", value}, {"error_description", "BODY_SENTINEL"}}}});
        okou::OAuth auth(config(f, root + "/diag-invalid"), http);
        auto code = auth.begin();
        DiagnosticCapture capture;
        CHECK(!fails([&] { auth.poll(code); }).empty());
        // Null retains str()'s existing absent-field behavior; status still rejects it.
        const bool rejected = value.is_string() || value.is_null();
        diagnostic_record(capture.take(), "token_polling", rejected ? "http" : "exception",
                          rejected ? "polling_rejected" : "invalid_oauth_field", 400, 0, "unknown");
    }
    {
        Fixture f({{200, metadata()},
                   {200, device()},
                   {400, Json(), std::string("{\"error\":PARSER_SENTINEL")}});
        okou::OAuth auth(config(f, root + "/diag-json"), http);
        auto code = auth.begin();
        DiagnosticCapture capture;
        CHECK(!fails([&] { auth.poll(code); }).empty());
        diagnostic_record(capture.take(), "token_polling", "http", "invalid_json_response", 400, 0,
                          "unknown");
    }
    {
        Fixture f({{200, metadata()}, {200, device()}, {0, Json(), {}, true}});
        okou::OAuth auth(config(f, root + "/diag-transport"), http);
        auto code = auth.begin();
        code.device_code = "DEVICE_SENTINEL";
        DiagnosticCapture capture;
        CHECK(!fails([&] { auth.poll(code); }).empty());
        diagnostic_record(capture.take(), "token_polling", "transport", "network_or_tls_failed", 0,
                          CURLE_GOT_NOTHING, "none", true);
    }
    {
        Fixture f(
            {{200, metadata()},
             {503,
              {{"error", "temporarily_unavailable"}, {"error_description", "BODY_SENTINEL"}}}});
        auto c = config(f, root + "/diag-refresh");
        okou::OAuth auth(c, http);
        okou::Tokens previous;
        previous.access_token = "ACCESS_SENTINEL";
        previous.refresh_token = "REFRESH_SENTINEL";
        DiagnosticCapture capture;
        CHECK(fails([&] { auth.refresh(previous); }) == "Refresh failed; authorize device again");
        diagnostic_record(capture.take(), "refresh", "http", "server_rejected", 503, 0,
                          "temporarily_unavailable", true);
        CHECK(record(c.data_dir + "/tokens.json")["reauthorization_required"] == true);
        CHECK(auth.load().access_token.empty());
    }
    {
        auto bad = token();
        bad["access_token"] = "ACCESS_SENTINEL";
        Fixture f({{200, metadata()}, {200, device()}, {200, bad}});
        okou::OAuth auth(config(f, root + "/diag-validation"), http);
        auto code = auth.begin();
        DiagnosticCapture capture;
        CHECK(!fails([&] { auth.poll(code); }).empty());
        diagnostic_record(capture.take(), "token_validation", "exception", "jwt_required", 200, 0,
                          "none");
    }
    {
        Fixture f({{400, {{"error", "invalid_request"}, {"error_description", "SENTINEL"}}}});
        auto c = config(f, root + "/diag-resource");
        c.oauth_issuer.clear();
        okou::OAuth auth(c, http);
        DiagnosticCapture capture;
        CHECK(!fails([&] { auth.poll({}); }).empty());
        diagnostic_record(capture.take(), "resource_discovery", "http", "oauth_rejected", 400, 0,
                          "invalid_request");
    }
    {
        Fixture f({{400, {{"error", "invalid_request"}}}});
        okou::OAuth auth(config(f, root + "/diag-issuer"), http);
        DiagnosticCapture capture;
        CHECK(!fails([&] { auth.poll({}); }).empty());
        diagnostic_record(capture.take(), "issuer_discovery", "http", "oauth_rejected", 400, 0,
                          "invalid_request");
    }
    {
        auto m = metadata();
        m["issuer"] = "https://ISSUER_SENTINEL.example";
        Fixture f({{200, m}});
        okou::OAuth auth(config(f, root + "/diag-metadata"), http);
        DiagnosticCapture capture;
        CHECK(!fails([&] { auth.begin(); }).empty());
        diagnostic_record(capture.take(), "issuer_discovery", "exception", "issuer_mismatch", 200,
                          0, "none");
    }
    for (const auto *issuer : {"http://ISSUER_SENTINEL.example",
                               "https://ISSUER_SENTINEL.example?query=SECRET_SENTINEL"}) {
        Fixture f(
            {{200,
              {{"authorization_servers", {issuer}}, {"resource", "https://api.okou.ai/mcp"}}}});
        auto c = config(f, root + "/diag-local-issuer");
        c.oauth_issuer.clear();
        okou::OAuth auth(c, http);
        DiagnosticCapture capture;
        CHECK(!fails([&] { auth.begin(); }).empty());
        diagnostic_record(capture.take(), "issuer_discovery", "exception",
                          std::string(issuer).find('?') == std::string::npos
                              ? "insecure_oauth_endpoint"
                              : "invalid_oauth_issuer",
                          0, 0, "none");
        CHECK(f.requests().size() == 1); // Local validation must not request the issuer.
    }
    for (bool fallback : {false, true}) {
        std::vector<Reply> replies = {
            {200, {{"authorization_servers", {"$BASE"}}, {"resource", "https://api.okou.ai/mcp"}}}};
        if (fallback)
            replies.push_back({404, {{"error", "UNKNOWN_SENTINEL"}}});
        replies.push_back({0, Json(), {}, true});
        Fixture f(std::move(replies));
        auto c = config(f, root + "/diag-discovery-transport");
        c.oauth_issuer.clear();
        okou::OAuth auth(c, http);
        DiagnosticCapture capture;
        CHECK(!fails([&] { auth.poll({}); }).empty());
        diagnostic_record(capture.take(), "issuer_discovery", "transport", "network_or_tls_failed",
                          0, CURLE_GOT_NOTHING, "none");
    }
    {
        Fixture f({{200, metadata()},
                   {200, device()},
                   {400, {{"error", "invalid_token"}, {"error_description", "SENTINEL"}}}});
        const auto c = config(f, root + "/diag-revoke-rejected");
        okou::OAuth auth(c, http);
        (void)auth.begin();
        okou::Tokens t;
        t.refresh_token = "REFRESH_SENTINEL";
        auth.save(t);
        DiagnosticCapture capture;
        auth.logout(t); // Best effort stays nonthrowing after the status rejection.
        diagnostic_record(capture.take(), "logout_revocation", "http", "oauth_rejected", 400, 0,
                          "invalid_token");
        CHECK(!std::filesystem::exists(c.data_dir + "/tokens.json"));
    }
    {
        Fixture f({{200, metadata()}, {200, device()}, {503, {{"error", "server_error"}}}});
        okou::OAuth auth(config(f, root + "/diag-revoke"), http);
        (void)auth.begin();
        okou::Tokens t;
        t.refresh_token = "REFRESH_SENTINEL";
        auth.save(t);
        DiagnosticCapture capture;
        auth.logout(t);
        diagnostic_record(capture.take(), "logout_revocation", "http", "server_rejected", 503, 0,
                          "server_error", true);
        CHECK(auth.load().access_token.empty());
    }
}
void tests() {
    ::setenv("OKOU_ZERO_TEST_ALLOW_LOOPBACK_HTTP", "1", 1);
    char temp[] = "/tmp/okou-auth-test-XXXXXX";
    CHECK(::mkdtemp(temp));
    std::string root = temp;
    ::setenv("XDG_CONFIG_HOME", (root + "/config").c_str(), 1);
    ::setenv("XDG_DATA_HOME", (root + "/data").c_str(), 1);
    ::unsetenv("OKOU_ZERO_CONFIG");
    ::unsetenv("OKOU_ZERO_DATA_DIR");
    okou::Http http;
    diagnostic_tests(root, http);
    {
        Fixture f(
            {{200, metadata()},
             {200, device()},
             {400,
              {{"error", "authorization_pending"}, {"error_description", "PRIVATE provider text"}}},
             {400, {{"error", "slow_down"}}},
             {400, {{"error", "access_denied"}}},
             {400, {{"error", "expired_token"}}},
             {200, token()},
             {200, token("org_test", "refresh-rotated")},
             {200, token("org_test", "")},
             {503, {{"error", "PRIVATE"}}}});
        auto c = config(f, root + "/credentials");
        c.scopes = "openid profile offline_access user:org:read";
        okou::OAuth auth(c, http);
        auto d = auth.begin();
        CHECK(d.interval == 7);
        CHECK(d.expires_in == 600);
        CHECK(d.user_code == "SAFE-CODE");
        CHECK(auth.poll(d).state == okou::PollState::pending);
        CHECK(auth.poll(d).state == okou::PollState::slow_down);
        CHECK(auth.poll(d).state == okou::PollState::denied);
        CHECK(auth.poll(d).state == okou::PollState::expired);
        auto result = auth.poll(d);
        CHECK(result.state == okou::PollState::authorized);
        auto t = result.tokens;
        CHECK(t.user_id == "user_test");
        CHECK(t.org_id == "org_test");
        CHECK(t.client_id == "public-test");
        CHECK(t.expires_at <= std::time(nullptr) + 3000);
        CHECK(auth.load().access_token.empty());
        auth.save(t);
        auto loaded = auth.load();
        CHECK(loaded.access_token == t.access_token);
        CHECK(loaded.org_id == "org_test");
        CHECK(loaded.expires_at == t.expires_at);
        struct stat st {};
        CHECK(::stat((c.data_dir + "/tokens.json").c_str(), &st) == 0);
        CHECK((st.st_mode & 0777) == 0600);
        CHECK(::stat(c.data_dir.c_str(), &st) == 0);
        CHECK((st.st_mode & 0777) == 0700);
        auto saved = record(c.data_dir + "/tokens.json");
        CHECK(saved["grant_binding"]["api_origin"] == c.api_origin);
        CHECK(saved["grant_binding"]["oauth_issuer"] == c.oauth_issuer);
        CHECK(saved["grant_binding"]["oauth_resource"] == c.oauth_resource);
        CHECK(saved["grant_binding"]["client_id"] == c.oauth_client_id);
        CHECK(saved["resolved_oauth_issuer"] == f.base);
        Json tampered = saved;
        tampered["org_id"] = "org_attacker";
        tampered["user_id"] = "user_attacker";
        tampered["expires_at"] = std::time(nullptr) + 999999;
        put(c.data_dir + "/tokens.json", tampered.dump());
        CHECK(auth.load().org_id == "org_test");
        CHECK(auth.load().user_id == "user_test");
        CHECK(auth.load().expires_at <= std::time(nullptr) + 3600);
        auto rotated = auth.refresh(t);
        CHECK(rotated.refresh_token == "refresh-rotated");
        auto retained = auth.refresh(rotated);
        CHECK(retained.refresh_token == "refresh-rotated");
        auth.save(retained);
        auth.logout(retained);
        CHECK(auth.load().access_token.empty());
        auto requests = f.requests();
        CHECK(requests.size() == 10);
        auto form_body = [&](std::size_t index, const std::string &path) {
            const auto &request = requests[index];
            CHECK(request.rfind("POST " + path + " HTTP/", 0) == 0);
            CHECK(request.find("\r\nContent-Type: application/x-www-form-urlencoded\r\n") !=
                  std::string::npos);
            auto separator = request.find("\r\n\r\n");
            CHECK(separator != std::string::npos);
            return request.substr(separator + 4);
        };
        CHECK(form_body(1, "/device") ==
              "client_id=public-test&scope=openid%20profile%20offline_access%20user%3Aorg%3Aread");
        for (std::size_t i = 2; i <= 6; ++i) {
            CHECK(form_body(i, "/token") ==
                  "grant_type=urn%3Aietf%3Aparams%3Aoauth%3Agrant-type%3Adevice_code"
                  "&device_code=private-device&client_id=public-test");
        }
        CHECK(form_body(7, "/token") ==
              "grant_type=refresh_token&refresh_token=refresh-old&client_id=public-test");
        CHECK(form_body(8, "/token") ==
              "grant_type=refresh_token&refresh_token=refresh-rotated&client_id=public-test");
        for (const auto &request : requests) {
            CHECK(request.find("client_secret") == std::string::npos);
            CHECK(request.find("organization_id") == std::string::npos);
            CHECK(request.find("Authorization:") == std::string::npos);
        }
    }
    {
        Fixture f({{200, metadata()},
                   {200, device()},
                   {200, token()},
                   {503, {{"error", "temporarily_unavailable"}}}});
        auto c = config(f, root + "/uncertain-refresh");
        okou::OAuth auth(c, http);
        auto authorized = auth.poll(auth.begin()).tokens;
        auth.save(authorized);
        CHECK(!fails([&] { auth.refresh(authorized); }).empty());
        CHECK(auth.load().access_token.empty());
        auto marker = record(c.data_dir + "/tokens.json");
        CHECK(marker["reauthorization_required"] == true);
        CHECK(!marker.contains("refresh_token") && !marker.contains("access_token"));
        CHECK(!fails([&] { auth.refresh(authorized); }).empty());
        okou::OAuth restarted(c, http);
        CHECK(restarted.load().access_token.empty());
        CHECK(!fails([&] { restarted.refresh(authorized); }).empty());
        CHECK(f.requests().size() == 4); // No automatic reuse, even after restart.
    }
    {
        auto m = metadata();
        Json resource = {{"authorization_servers", {"$BASE"}},
                         {"resource", "https://api.okou.ai/mcp"}};
        Fixture f({{200, resource},
                   {404, {{"error", "not_found"}}},
                   {200, m},
                   {200, device()},
                   {200, token()},
                   {200, resource},
                   {200, m},
                   {200, token("org_test", "refresh-discovered")}});
        auto c = config(f, root + "/discovery");
        c.oauth_issuer.clear();
        okou::OAuth auth(c, http);
        auto d = auth.begin();
        CHECK(d.device_code == "private-device");
        auto r = f.requests();
        CHECK(r[0].find("GET /.well-known/oauth-protected-resource/mcp ") != std::string::npos);
        CHECK(r[2].find("GET /.well-known/oauth-authorization-server ") != std::string::npos);
        auto t = auth.poll(d).tokens;
        auth.save(t);
        CHECK(auth.config().oauth_issuer.empty());
        auto saved = record(c.data_dir + "/tokens.json");
        CHECK(saved["grant_binding"]["oauth_issuer"] == "");
        CHECK(saved["resolved_oauth_issuer"] == f.base);
        okou::OAuth restarted(c, http);
        CHECK(restarted.load().access_token == t.access_token);
        CHECK(f.requests().size() == 5);
        auto refreshed = restarted.refresh(restarted.load());
        CHECK(refreshed.refresh_token == "refresh-discovered");
        restarted.save(refreshed);
        CHECK(record(c.data_dir + "/tokens.json")["grant_binding"] == saved["grant_binding"]);
        okou::OAuth again(c, http);
        CHECK(again.load().refresh_token == "refresh-discovered");
        CHECK(f.requests().size() == 8);
        c.oauth_issuer = f.base;
        okou::OAuth explicit_issuer(c, http);
        CHECK(explicit_issuer.load().access_token.empty());
        CHECK(f.requests().size() == 8);
    }
    {
        // HTTPS configuration identities are checked offline, without discovery.
        Fixture f({});
        auto c = config(f, root + "/binding");
        c.api_origin = "https://api.original.example";
        c.oauth_issuer = "https://issuer.original.example";
        c.oauth_resource = "https://api.original.example/mcp";
        std::atomic_bool cancelled{true};
        okou::Http offline(&cancelled);
        okou::OAuth original(c, offline);
        // Absence of a JWT client claim must not bypass the configured client
        // binding (parse_tokens otherwise falls back to the configured ID).
        okou::Tokens t;
        t.access_token = token("org_test", "", "")["access_token"].get<std::string>();
        t.refresh_token = "refresh-private";
        t.client_id = c.oauth_client_id;
        original.save(t);
        CHECK(original.load().access_token == t.access_token);
        const auto path = c.data_dir + "/tokens.json";
        const auto saved = record(path);
        auto reject = [&](const okou::Config &changed) {
            okou::OAuth other(changed, offline);
            auto loaded = other.load();
            CHECK(loaded.access_token.empty());
            CHECK(loaded.refresh_token.empty());
            CHECK(loaded.client_id.empty());
            CHECK(loaded.expires_at == 0);
            CHECK(record(path) == saved);
            CHECK(f.requests().empty());
        };
        auto changed = c;
        changed.api_origin = "https://api.other.example";
        reject(changed);
        changed = c;
        changed.oauth_client_id = "other-public-client";
        reject(changed);
        changed = c;
        changed.oauth_resource = "https://api.original.example/other-resource";
        reject(changed);
        changed = c;
        changed.oauth_issuer = "https://issuer.other.example";
        reject(changed);
        changed = c;
        changed.oauth_issuer.clear();
        reject(changed);
        changed = c;
        changed.api_origin += "/";
        okou::OAuth trailing_slash(changed, offline);
        CHECK(trailing_slash.load().access_token == t.access_token);
        changed = c;
        changed.oauth_resource += "/";
        reject(changed);
        for (const auto &field :
             {"api_origin", "oauth_resource", "oauth_issuer", "client_id", "version"}) {
            auto broken = saved;
            broken["grant_binding"].erase(field);
            put(path, broken.dump());
            CHECK(original.load().access_token.empty());
            CHECK(original.load().refresh_token.empty());
        }
        for (const auto &binding : {Json(), Json("legacy"), Json::array(), Json::object()}) {
            auto broken = saved;
            broken["grant_binding"] = binding;
            put(path, broken.dump());
            CHECK(original.load().access_token.empty());
        }
        auto legacy = saved;
        legacy.erase("grant_binding");
        put(path, legacy.dump());
        CHECK(original.load().access_token.empty());
        CHECK(original.load().refresh_token.empty());
        legacy["access_token"] = "not-even-a-jwt";
        put(path, legacy.dump());
        CHECK(original.load().access_token.empty());
        CHECK(f.requests().empty());
        // Exercise the application's startup bearer guard with a live observer
        // and uncancelled transport, not just a cancelled offline transport.
        put(path, saved.dump());
        Fixture observer({{200, Json::object()}});
        changed = c;
        changed.api_origin = observer.base;
        okou::OAuth startup(changed, http);
        auto loaded = startup.load();
        if (!loaded.access_token.empty())
            (void)http.request("GET", observer.base + "/api/chat-threads", "", loaded.access_token);
        CHECK(loaded.access_token.empty());
        CHECK(loaded.refresh_token.empty());
        CHECK(observer.requests().empty());
        // Rejected caches are not migrated/rebound. Local logout is mandatory
        // even when cancellation prevents discovery and remote revocation.
        changed = c;
        changed.oauth_issuer = "https://issuer.other.example";
        okou::OAuth other(changed, offline);
        other.logout(other.load());
        CHECK(!std::filesystem::exists(path));
        CHECK(f.requests().empty());
    }
    {
        Fixture f({{200, metadata()}, {200, device()}, {200, token("")}});
        okou::OAuth auth(config(f, root + "/org"), http);
        auto d = auth.begin();
        CHECK(fails([&] { auth.poll(d); }).find("Org-bound") != std::string::npos);
    }
    {
        Fixture f(
            {{200, metadata()},
             {200, device()},
             {400, {{"error", "invalid_client"}, {"error_description", "PRIVATE client secret"}}}});
        okou::OAuth auth(config(f, root + "/error"), http);
        auto d = auth.begin();
        CHECK(fails([&] { auth.poll(d); }).find("PRIVATE") == std::string::npos);
    }
    {
        auto m = metadata();
        m["issuer"] = "https://wrong.example";
        Fixture f({{200, m}});
        okou::OAuth auth(config(f, root + "/issuer"), http);
        CHECK(fails([&] { auth.begin(); }).find("issuer mismatch") != std::string::npos);
    }
    {
        auto m = metadata();
        m["token_endpoint_auth_methods_supported"] = {"client_secret_basic"};
        Fixture f({{200, m}});
        okou::OAuth auth(config(f, root + "/public"), http);
        CHECK(fails([&] { auth.begin(); }).find("public clients") != std::string::npos);
    }
    {
        auto m = metadata();
        m["grant_types_supported"] = {"authorization_code"};
        Fixture f({{200, m}});
        okou::OAuth auth(config(f, root + "/grant"), http);
        CHECK(fails([&] { auth.begin(); }).find("device grant") != std::string::npos);
    }
    {
        Fixture f({});
        auto c = config(f, root + "/missing");
        c.oauth_client_id.clear();
        okou::OAuth auth(c, http);
        CHECK(fails([&] { auth.begin(); }).find("CLIENT_ID") != std::string::npos);
    }
    {
        Fixture f({});
        auto c = config(f, root + "/cancel");
        std::atomic_bool cancelled{true};
        okou::Http hc(&cancelled);
        okou::OAuth auth(c, hc);
        okou::Tokens t;
        t.access_token = "private";
        auth.save(t);
        auth.logout(t);
        CHECK(auth.load().access_token.empty());
    }
    {
        auto c = okou::load_config();
        CHECK(c.data_dir == root + "/data/okou-zero");
        c.oauth_client_id = "configured-public";
        c.oauth_issuer = "https://issuer.example";
        okou::save_config(c);
        CHECK(okou::load_config().oauth_client_id == "configured-public");
        ::setenv("OKOU_ZERO_OAUTH_CLIENT_ID", "environment-public", 1);
        CHECK(okou::load_config().oauth_client_id == "environment-public");
        ::unsetenv("OKOU_ZERO_OAUTH_CLIENT_ID");
        put(root + "/explicit.json", "{\"oauth_client_id\":\"explicit\"}");
        CHECK(okou::load_config(root + "/explicit.json").oauth_client_id == "explicit");
        ::chmod((root + "/explicit.json").c_str(), 0644);
        CHECK(fails([&] { okou::load_config(root + "/explicit.json"); }).find("permissions") !=
              std::string::npos);
        ::chmod((root + "/explicit.json").c_str(), 0600);
        put(root + "/explicit.json", "{ PRIVATE SECRET");
        CHECK(fails([&] { okou::load_config(root + "/explicit.json"); }).find("PRIVATE") ==
              std::string::npos);
        CHECK(::symlink((root + "/explicit.json").c_str(), (root + "/symlink.json").c_str()) == 0);
        CHECK(!fails([&] { okou::load_config(root + "/symlink.json"); }).empty());
        CHECK(::symlink(root.c_str(), (root + "/linkdir").c_str()) == 0);
        CHECK(!fails([&] { okou::load_config(root + "/linkdir/explicit.json"); }).empty());
        put(root + "/source.json", "{}");
        CHECK(::link((root + "/source.json").c_str(), (root + "/hardlink.json").c_str()) == 0);
        CHECK(!fails([&] { okou::load_config(root + "/hardlink.json"); }).empty());
        CHECK(::mkdir((root + "/unsafe").c_str(), 0755) == 0);
        CHECK(!fails([&] { okou::load_config(root + "/unsafe/config.json"); }).empty());
        CHECK(::mkfifo((root + "/fifo").c_str(), 0600) == 0);
        CHECK(!fails([&] { okou::load_config(root + "/fifo"); }).empty());
    }
    std::filesystem::remove_all(root);
}
} // namespace
int main() {
    try {
        tests();
        std::cout << "auth_test passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
