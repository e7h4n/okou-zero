// SPDX-License-Identifier: MIT
#include "okou/core.hpp"
#include "update_process.hpp"
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <unistd.h>

namespace {
void check(bool value) {
    if (!value)
        throw std::runtime_error("Bundled configuration check failed");
}
} // namespace
int main() {
    char temporary[] = "/tmp/okou-config-test-XXXXXX";
    const char *directory = ::mkdtemp(temporary);
    check(directory != nullptr);
    const std::string root(directory);
    for (const auto *name : {"OKOU_ZERO_CONFIG", "OKOU_ZERO_API_ORIGIN", "OKOU_ZERO_OAUTH_ISSUER",
                             "OKOU_ZERO_OAUTH_CLIENT_ID", "OKOU_ZERO_OAUTH_RESOURCE",
                             "OKOU_ZERO_DATA_DIR", "OKOU_ZERO_SCOPES"})
        ::unsetenv(name);
    ::setenv("XDG_CONFIG_HOME", (root + "/config").c_str(), 1);
    ::setenv("XDG_DATA_HOME", (root + "/data").c_str(), 1);
    auto c = okou::load_config();
    check(c.oauth_client_id == okou::build::oauth_client_id);
    check(c.oauth_issuer.empty()); // Resource metadata remains authoritative for issuer discovery.
    check(c.oauth_resource == c.api_origin + "/mcp");
    const bool staging = std::string(okou::build::profile) == "staging";
    check(c.api_origin == (staging ? "https://pr-48263-api.vm6.ai" : "https://api.okou.ai"));
    check(c.data_dir == root + "/data/" + (staging ? "okou-zero-staging" : "okou-zero"));
    check(okou::build::stable_updates == !staging);
    if (staging) {
        check(!c.oauth_client_id.empty());
        okou::UpdateProcess updater;
        check(!updater.start(false, "0.2.3"));
        check(!updater.start(true, "0.2.3"));
        check(updater.view().status == okou::UpdateStatus::idle);
    }
    c.oauth_client_id = "user-public-client";
    okou::save_config(c);
    const auto file = root + "/config/" + okou::build::storage_name + "/config.json";
    check(std::filesystem::exists(file));
    check(okou::load_config().oauth_client_id == "user-public-client");
    ::setenv("OKOU_ZERO_OAUTH_CLIENT_ID", "runtime-public-client", 1);
    check(okou::load_config().oauth_client_id == "runtime-public-client");
    ::setenv("OKOU_ZERO_OAUTH_CLIENT_ID", "", 1);
    check(okou::load_config().oauth_client_id.empty());
    std::filesystem::remove_all(root);
}
