// SPDX-License-Identifier: MIT
// Independent implementation of Ably's published JSON wire protocol, not an SDK port.
#include "okou/core.hpp"
#include <algorithm>
#include <condition_variable>
#include <deque>
#include <ixwebsocket/IXWebSocket.h>
#include <random>
#include <thread>

namespace okou {
namespace realtime_detail {
// Deliberately pure helpers, externally linked only so offline tests can exercise
// exactly the reducer used by the transport. Not part of the supported public API.
std::string text(const Json &j, const char *key) {
    const auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string{};
}
std::int64_t number(const Json &j, const char *key, std::int64_t fallback = 0) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_number_integer())
        return fallback;
    if (it->is_number_unsigned() && it->get<std::uint64_t>() > INT64_MAX)
        return fallback;
    return it->get<std::int64_t>();
}
bool safe_id(const std::string &id) {
    return !id.empty() && id.size() <= 256 &&
           std::all_of(id.begin(), id.end(), [](unsigned char c) {
               return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                      c == '-' || c == '_';
           });
}
Json begin(const std::string &user, const std::string &org, const std::string &thread) {
    return {{"channel", "user-org:" + user + ":" + org},
            {"message_event", "chatThreadMessageCreated:" + thread},
            {"detail_event", "chatThreadDetailChanged:" + thread},
            {"attached", false},
            {"transport", false},
            {"key", ""},
            {"serial", ""}};
}
void lost(Json &state) {
    state["attached"] = false;
    state["transport"] = false;
}
Json step(Json &state, const Json &message) {
    Json result{{"send", Json::array()},
                {"refresh", false},
                {"reconnect", false},
                {"auth", false},
                {"fatal", false}};
    if (!message.is_object())
        return result;
    const auto action = number(message, "action", -1);
    const auto channel = text(message, "channel");
    const bool ours = channel == state.at("channel");
    if (action == 4) { // CONNECTED, including AUTH confirmation
        if (message.contains("connectionDetails")) {
            auto key = text(message["connectionDetails"], "connectionKey");
            if (!key.empty() && key.size() <= 4096)
                state["key"] = std::move(key);
        }
        if (!state.at("transport").get<bool>()) {
            state["transport"] = true;
            Json attach{{"action", 10}, {"channel", state["channel"]}, {"flags", 1 << 18}};
            if (state["serial"] != "")
                attach["channelSerial"] = state["serial"];
            result["send"].push_back(std::move(attach));
        }
    } else if (action == 11 && ours && state.at("transport").get<bool>()) { // ATTACHED
        // Catch up on each new transport and on an in-place loss of continuity.
        result["refresh"] = !state["attached"].get<bool>() || !(number(message, "flags") & 4);
        state["attached"] = true;
    } else if (action == 15 && ours && state.at("attached").get<bool>()) { // MESSAGE
        if (message.contains("messages") && message["messages"].is_array()) {
            for (const auto &item : message["messages"]) {
                const auto name = text(item, "name");
                if (name == state["message_event"] || name == state["detail_event"]) {
                    result["refresh"] = true;
                    break;
                }
            }
        }
    } else if (action == 17) { // AUTH challenge
        result["auth"] = true;
    } else if (action == 6 || action == 8 || action == 9 || (action == 13 && ours)) {
        // DISCONNECTED, CLOSED, ERROR, or an unexpected DETACHED subscription.
        if (!channel.empty() && !ours)
            return result;
        std::int64_t code = 0, status = 0;
        if (message.contains("error") && message["error"].is_object()) {
            code = number(message["error"], "code");
            status = number(message["error"], "statusCode");
        }
        const bool token_error = code >= 40140 && code < 40150;
        result["auth"] = token_error;
        result["fatal"] =
            !token_error && (status == 401 || status == 403 || (code >= 40000 && code < 50000));
        result["reconnect"] = true;
        lost(state);
    }
    if (ours && state["attached"].get<bool>() && (action == 11 || action == 15)) {
        const auto serial = text(message, "channelSerial");
        if (!serial.empty() && serial.size() <= 4096)
            state["serial"] = serial;
    }
    return result;
}
Json token_request_body(const Json &request, const std::string &user) {
    if (!request.is_object())
        throw std::runtime_error("Invalid Ably TokenRequest");
    const auto key = text(request, "keyName");
    if (key.empty() || key.size() > 256 ||
        !std::all_of(key.begin(), key.end(), [](unsigned char c) {
            return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                   c == '.' || c == '-' || c == '_';
        }))
        throw std::runtime_error("Invalid Ably key name");
    for (const auto *field : {"mac", "nonce"})
        if (text(request, field).empty() || text(request, field).size() > 4096)
            throw std::runtime_error("Invalid Ably TokenRequest");
    if (number(request, "timestamp") <= 0)
        throw std::runtime_error("Invalid Ably timestamp");
    const auto client = text(request, "clientId");
    if (!client.empty() && client != user)
        throw std::runtime_error("Ably identity mismatch");
    Json body;
    // Forward signed fields exactly; do not invent capability or re-sign anything.
    for (const auto *field :
         {"keyName", "ttl", "capability", "clientId", "timestamp", "nonce", "mac"})
        if (request.contains(field))
            body[field] = request[field];
    return body;
}
} // namespace realtime_detail

