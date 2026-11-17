// SPDX-License-Identifier: MIT
#include "okou/core.hpp"
#include <algorithm>
#include <ctime>
#include <curl/curl.h>
#include <filesystem>
#include <limits>
#include <openssl/evp.h>

namespace okou {
namespace detail {
std::string data_home();
Json read_private(const std::string &);
void write_private(const std::string &, const Json &);
void remove_private(const std::string &);
} // namespace detail
namespace {
std::runtime_error auth_error(const char *text) {
    return std::runtime_error(text);
}
std::string str(const Json &j, const char *field, bool required = false) {
    auto i = j.find(field);
    if (i == j.end() || i->is_null()) {
        if (required)
            throw auth_error("OAuth response missing required field");
        return {};
    }
    if (!i->is_string())
        throw auth_error("Invalid OAuth response field");
    auto s = i->get<std::string>();
    if (s.size() > 64 * 1024 || s.find('\0') != std::string::npos)
        throw auth_error("Invalid OAuth response field");
    if (required && s.empty())
        throw auth_error("OAuth response missing required field");
    return s;
}
std::int64_t number(const Json &j, const char *field, std::int64_t fallback) {
    auto i = j.find(field);
    if (i == j.end())
        return fallback;
    if (!i->is_number_integer())
        throw auth_error("Invalid OAuth numeric field");
    if (i->is_number_unsigned() &&
        i->get<std::uint64_t>() >
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        throw auth_error("Invalid OAuth numeric field");
    return i->get<std::int64_t>();
}
void secure_url(const std::string &url) {
    std::unique_ptr<CURLU, decltype(&curl_url_cleanup)> u(curl_url(), curl_url_cleanup);
    if (!u || url.find('\0') != std::string::npos ||
        curl_url_set(u.get(), CURLUPART_URL, url.c_str(), 0))
        throw auth_error("Invalid OAuth endpoint URL");
    auto part = [&](CURLUPart key) {
        char *v = nullptr;
        std::string r;
        if (!curl_url_get(u.get(), key, &v, 0)) {
            r = v;
            curl_free(v);
        }
        return r;
    };
    auto scheme = part(CURLUPART_SCHEME), host = part(CURLUPART_HOST);
    const char *fixture = std::getenv("OKOU_ZERO_TEST_ALLOW_LOOPBACK_HTTP");
    if ((scheme != "https" && !(scheme == "http" && (host == "127.0.0.1" || host == "[::1]") &&
                                fixture && std::string(fixture) == "1")) ||
        !part(CURLUPART_USER).empty() || !part(CURLUPART_PASSWORD).empty() ||
        !part(CURLUPART_FRAGMENT).empty())
        throw auth_error("OAuth requires secure URLs without credentials");
}
std::string form(const std::vector<std::pair<std::string, std::string>> &fields) {
    std::string out;
    for (const auto &p : fields) {
        if (p.second.empty())
            continue;
        if (!out.empty())
            out += '&';
        out += Http::encode(p.first) + "=" + Http::encode(p.second);
    }
    return out;
}
HttpResponse post(Http &h, const std::string &url,
                  const std::vector<std::pair<std::string, std::string>> &fields) {
    secure_url(url);
    return h.request("POST", url, form(fields), {},
                     {{"Content-Type", "application/x-www-form-urlencoded"}});
}
Json checked(const HttpResponse &r) {
    if (r.status < 200 || r.status >= 300)
        throw HttpError(r.status, "OAuth request rejected");
    auto j = r.json();
    if (!j.is_object())
        throw auth_error("Invalid OAuth response");
    return j;
}
Json claims(const std::string &token) {
    auto a = token.find('.'), b = a == std::string::npos ? a : token.find('.', a + 1);
    if (a == std::string::npos || b == std::string::npos ||
        token.find('.', b + 1) != std::string::npos || b == a + 1)
        throw auth_error("Org-bound JWT access token required; check Clerk application setup");
    std::string encoded = token.substr(a + 1, b - a - 1);
    if (encoded.size() > 32768 || encoded.size() % 4 == 1)
        throw auth_error("Invalid access token claims");
    for (char &c : encoded) {
        if (c == '-')
            c = '+';
        else if (c == '_')
            c = '/';
        else if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')))
            throw auth_error("Invalid access token claims");
    }
    auto unpadded = encoded.size();
    while (encoded.size() % 4)
        encoded += '=';
    std::string decoded(encoded.size() / 4 * 3, '\0');
    int n = EVP_DecodeBlock(reinterpret_cast<unsigned char *>(decoded.data()),
                            reinterpret_cast<const unsigned char *>(encoded.data()),
                            static_cast<int>(encoded.size()));
    if (n < 0)
        throw auth_error("Invalid access token claims");
    decoded.resize(static_cast<std::size_t>(n) - (encoded.size() - unpadded));
    try {
        auto j = Json::parse(decoded);
        if (!j.is_object())
            throw auth_error("Invalid access token claims");
        return j;
    } catch (const Json::exception &) {
        throw auth_error("Invalid access token claims");
    }
}
Json grant_binding(const Config &c) {
    // Normalize only trailing slashes on the API origin. Resource/client IDs and
    // the configured issuer (including empty = discovery) are exact identities;
    // do not discover metadata merely to decide whether a cache may be loaded.
    std::string origin = c.api_origin;
    while (!origin.empty() && origin.back() == '/')
        origin.pop_back();
    return {{"version", 1},
            {"api_origin", origin},
            {"oauth_resource", c.oauth_resource},
            {"oauth_issuer", c.oauth_issuer},
            {"client_id", c.oauth_client_id}};
}
std::string token_path(const Config &c) {
    if (c.data_dir.empty() || c.data_dir[0] != '/')
        throw auth_error("Absolute credential directory required");
    return (std::filesystem::path(c.data_dir) / "tokens.json").string();
}
} // namespace
OAuth::OAuth(Config config, Http &http) : config_(std::move(config)), http_(http) {
    if (config_.data_dir.empty())
        config_.data_dir = detail::data_home();
}
void OAuth::discover() {
    if (!metadata_.is_null())
        return;
    if (config_.oauth_client_id.empty())
        throw auth_error("Set OKOU_ZERO_OAUTH_CLIENT_ID to an approved Clerk public device client; "
                         "see docs/auth-setup.md");
    std::string issuer = config_.oauth_issuer;
    if (issuer.empty()) {
        std::string origin = config_.api_origin;
        while (!origin.empty() && origin.back() == '/')
            origin.pop_back();
        auto resource =
            checked(http_.request("GET", origin + "/.well-known/oauth-protected-resource/mcp"));
        auto it = resource.find("authorization_servers");
        if (it == resource.end() || !it->is_array() || it->empty() || !(*it)[0].is_string())
            throw auth_error("No OAuth issuer advertised by resource metadata");
        issuer = (*it)[0].get<std::string>();
        if (resource.contains("resource") &&
            str(resource, "resource", true) != config_.oauth_resource)
            throw auth_error("OAuth resource metadata mismatch");
    }
    secure_url(issuer);
    if (issuer.find('?') != std::string::npos)
        throw auth_error("OAuth issuer must not contain a query");
    while (!issuer.empty() && issuer.back() == '/')
        issuer.pop_back();
    auto r = http_.request("GET", issuer + "/.well-known/openid-configuration");
    if (r.status == 404) {
        const auto slash = issuer.find('/', issuer.find("://") + 3);
        const std::string origin = issuer.substr(0, slash),
                          path = slash == std::string::npos ? "" : issuer.substr(slash);
        r = http_.request("GET", origin + "/.well-known/oauth-authorization-server" + path);
    }
    auto m = checked(r);
    auto advertised = str(m, "issuer", true);
    while (!advertised.empty() && advertised.back() == '/')
        advertised.pop_back();
    if (advertised != issuer)
        throw auth_error("OAuth metadata issuer mismatch");
    secure_url(str(m, "token_endpoint", true));
    secure_url(str(m, "device_authorization_endpoint", true));
    if (m.contains("revocation_endpoint"))
        secure_url(str(m, "revocation_endpoint", true));
    if (m.contains("token_endpoint_auth_methods_supported") &&
        (!m["token_endpoint_auth_methods_supported"].is_array() ||
         std::find(m["token_endpoint_auth_methods_supported"].begin(),
                   m["token_endpoint_auth_methods_supported"].end(),
                   Json("none")) == m["token_endpoint_auth_methods_supported"].end()))
        throw auth_error("OAuth provider does not advertise public clients");
    if (m.contains("grant_types_supported") &&
        (!m["grant_types_supported"].is_array() ||
         std::find(m["grant_types_supported"].begin(), m["grant_types_supported"].end(),
                   Json("urn:ietf:params:oauth:grant-type:device_code")) ==
             m["grant_types_supported"].end()))
        throw auth_error(
            "OAuth device grant unavailable; enable it for the public Clerk application");
    // Keep config_ as the caller's binding, especially the empty issuer used
    // for resource discovery. The validated resolved issuer lives in metadata_.
    metadata_ = std::move(m);
}
DeviceCode OAuth::begin() {
    discover();
    auto j = checked(post(http_, str(metadata_, "device_authorization_endpoint", true),
                          {{"client_id", config_.oauth_client_id},
                           {"scope", config_.scopes},
                           {"resource", config_.oauth_resource}}));
    DeviceCode c;
    c.device_code = str(j, "device_code", true);
    c.user_code = str(j, "user_code", true);
    c.verification_uri = str(j, "verification_uri", true);
    c.verification_uri_complete = str(j, "verification_uri_complete");
    secure_url(c.verification_uri);
    if (!c.verification_uri_complete.empty())
        secure_url(c.verification_uri_complete);
    auto interval = number(j, "interval", 5), expiry = number(j, "expires_in", 600);
    if (interval < 1 || interval > 3600 || expiry < 1 || expiry > 86400)
        throw auth_error("Invalid OAuth device timing");
    c.interval = static_cast<int>(interval);
    c.expires_in = static_cast<int>(expiry);
    return c;
}
PollResult OAuth::poll(const DeviceCode &code) {
    discover();
    if (code.device_code.empty())
        throw auth_error("Missing OAuth device code");
    auto r = post(http_, str(metadata_, "token_endpoint", true),
                  {{"grant_type", "urn:ietf:params:oauth:grant-type:device_code"},
                   {"device_code", code.device_code},
                   {"client_id", config_.oauth_client_id},
                   {"resource", config_.oauth_resource}});
    auto j = r.json();
    if (!j.is_object())
        throw auth_error("Invalid OAuth polling response");
    auto e = str(j, "error");
    if (r.status == 400 && e == "authorization_pending")
        return {PollState::pending, {}};
    if (r.status == 400 && e == "slow_down")
        return {PollState::slow_down, {}};
    if ((r.status == 400 || r.status == 403) && e == "access_denied")
        return {PollState::denied, {}};
    if (r.status == 400 && e == "expired_token")
        return {PollState::expired, {}};
    if (!e.empty() || r.status < 200 || r.status >= 300)
        throw HttpError(r.status, "OAuth token polling rejected");
    return {PollState::authorized, parse_tokens(j)};
}
Tokens OAuth::parse_tokens(const Json &body, const Tokens &previous) const {
    try {
        if (!body.is_object())
            throw auth_error("Invalid OAuth token response");
        Tokens t;
        t.access_token = str(body, "access_token", true);
        t.refresh_token = str(body, "refresh_token");
        if (t.refresh_token.empty())
            t.refresh_token = previous.refresh_token;
        auto type = str(body, "token_type");
        std::transform(type.begin(), type.end(), type.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        if (!type.empty() && type != "bearer")
            throw auth_error("Unsupported OAuth token type");
        // Deliberately UNVERIFIED. Claims address UI/realtime channels only;
        // the API validates signature, issuer, audience, membership and scopes.
        auto j = claims(t.access_token);
        t.user_id = str(j, "sub", true);
        t.org_id = str(j, "org_id");
        if (t.org_id.empty() && j.contains("o") && j["o"].is_object())
            t.org_id = str(j["o"], "id");
        t.client_id = str(j, "client_id");
        auto identifier = [](const std::string &s) {
            return s.size() <= 2048 && std::all_of(s.begin(), s.end(), [](unsigned char c) {
                       return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                              (c >= '0' && c <= '9') || c == '_' || c == '-';
                   });
        };
        if (t.user_id.rfind("user_", 0) != 0 || t.org_id.rfind("org_", 0) != 0 ||
            !identifier(t.user_id) || !identifier(t.org_id))
            throw auth_error("Org-bound access token required; reauthorize and select a workspace");
        if (!t.client_id.empty() && t.client_id != config_.oauth_client_id)
            throw auth_error("Access token client does not match configured public client");
        if (t.client_id.empty())
            t.client_id = config_.oauth_client_id;
        t.expires_at = number(j, "exp", 0);
        if (t.expires_at <= 0)
            throw auth_error("Access token expiry missing");
        auto seconds = number(body, "expires_in", -1);
        if (seconds != -1) {
            if (seconds <= 0 || seconds > 365LL * 86400)
                throw auth_error("Invalid OAuth expiry");
            t.expires_at =
                std::min(t.expires_at, static_cast<std::int64_t>(std::time(nullptr)) + seconds);
        }
        return t;
    } catch (const Json::exception &) {
        throw auth_error("Invalid OAuth token response");
    }
}
Tokens OAuth::refresh(const Tokens &previous) {
    auto cached = detail::read_private(token_path(config_));
    if (cached.value("reauthorization_required", false) &&
        cached.value("grant_binding", Json::object()) == grant_binding(config_))
        throw HttpError(401, "Authorize device again before refreshing");
    discover();
    if (previous.refresh_token.empty())
        throw HttpError(401, "No refresh token; authorize device again");
    try {
        return parse_tokens(checked(post(http_, str(metadata_, "token_endpoint", true),
                                         {{"grant_type", "refresh_token"},
                                          {"refresh_token", previous.refresh_token},
                                          {"client_id", config_.oauth_client_id},
                                          {"resource", config_.oauth_resource}})),
                            previous);
    } catch (...) {
        // The provider may have rotated the token even when its response was lost.
        // Persist a token-free barrier so reconnects and process restarts cannot
        // automatically reuse an uncertain refresh generation. Fresh login replaces it.
        detail::write_private(token_path(config_), {{"grant_binding", grant_binding(config_)},
                                                    {"reauthorization_required", true}});
        throw HttpError(401, "Refresh failed; authorize device again");
    }
}
Tokens OAuth::load() const {
    auto j = detail::read_private(token_path(config_));
    if (j.empty() || j.value("reauthorization_required", false))
        return {};
    // Fail closed before parsing or using either credential. Legacy records
    // cannot prove their grant binding and must be replaced by a fresh login.
    auto binding = j.find("grant_binding");
    if (binding == j.end() || *binding != grant_binding(config_))
        return {};
    auto t = parse_tokens(j);
    auto persisted = number(j, "expires_at", t.expires_at);
    if (persisted > 0)
        t.expires_at = std::min(t.expires_at, persisted);
    return t;
}
void OAuth::save(const Tokens &t) const {
    // Persist only one credential generation, atomically; no ID token or secrets.
    Json record = {{"access_token", t.access_token},
                   {"refresh_token", t.refresh_token},
                   {"user_id", t.user_id},
                   {"org_id", t.org_id},
                   {"client_id", t.client_id},
                   {"expires_at", t.expires_at},
                   {"grant_binding", grant_binding(config_)}};
    // Provenance only: startup compares configured values without network I/O.
    if (!metadata_.is_null())
        record["resolved_oauth_issuer"] = str(metadata_, "issuer", true);
    detail::write_private(token_path(config_), record);
}
void OAuth::logout(const Tokens &t) {
    // Local clearing is mandatory even while cancellation prevents revocation.
    detail::remove_private(token_path(config_));
    try {
        discover();
        if (!metadata_.contains("revocation_endpoint"))
            return;
        const auto token = t.refresh_token.empty() ? t.access_token : t.refresh_token;
        if (!token.empty())
            (void)post(
                http_, str(metadata_, "revocation_endpoint", true),
                {{"token", token},
                 {"token_type_hint", t.refresh_token.empty() ? "access_token" : "refresh_token"},
                 {"client_id", config_.oauth_client_id}});
    } catch (...) { /* best effort revocation; never expose provider bodies */
    }
}
} // namespace okou
