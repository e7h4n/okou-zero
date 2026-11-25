// SPDX-License-Identifier: MIT
#include "okou/core.hpp"
#include <condition_variable>
#include <cstdlib>
#include <iostream>

// Internal pure reducer seam; no WebSocket, provider, API, or microphone starts.
namespace okou::realtime_detail {
Json begin(const std::string &, const std::string &, const std::string &);
Json step(Json &, const Json &);
Json token_request_body(const Json &, const std::string &);
} // namespace okou::realtime_detail
namespace {
void check(bool value, const char *message) {
    if (!value) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}
void expect_failure(const std::function<void()> &fn, const char *message) {
    try {
        fn();
    } catch (...) {
        return;
    }
    check(false, message);
}
} // namespace
int main() {
    using okou::Json;
    using namespace okou::realtime_detail;
    const std::string channel = "user-org:user_abc:org_abc";
    const std::string thread = "01234567-89ab-cdef-0123-456789abcdef";
    auto state = begin("user_abc", "org_abc", thread);
    auto effects =
        step(state, {{"action", 4}, {"connectionDetails", {{"connectionKey", "resume-key"}}}});
    const Json attach{{"action", 10}, {"channel", channel}, {"flags", 1 << 18}};
    check(effects["send"] == Json::array({attach}), "Subscribe to the account's workspace");
    check(state["key"] == "resume-key", "Keep the connection key for resume");

    auto attached =
        Json{{"action", 11}, {"channel", channel}, {"channelSerial", "s1"}, {"flags", 4}};
    check(step(state, attached)["refresh"] == true, "First attachment catches up history");
    check(step(state, attached)["refresh"] == false, "Continuous attachment keeps history");
    attached["flags"] = 0;
    check(step(state, attached)["refresh"] == true, "An in-place gap catches up history");

    auto message = Json{{"action", 15}, {"channel", channel}, {"channelSerial", "s2"}};
    for (const auto *prefix : {"chatThreadMessageCreated:", "chatThreadDetailChanged:"}) {
        message["messages"] = Json::array({{{"name", prefix + thread}}});
        check(step(state, message)["refresh"] == true, "Fixed-chat event refreshes history");
    }
    message["channel"] = "user-org:user_abc:org_other";
    check(step(state, message)["refresh"] == false, "Foreign workspace event is ignored");
    message["channel"] = channel;
    message["messages"] =
        Json::array({{{"name", "chatThreadMessageCreated:aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"}}});
    check(step(state, message)["refresh"] == false, "Another chat leaves this reader alone");
    check(step(state, {{"action", 17}})["auth"] == true, "AUTH initiates renewal");
    effects = step(state, {{"action", 4}});
    check(effects["send"].empty() && effects["refresh"] == false,
          "AUTH confirmation keeps the attachment");
    effects = step(state, {{"action", 6}, {"error", {{"statusCode", 401}, {"code", 40142}}}});
    check(effects["reconnect"] == true && effects["auth"] == true && effects["fatal"] == false,
          "Expired token reconnects with renewed credentials");
    effects = step(state, {{"action", 4}});
    auto resumed = attach;
    resumed["channelSerial"] = "s2";
    check(effects["send"] == Json::array({resumed}), "Resume uses the last channel serial");
    attached["flags"] = 4;
    check(step(state, attached)["refresh"] == true, "Catch up on the new transport");
    effects = step(
        state,
        {{"action", 9}, {"channel", channel}, {"error", {{"statusCode", 403}, {"code", 40160}}}});
    check(effects["fatal"] == true, "Capability denial ends reconnection");
    Json request{{"keyName", "app.key"},
                 {"timestamp", 123456789},
                 {"nonce", "nonce"},
                 {"mac", "secret-signature"},
                 {"ttl", 3600000},
                 {"clientId", "user_abc"},
                 {"capability", "{\"user-org:user_abc:org_abc\":[\"subscribe\"]}"},
                 {"unexpected", "discarded"}};
    const auto body = token_request_body(request, "user_abc");
    check(body["mac"] == request["mac"] && body["capability"] == request["capability"] &&
              !body.contains("unexpected"),
          "TokenRequest signed fields forwarded exactly");
    request["clientId"] = "other";
    expect_failure([&] { token_request_body(request, "user_abc"); }, "Reject wrong token identity");
    request["clientId"] = "user_abc";
    request["keyName"] = "../../bad?token=secret";
    expect_failure([&] { token_request_body(request, "user_abc"); },
                   "Reject injected token endpoint");
    expect_failure([&] { token_request_body(Json::object(), "user_abc"); },
                   "Reject missing signed token fields");
    std::mutex mutex;
    std::condition_variable cv;
    bool requested = false;
    auto retry = std::make_unique<okou::Realtime>(
        [&] {
            {
                std::lock_guard<std::mutex> lock(mutex);
                requested = true;
                cv.notify_all();
            }
            return Json::object(); // Invalid before HTTP exchange.
        },
        "user_abc", "org_abc", thread, [] {});
    {
        std::unique_lock<std::mutex> lock(mutex);
        check(cv.wait_for(lock, std::chrono::seconds(2), [&] { return requested; }),
              "Worker requests a token");
    }
    const auto stop_started = std::chrono::steady_clock::now();
    retry.reset();
    check(std::chrono::steady_clock::now() - stop_started < std::chrono::seconds(1),
          "Stop interrupts retry backoff");
    std::cout << "realtime_test passed (offline protocol reducer, no network)\n";
}
