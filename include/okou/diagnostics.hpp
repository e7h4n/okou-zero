// SPDX-License-Identifier: MIT
#pragma once
#include "okou/core.hpp"
#include <string_view>

namespace okou::diagnostics {
enum class Stage {
    resource_discovery,
    issuer_discovery,
    device_authorization,
    token_polling,
    token_validation,
    refresh,
    logout_revocation,
    application,
    startup
};
// Every returned string is a compiled constant, never provider-controlled text.
const char *oauth_error_code(std::string_view code) noexcept;
const char *oauth_error_code(const Json &body) noexcept;
const char *response_oauth_error(const std::string &body) noexcept;
// One bounded JSON line, including the final newline. No exception text is copied.
std::string format_error(Stage stage, const std::exception &error,
                         const HttpResponse *failed_response = nullptr);
void log_error(Stage stage, const std::exception &error,
               const HttpResponse *failed_response = nullptr) noexcept;
void log_terminal(Stage stage, const HttpResponse &response) noexcept;
} // namespace okou::diagnostics
