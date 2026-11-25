// SPDX-License-Identifier: MIT
#include "../src/idle_display.hpp"
#include "okou/core.hpp"
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
    check(c.api_origin == "https://api.okou.ai");
    check(c.oauth_resource == c.api_origin + "/mcp");
    check(c.data_dir == root + "/data/okou-zero");
    c.oauth_client_id = "user-public-client";
    okou::save_config(c);
    check(std::filesystem::exists(root + "/config/okou-zero/config.json"));
    check(okou::load_config().oauth_client_id == "user-public-client");
    ::setenv("OKOU_ZERO_OAUTH_CLIENT_ID", "runtime-public-client", 1);
    check(okou::load_config().oauth_client_id == "runtime-public-client");
    ::setenv("OKOU_ZERO_OAUTH_CLIENT_ID", "", 1);
    check(okou::load_config().oauth_client_id.empty());
    using namespace std::chrono;
    const auto settings = root + "/launcher.json";
    check(okou::launcher_dark_time(settings) == seconds(30));
    std::ofstream(settings) << R"({"dark_time":0})";
    const auto never = okou::launcher_dark_time(settings);
    check(never == seconds(0));
    std::ofstream(settings) << R"({"dark_time":10})";
    check(okou::launcher_dark_time(settings) == seconds(10));
    const auto backlight = root + "/brightness";
    std::ofstream(backlight) << 47;
    auto brightness = [&] {
        int n = -1;
        std::ifstream(backlight) >> n;
        return n;
    };
    {
        okou::IdleDisplay screen(backlight, seconds(10));
        screen.expire(screen.deadline() - milliseconds(1));
        check(!screen.asleep() && brightness() == 47);
        screen.expire(screen.deadline());
        check(screen.asleep() && brightness() == 0);
        screen.wake();
        check(!screen.asleep() && brightness() == 47);
        screen.expire(screen.deadline());
    }
    check(brightness() == 47);
    okou::IdleDisplay disabled(backlight, never);
    disabled.expire(steady_clock::now() + hours(1));
    check(!disabled.asleep() && brightness() == 47);
    std::filesystem::remove_all(root);
}