struct Realtime::Impl {
    using Clock = std::chrono::steady_clock;
    std::function<Json()> provider;
    std::string user;
    Notify refresh_history;
    std::atomic_bool stopping{false};
    std::mutex queue_mutex;
    std::condition_variable wake;
    std::thread worker;
    struct Frame {
        bool closed;
        std::string data;
    };
    std::deque<Frame> queue;
    std::size_t queue_bytes = 0;
    bool overflow = false;
    Json state;
    std::string token;
    std::int64_t refresh_at = 0;
    Impl(std::function<Json()> p, std::string u, const std::string &org, const std::string &thread,
         Notify refresh)
        : provider(std::move(p)), user(std::move(u)), refresh_history(std::move(refresh)),
          state(realtime_detail::begin(user, org, thread)) {
        if (!provider || !refresh_history || !realtime_detail::safe_id(user) ||
            !realtime_detail::safe_id(org))
            throw std::invalid_argument("Invalid realtime subscriber");
    }
    static std::int64_t now_ms() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    }
    void obtain_token() {
        // Only this worker calls provider: startup, timer refresh and AUTH are serialized.
        auto request = realtime_detail::token_request_body(provider(), user);
        if (stopping.load())
            return;
        Http http(&stopping);
        const auto response = http.request(
            "POST",
            "https://rest.ably.io/keys/" + Http::encode(request["keyName"].get<std::string>()) +
                "/requestToken",
            request.dump(), {}, {{"Content-Type", "application/json"}, {"X-Ably-Version", "1.2"}});
        if (response.status < 200 || response.status >= 300)
            throw std::runtime_error("Ably token exchange failed");
        auto details = response.json();
        auto next = realtime_detail::text(details, "token");
        const auto expires = realtime_detail::number(details, "expires");
        const auto now = now_ms();
        if (next.empty() || next.size() > 16384 || expires <= now + 1000)
            throw std::runtime_error("Invalid or expired Ably token");
        const auto identity = realtime_detail::text(details, "clientId");
        if (!identity.empty() && identity != user)
            throw std::runtime_error("Ably token identity mismatch");
        token = std::move(next);
        refresh_at = expires - std::min<std::int64_t>(30000, (expires - now) / 5);
    }
    bool wait(std::chrono::milliseconds delay) {
        std::unique_lock<std::mutex> lock(queue_mutex);
        return wake.wait_for(lock, delay, [this] { return stopping.load(); });
    }
    void enqueue(const ix::WebSocketMessagePtr &msg) {
        const bool closed = msg->type == ix::WebSocketMessageType::Error ||
                            msg->type == ix::WebSocketMessageType::Close;
        if (!closed && msg->type != ix::WebSocketMessageType::Message)
            return;
        std::lock_guard<std::mutex> lock(queue_mutex);
        // Error strings, close reasons, HTTP headers and URL are never copied/logged.
        if (msg->str.size() > 1024U * 1024U || queue.size() >= 128 ||
            queue_bytes + msg->str.size() > 4U * 1024U * 1024U)
            overflow = true;
        else {
            queue.push_back({closed, closed ? std::string{} : msg->str});
            queue_bytes += queue.back().data.size();
        }
        wake.notify_all();
    }
    bool send(ix::WebSocket &socket, const Json &j) {
        return !stopping.load() && socket.send(j.dump()).success;
    }
    bool connection(ix::WebSocket &socket) {
        auto last_rx = Clock::now();
        auto deadline = last_rx + std::chrono::seconds(15);
        auto auth_deadline = Clock::time_point::max();
        auto ping_deadline = Clock::time_point::max();
        std::int64_t max_idle_ms = 15000;
        bool auth_pending = false;
        while (!stopping.load()) {
            std::deque<Frame> batch;
            {
                std::unique_lock<std::mutex> lock(queue_mutex);
                wake.wait_for(lock, std::chrono::milliseconds(100),
                              [this] { return stopping.load() || overflow || !queue.empty(); });
                if (overflow)
                    return false;
                batch.swap(queue);
                queue_bytes = 0;
            }
            bool auth_requested = false;
            for (const auto &frame : batch) {
                if (stopping.load() || frame.closed)
                    return false;
                const auto parsed = Json::parse(frame.data, nullptr, false);
                if (parsed.is_discarded() || (!parsed.is_object() && !parsed.is_array()))
                    return false;
                const auto messages = parsed.is_array() ? parsed : Json::array({parsed});
                for (const auto &message : messages) {
                    if (!message.is_object())
                        return false;
                    last_rx = Clock::now();
                    const auto action = realtime_detail::number(message, "action", -1);
                    if (action == 0 && realtime_detail::text(message, "id") == "okou-idle")
                        ping_deadline = Clock::time_point::max();
                    if (action == 4) {
                        if (!state.at("transport").get<bool>())
                            deadline = Clock::now() + std::chrono::seconds(15);
                        auth_pending = false;
                        auth_deadline = Clock::time_point::max();
                        if (message.contains("connectionDetails") &&
                            message["connectionDetails"].is_object()) {
                            const auto idle = realtime_detail::number(message["connectionDetails"],
                                                                      "maxIdleInterval", 15000);
                            if (idle >= 0 && idle <= 120000)
                                max_idle_ms = idle;
                        }
                    }
                    const auto effects = realtime_detail::step(state, message);
                    for (const auto &command : effects["send"])
                        if (!send(socket, command))
                            return false;
                    if (effects["refresh"].get<bool>() && !stopping.load())
                        refresh_history();
                    if (effects["fatal"].get<bool>()) {
                        // A new authorized session creates a new subscriber.
                        stopping.store(true);
                        return false;
                    }
                    if (effects["reconnect"].get<bool>()) {
                        if (effects["auth"].get<bool>())
                            token.clear();
                        return false;
                    }
                    auth_requested = auth_requested || effects["auth"].get<bool>();
                }
            }
            const auto now = Clock::now();
            if ((!state.at("attached").get<bool>() && now > deadline) || now > auth_deadline ||
                now > ping_deadline ||
                (max_idle_ms > 0 && now - last_rx > std::chrono::milliseconds(max_idle_ms + 10000)))
                return false;
            if (!auth_pending && state.at("transport").get<bool>() &&
                (auth_requested || now_ms() >= refresh_at)) {
                obtain_token();
                if (!send(socket, {{"action", 17}, {"auth", {{"accessToken", token}}}}))
                    return false;
                auth_pending = true;
                auth_deadline = Clock::now() + std::chrono::seconds(15);
            }
            if (state.at("transport").get<bool>() && ping_deadline == Clock::time_point::max() &&
                now - last_rx > std::chrono::seconds(10)) {
                if (!send(socket, {{"action", 0}, {"id", "okou-idle"}}))
                    return false;
                ping_deadline = Clock::now() + std::chrono::seconds(10);
            }
        }
        return false;
    }
    void run() noexcept {
        unsigned failures = 0;
        std::mt19937 random(
            static_cast<std::mt19937::result_type>(Clock::now().time_since_epoch().count()));
        while (!stopping.load()) {
            const auto attempt_started = Clock::now();
            try {
                if (token.empty() || now_ms() >= refresh_at)
                    obtain_token();
                if (stopping.load())
                    break;
                {
                    std::lock_guard<std::mutex> lock(queue_mutex);
                    queue.clear();
                    queue_bytes = 0;
                    overflow = false;
                }
                realtime_detail::lost(state);
                ix::WebSocket socket;
                socket.disableAutomaticReconnection();
                socket.disablePerMessageDeflate();
                socket.setHandshakeTimeout(10);
                ix::SocketTLSOptions tls;
                tls.caFile = "SYSTEM";
                tls.disable_hostname_validation = false;
                socket.setTLSOptions(tls);
                // Ably RSA3c specifies accessToken on the initial WebSocket URL.
                // No URL/header/error/traffic logging is registered. Never expose getUrl().
                std::string url = "wss://realtime.ably.io/"
                                  "?v=6&format=json&echo=false&heartbeats=true&clientId=" +
                                  Http::encode(user) + "&accessToken=" + Http::encode(token);
                const auto key = realtime_detail::text(state, "key");
                if (!key.empty())
                    url += "&resume=" + Http::encode(key);
                socket.setUrl(url);
                socket.setOnMessageCallback(
                    [this](const ix::WebSocketMessagePtr &m) { enqueue(m); });
                socket.start();
                try {
                    connection(socket);
                } catch (...) { /* sanitized failure; retry below */
                }
                socket.stop(); // Owner worker, never called on IX's callback thread.
                realtime_detail::lost(state);
            } catch (...) {
                // Provider, HTTP and JSON exceptions may contain private responses.
                // Reconnect without logging private details.
            }
            if (stopping.load())
                break;
            if (Clock::now() - attempt_started > std::chrono::seconds(60))
                failures = 0;
            const unsigned base = std::min(30000U, 1000U << std::min(failures++, 5U));
            const auto delay = std::chrono::milliseconds(base + random() % (base / 4 + 1));
            if (wait(delay))
                break;
        }
    }
};
Realtime::Realtime(std::function<Json()> provider, std::string user, std::string org,
                   std::string thread_id, Notify refresh_history)
    : impl_(std::make_unique<Impl>(std::move(provider), std::move(user), org, thread_id,
                                   std::move(refresh_history))) {
    impl_->worker = std::thread([p = impl_.get()] { p->run(); });
}
Realtime::~Realtime() {
    {
        std::lock_guard<std::mutex> lock(impl_->queue_mutex);
        impl_->stopping = true;
    }
    impl_->wake.notify_all();
    impl_->worker.join();
}
} // namespace okou
