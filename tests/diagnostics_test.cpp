// SPDX-License-Identifier: MIT
#include "diagnostic_capture.hpp"
#include "okou/diagnostics.hpp"
#include <curl/curl.h>
#include <iostream>
#include <limits>

#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x))                                                                                  \
            throw std::runtime_error("check failed: " #x);                                         \
    } while (false)
namespace {
using namespace okou;
using namespace okou::diagnostics;
Json check_record(const std::string &line) {
    CHECK(!line.empty() && line.size() < 768 && line.back() == '\n');
    CHECK(line.find('\n') == line.size() - 1);
    auto j = Json::parse(line);
    CHECK(j.size() == 10);
    CHECK(j["schema"] == 1);
    CHECK(j["component"] == "okou-zero");
    CHECK(j["version"] == OKOU_ZERO_VERSION);
    CHECK(j["stage"].is_string() && j["category"].is_string() && j["reason"].is_string());
    CHECK(j["http_status"].is_number_integer() && j["curl_code"].is_number_integer());
    CHECK(j["oauth_error"].is_string() && j["uncertain"].is_boolean());
    return j;
}
void tests() {
    HttpError e(403, "OAuth request rejected", true, CURLE_RECV_ERROR, "invalid_client");
    auto line = format_error(Stage::refresh, e);
    auto j = check_record(line);
    CHECK(j["stage"] == "refresh" && j["category"] == "transport");
    CHECK(j["reason"] == "oauth_rejected" && j["http_status"] == 403);
    CHECK(j["curl_code"] == CURLE_RECV_ERROR && j["oauth_error"] == "invalid_client");
    CHECK(j["uncertain"] == true);
    {
        DiagnosticCapture capture;
        log_error(Stage::refresh, e);
        CHECK(capture.take() == line);
    }
    for (const auto code :
         {"authorization_pending", "slow_down", "access_denied", "expired_token", "invalid_request",
          "invalid_client", "invalid_grant", "unauthorized_client", "unsupported_grant_type",
          "invalid_scope", "server_error", "temporarily_unavailable", "invalid_token",
          "insufficient_scope"}) {
        HttpError allowed(400, "OAuth request rejected", false, 0, code);
        CHECK(check_record(format_error(Stage::token_polling, allowed))["oauth_error"] == code);
    }
    for (const std::string text :
         {std::string("SECRET\n{\"stage\":\"injected\"}"),
          std::string("OAuth request rejected\nSECRET"), std::string(1024 * 1024, 'S')}) {
        std::runtime_error unknown(text);
        auto record = check_record(format_error(static_cast<Stage>(999), unknown));
        CHECK(record["reason"] == "unknown_error");
        CHECK(record["stage"] == "unknown");
        CHECK(record["uncertain"] == false);
        HttpError injected(400, text, false, 0, text);
        CHECK(injected.oauth_error == "unknown");
        CHECK(check_record(format_error(Stage::application, injected))["oauth_error"] == "unknown");
    }
    // Public mutable fields are not a trusted bypass of the constructor.
    e.oauth_error = "invalid_grant\nSECRET";
    e.status = std::numeric_limits<long>::max();
    e.curl_code = std::numeric_limits<int>::max();
    j = check_record(format_error(Stage::application, e));
    CHECK(j["http_status"] == 0 && j["curl_code"] == 0 && j["oauth_error"] == "unknown");
    CHECK(j["uncertain"] == true);
    for (long status : {std::numeric_limits<long>::min(), -1L, 0L, 99L, 600L}) {
        e.status = status;
        CHECK(check_record(format_error(Stage::startup, e))["http_status"] == 0);
    }
    for (long status : {100L, 200L, 599L}) {
        e.status = status;
        CHECK(check_record(format_error(Stage::startup, e))["http_status"] == status);
    }
    for (int code : {std::numeric_limits<int>::min(), -1, 0, static_cast<int>(CURL_LAST)}) {
        e.curl_code = code;
        CHECK(check_record(format_error(Stage::startup, e))["curl_code"] == 0);
    }
    CHECK(std::string(response_oauth_error("{SECRET")) == "unknown");
    CHECK(std::string(response_oauth_error("{\"error\":{\"SECRET\":true}}")) == "unknown");
    CHECK(std::string(response_oauth_error("{\"error\":\"invalid_grant\\nSECRET\"}")) == "unknown");
    try {
        (void)Json::parse("{\"SECRET_PARSER\":injected}");
        CHECK(false);
    } catch (const Json::exception &parser) {
        auto record = format_error(Stage::application, parser);
        CHECK(record.find("SECRET") == std::string::npos);
        CHECK(check_record(record)["category"] == "json");
    }
    HttpError defaults(401, "unknown");
    CHECK(defaults.curl_code == 0 && defaults.oauth_error == "none" && !defaults.uncertain);
}
} // namespace
int main() {
    try {
        tests();
        std::cout << "diagnostics_test passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
