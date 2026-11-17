// SPDX-License-Identifier: MIT
#include "okou/core.hpp"
#include <cstdlib>
#include <condition_variable>
#include <iostream>
#include <set>

// Internal pure reducer seam; no WebSocket, provider, API, or microphone starts.
namespace okou::realtime_detail {
std::vector<std::string> subscription_channels(const std::string&, const std::string&);
Json begin(const std::vector<std::string>&);
void lost(Json&);
Json step(Json&, const Json&);
Json token_request_body(const Json&, const std::string&);
bool notification_name(const std::string&);
}
namespace {
void check(bool value, const char* message) {
    if (!value) { std::cerr << message << '\n'; std::exit(1); }
}
void expect_failure(const std::function<void()>& fn, const char* message) {
    try { fn(); } catch (...) { return; }
    check(false, message);
}
}
int main() {
    using okou::Json;
    using namespace okou::realtime_detail;
    const std::string a = "user-org:user_abc:org_abc", b = "user:user_abc";
    const auto channels = subscription_channels("user_abc", "org_abc");
    check(channels == std::vector<std::string>{a}, "Production channel set is exactly workspace-only");
    auto state = begin(channels);
    check(!state["ready"].get<bool>(), "Initial state is not connected");
    auto effects = step(state, {{"action", 11}, {"channel", a}});
    check(!state["ready"].get<bool>(), "Ignore ATTACHED before CONNECTED");
    effects = step(state, {{"action", 4}, {"connectionDetails", {{"connectionKey", "private-resume-key"}}}});
    check(effects["send"].size() == 1, "CONNECTED attaches only the workspace channel");
    check(state["key"] == "private-resume-key", "Nested connection key saved for resume");
    std::set<std::string> attached;
    for (const auto& attach : effects["send"]) {
        check(attach["action"] == 10 && attach["flags"] == (1 << 18), "Only subscribe ATTACH is sent");
        attached.insert(attach["channel"].get<std::string>());
    }
    check(attached == std::set<std::string>{a}, "No global user channel is subscribed");
    effects = step(state, {{"action", 11}, {"channel", b}});
    check(!state["ready"].get<bool>() && effects["events"].empty(), "Own global-user ATTACHED cannot establish health");
    effects = step(state, {{"action", 11}, {"channel", "user-org:user_abc:org_other"}});
    check(!state["ready"].get<bool>() && effects["events"].empty(), "Foreign workspace ATTACHED cannot establish health");
    effects = step(state, {{"action", 11}, {"channel", a}, {"channelSerial", "s1"}});
    check(state["ready"].get<bool>() && effects["events"] == Json::array({"connection:gap", "connection:connected"}), "Workspace ATTACHED alone emits gap then connected");
    effects = step(state, {{"action", 4}});
    check(effects["send"].empty() && effects["events"].empty(), "AUTH CONNECTED does not reattach or catch up");
    effects = step(state, {{"action", 15}, {"channel", a}, {"channelSerial", "s2"},
        {"messages", Json::array({{{"name", "threadListChanged"}, {"data", "SECRET RAW PAYLOAD"}},
                                  {{"name", "chatIndicatorsChanged"}},
                                  {{"name", "unknown-secret-event"}},
                                  {{"name", "chatThreadMessageCreated:01234567-89ab-cdef-0123-456789abcdef"}},
                                  {{"name", 42}}, nullptr})}});
    check(effects["events"].size() == 3, "Only whitelisted valid notification names emitted");
    check(effects.dump().find("SECRET") == std::string::npos &&
          effects.dump().find("private-resume-key") == std::string::npos, "No payloads or keys in effects");
    check(state["serials"][a] == "s2", "Channel serial tracked");
    effects = step(state, {{"action", 15}, {"channel", b},
                           {"messages", Json::array({{{"name", "threadListChanged"}}})}});
    check(effects["events"].empty(), "Ignore own global-user channel messages");
    effects = step(state, {{"action", 11}, {"channel", a}, {"flags", 4}});
    check(effects["events"].empty(), "Duplicate continuous attachment causes no gap");
    effects = step(state, {{"action", 11}, {"channel", a}, {"flags", 0}});
    check(effects["events"] == Json::array({"connection:gap"}), "In-place channel gap catches up");
    effects = step(state, {{"action", 17}, {"auth", {{"accessToken", "secret-server-field"}}}});
    check(effects["auth"].get<bool>() && effects.dump().find("secret-server-field") == std::string::npos, "AUTH requests serialized renewal without payload disclosure");
    effects = step(state, {{"action", 6}, {"error", {{"statusCode", 401}, {"code", 40142}, {"message", "SECRET"}}}});
    check(effects["reconnect"].get<bool>() && effects["auth"].get<bool>() &&
          !effects["fatal"].get<bool>() && !state["ready"].get<bool>() &&
          effects["events"] == Json::array({"connection:disconnected"}), "Token disconnect notifies, renews and reconnects");
    effects = step(state, {{"action", 4}, {"connectionKey", "new-resume-key"}});
    check(effects["send"][0]["channelSerial"] == "s2", "Resume includes last channel serial");
    check(effects["send"].size() == 1 && effects["send"][0]["channel"] == a,
          "Reconnect still attaches exactly the workspace channel");
    effects = step(state, {{"action", 11}, {"channel", a}, {"flags", 4}});
    check(effects["events"] == Json::array({"connection:gap", "connection:connected"}), "Conservative catch-up even after apparent resume");
    effects = step(state, {{"action", 9}, {"channel", a}, {"error", {{"statusCode", 403}, {"code", 40160}}}});
    check(effects["fatal"].get<bool>() && !state["ready"].get<bool>(), "Capability denial is terminal, not a retry storm");
    state = begin(channels);
    for (const auto& malformed : Json::array({nullptr, 1, "text", Json::array(),
                                             {{"action", "CONNECTED"}}, {{"action", UINT64_MAX}},
                                             {{"action", 987654}}, {{"action", 0}}})) {
        effects = step(state, malformed);
        check(effects["send"].empty() && effects["events"].empty(), "Malformed and unknown frames do not trigger work");
    }
    check(!notification_name("chatThreadDetailChanged:not-a-uuid") &&
          !notification_name("chatThreadDetailChanged:01234567-89ab-cdef-0123-456789abcdeZ"), "Reject unsafe event suffixes");
    Json request{{"keyName", "app.key"}, {"timestamp", 123456789}, {"nonce", "nonce"},
                 {"mac", "secret-signature"}, {"ttl", 3600000}, {"clientId", "user_abc"},
                 {"capability", "{\"user-org:user_abc:org_abc\":[\"subscribe\"]}"}, {"unexpected", "discarded"}};
    const auto body = token_request_body(request, "user_abc");
    check(body["mac"] == request["mac"] && body["capability"] == request["capability"] &&
          !body.contains("unexpected"), "TokenRequest signed fields forwarded exactly");
    request["clientId"] = "other";
    expect_failure([&] { token_request_body(request, "user_abc"); }, "Reject wrong token identity");
    request["clientId"] = "user_abc"; request["keyName"] = "../../bad?token=secret";
    expect_failure([&] { token_request_body(request, "user_abc"); }, "Reject injected token endpoint");
    expect_failure([&] { token_request_body(Json::object(), "user_abc"); }, "Reject missing signed token fields");
    int calls = 0;
    okou::Realtime realtime([&] { ++calls; return Json::object(); }, "user_abc", "org_abc", [](const std::string&) {});
    check(!realtime.connected() && calls == 0, "Constructor does not start network or request a token");
    realtime.stop(); realtime.stop();
    check(calls == 0, "Stop before start is safe and does not authenticate");
    std::mutex mutex;
    std::condition_variable cv;
    bool callback_seen = false;
    okou::Realtime* self = nullptr;
    std::atomic_int provider_calls{0};
    okou::Realtime callback_stop([&] { ++provider_calls; return Json::object(); },
        "user_abc", "org_abc", [&](const std::string& name) {
            check(name == "connection:disconnected", "Startup announces unavailable, no secrets");
            self->stop(); // Must not self-join or wait on the owner's lifecycle mutex.
            std::lock_guard<std::mutex> lock(mutex);
            callback_seen = true; cv.notify_all();
        });
    self = &callback_stop;
    callback_stop.start();
    {
        std::unique_lock<std::mutex> lock(mutex);
        check(cv.wait_for(lock, std::chrono::seconds(2), [&] { return callback_seen; }), "Worker callback stop completes");
    }
    callback_stop.stop();
    check(provider_calls == 0 && !callback_stop.connected(), "Callback shutdown never authenticates");
    bool requested = false;
    okou::Realtime retry([&] {
        ++provider_calls;
        { std::lock_guard<std::mutex> lock(mutex); requested = true; cv.notify_all(); }
        return Json::object(); // Invalid before HTTP exchange: deterministic offline failure.
    }, "user_abc", "org_abc", [](const std::string&) {});
    retry.start(); retry.start();
    {
        std::unique_lock<std::mutex> lock(mutex);
        check(cv.wait_for(lock, std::chrono::seconds(2), [&] { return requested; }), "Provider runs on explicit start");
    }
    const auto stop_started = std::chrono::steady_clock::now();
    retry.stop(); retry.stop();
    check(std::chrono::steady_clock::now() - stop_started < std::chrono::seconds(1), "Stop interrupts retry backoff");
    check(provider_calls == 1, "Duplicate start does not create concurrent token providers");
    std::cout << "realtime_test passed (offline protocol reducer, no network)\n";
}
