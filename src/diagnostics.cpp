// SPDX-License-Identifier: MIT
#include "okou/diagnostics.hpp"
#include <cstdio>
#include <cstring>
#include <curl/curl.h>

namespace okou::diagnostics {
const char *oauth_error_code(std::string_view code) noexcept {
    // Equality is bounded; return the literal, not the matching input.
    for (const char *allowed :
         {"none", "authorization_pending", "slow_down", "access_denied", "expired_token",
          "invalid_request", "invalid_client", "invalid_grant", "unauthorized_client",
          "unsupported_grant_type", "invalid_scope", "server_error", "temporarily_unavailable",
          "invalid_token", "insufficient_scope"}) {
        if (code == allowed)
            return allowed;
    }
    return "unknown";
}
const char *oauth_error_code(const Json &body) noexcept {
    try {
        if (!body.is_object())
            return "unknown";
        auto i = body.find("error");
        if (i == body.end())
            return "none";
        if (!i->is_string())
            return "unknown";
        const auto &code = i->get_ref<const std::string &>();
        // "none" is our absence sentinel, not a standard provider error code.
        return code == "none" ? "unknown" : oauth_error_code(std::string_view(code));
    } catch (...) {
        return "unknown";
    }
}
const char *response_oauth_error(const std::string &body) noexcept {
    try {
        return oauth_error_code(Json::parse(body, nullptr, false));
    } catch (...) {
        return "unknown";
    }
}
namespace {
const char *stage_name(Stage stage) noexcept {
    switch (stage) {
    case Stage::resource_discovery:
        return "resource_discovery";
    case Stage::issuer_discovery:
        return "issuer_discovery";
    case Stage::device_authorization:
        return "device_authorization";
    case Stage::token_polling:
        return "token_polling";
    case Stage::token_validation:
        return "token_validation";
    case Stage::refresh:
        return "refresh";
    case Stage::logout_revocation:
        return "logout_revocation";
    case Stage::application:
        return "application";
    case Stage::voice:
        return "voice";
    case Stage::startup:
        return "startup";
    }
    return "unknown";
}
struct Reason {
    const char *text;
    const char *code;
};
const char *reason(const std::exception &error) noexcept {
    // Parser errors may contain body fragments. Never serialize what(), even
    // for recognized errors: compare only, and emit a compiled literal.
    static constexpr Reason reasons[] = {
        {"OAuth response missing required field", "missing_oauth_field"},
        {"Invalid OAuth response field", "invalid_oauth_field"},
        {"Invalid OAuth numeric field", "invalid_oauth_number"},
        {"Invalid OAuth endpoint URL", "invalid_oauth_endpoint"},
        {"OAuth requires secure URLs without credentials", "insecure_oauth_endpoint"},
        {"OAuth request rejected", "oauth_rejected"},
        {"Invalid OAuth response", "invalid_oauth_response"},
        {"Org-bound JWT access token required; check Clerk application setup", "jwt_required"},
        {"Invalid access token claims", "invalid_token_claims"},
        {"Absolute credential directory required", "invalid_credential_directory"},
        {"Set OKOU_ZERO_OAUTH_CLIENT_ID to an approved Clerk public device client; see "
         "docs/auth-setup.md",
         "oauth_client_required"},
        {"No OAuth issuer advertised by resource metadata", "missing_oauth_issuer"},
        {"OAuth resource metadata mismatch", "resource_mismatch"},
        {"OAuth issuer must not contain a query", "invalid_oauth_issuer"},
        {"OAuth metadata issuer mismatch", "issuer_mismatch"},
        {"OAuth provider does not advertise public clients", "public_client_unavailable"},
        {"OAuth device grant unavailable; enable it for the public Clerk application",
         "device_grant_unavailable"},
        {"Invalid OAuth device timing", "invalid_device_timing"},
        {"Missing OAuth device code", "missing_device_code"},
        {"Invalid OAuth polling response", "invalid_polling_response"},
        {"OAuth token polling rejected", "polling_rejected"},
        {"Invalid OAuth token response", "invalid_token_response"},
        {"Unsupported OAuth token type", "unsupported_token_type"},
        {"Org-bound access token required; reauthorize and select a workspace",
         "org_token_required"},
        {"Access token client does not match configured public client", "token_client_mismatch"},
        {"Access token expiry missing", "missing_token_expiry"},
        {"Invalid OAuth expiry", "invalid_token_expiry"},
        {"Authorize device again before refreshing", "reauthorization_required"},
        {"No refresh token; authorize device again", "missing_refresh_token"},
        {"Refresh failed; authorize device again", "refresh_failed"},
        {"HTTP initialization failed", "http_initialization_failed"},
        {"Request identity generation failed", "request_identity_failed"},
        {"Invalid JSON response", "invalid_json_response"},
        {"Invalid or oversized server protocol response", "invalid_server_protocol"},
        {"Invalid thread identity", "invalid_thread_identity"},
        {"Cannot reserve pending send identity", "send_reservation_failed"},
        {"Cannot persist pending send identity", "send_identity_write_failed"},
        {"Cannot durably persist pending send identity", "send_identity_sync_failed"},
        {"Secure random source unavailable", "random_source_unavailable"},
        {"Request cancelled", "cancelled"},
        {"Invalid HTTP method", "invalid_http_method"},
        {"Invalid request URL", "invalid_request_url"},
        {"HTTPS URL without credentials required", "insecure_request_url"},
        {"Invalid authorization header", "invalid_authorization_header"},
        {"HTTP header allocation failed", "header_allocation_failed"},
        {"Invalid request header", "invalid_request_header"},
        {"Reserved request header", "reserved_request_header"},
        {"Response exceeds size limit", "response_size_limit"},
        {"Request timed out", "timeout"},
        {"Network or TLS request failed", "network_or_tls_failed"},
        {"Server request failed", "server_rejected"}};
    for (const auto &r : reasons)
        if (std::strcmp(error.what(), r.text) == 0)
            return r.code;
    return "unknown_error";
}
const char *version() noexcept {
    // Build metadata is trusted, but keep even an accidental exotic value out
    // of the fixed-size JSON buffer and JSON string syntax.
    constexpr char value[] = OKOU_ZERO_VERSION;
    if (sizeof value > 65)
        return "unknown";
    for (char c : value)
        if (c && !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                   c == '.' || c == '-' || c == '+'))
            return "unknown";
    return OKOU_ZERO_VERSION;
}
} // namespace
std::string format_error(Stage stage, const std::exception &error,
                         const HttpResponse *failed_response) {
    const auto *http = dynamic_cast<const HttpError *>(&error);
    const long raw_status = failed_response && failed_response->status ? failed_response->status
                            : http                                     ? http->status
                                                                       : 0;
    const long status = raw_status >= 100 && raw_status <= 599 ? raw_status : 0;
    const auto curl =
        http && http->curl_code > 0 && http->curl_code < CURL_LAST ? http->curl_code : 0;
    const char *category = http ? (curl ? "transport" : "http")
                           : dynamic_cast<const Json::exception *>(&error) ? "json"
                                                                           : "exception";
    char line[768];
    const int n = std::snprintf(
        line, sizeof line,
        "{\"schema\":1,\"component\":\"okou-zero\",\"version\":\"%s\",\"stage\":\"%s\","
        "\"category\":\"%s\",\"reason\":\"%s\",\"http_status\":%ld,\"curl_code\":%d,"
        "\"oauth_error\":\"%s\",\"uncertain\":%s}\n",
        version(), stage_name(stage), category, reason(error), status, curl,
        failed_response && failed_response->status &&
                (failed_response->status < 200 || failed_response->status >= 300 ||
                 stage == Stage::token_polling)
            ? response_oauth_error(failed_response->body)
        : http ? oauth_error_code(std::string_view(http->oauth_error))
               : "none",
        http && http->uncertain ? "true" : "false");
    if (n < 0 || static_cast<std::size_t>(n) >= sizeof line)
        return {}; // Never emit a truncated record.
    return std::string(line, static_cast<std::size_t>(n));
}
void log_error(Stage stage, const std::exception &error,
               const HttpResponse *failed_response) noexcept {
    try {
        const auto line = format_error(stage, error, failed_response);
        // One stdio operation keeps concurrent diagnostics from interleaving.
        // Diagnostic I/O failure must never replace the original failure.
        (void)std::fwrite(line.data(), 1, line.size(), stderr);
    } catch (...) {
    }
}
void log_terminal(Stage stage, const HttpResponse &response) noexcept {
    try {
        const HttpError error(response.status,
                              stage == Stage::logout_revocation ? "OAuth request rejected"
                                                                : "OAuth token polling rejected",
                              false, 0, response_oauth_error(response.body));
        log_error(stage, error);
    } catch (...) {
    }
}
} // namespace okou::diagnostics
