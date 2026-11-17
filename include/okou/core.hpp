#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace okou {
using Json = nlohmann::json;
struct Config {
    std::string api_origin = "https://api.okou.ai";
    std::string oauth_issuer;
    std::string oauth_client_id;
    std::string oauth_resource = "https://api.okou.ai/mcp";
    std::string scopes = "openid profile offline_access user:org:read okou:chat:read okou:chat:send okou:project:read okou:voice:transcribe";
    std::string keyboard = "/dev/input/by-path/platform-3f804000.i2c-event";
    std::string framebuffer = "/dev/fb0";
    std::string capture_device = "plughw:CARD=ES8389Audio,DEV=0";
    std::string data_dir;
};
struct HttpResponse {
    long status = 0;
    std::string body;
    std::map<std::string, std::string> headers;
    Json json() const;
};
struct UploadPart {
    std::string name;
    std::string value;
    std::string filename;
    std::string content_type;
};
class HttpError : public std::runtime_error {
public:
    long status;
    bool uncertain;
    HttpError(long status, std::string message, bool uncertain = false);
};
class Http {
public:
    explicit Http(std::atomic_bool* cancelled = nullptr);
    HttpResponse request(const std::string& method, const std::string& url,
                         const std::string& body = {}, const std::string& bearer = {},
                         const std::map<std::string, std::string>& headers = {},
                         const std::vector<UploadPart>& parts = {});
    static std::string encode(const std::string& value);
private:
    std::atomic_bool* cancelled_;
};
struct Tokens {
    std::string access_token;
    std::string refresh_token;
    std::string user_id;
    std::string org_id;
    std::string client_id;
    std::int64_t expires_at = 0;
};
struct DeviceCode {
    std::string device_code, user_code, verification_uri, verification_uri_complete;
    int interval = 5;
    int expires_in = 600;
};
enum class PollState { pending, slow_down, authorized, denied, expired };
struct PollResult { PollState state = PollState::pending; Tokens tokens; };
class OAuth {
public:
    OAuth(Config config, Http& http);
    DeviceCode begin();
    PollResult poll(const DeviceCode& code);
    Tokens refresh(const Tokens& previous);
    Tokens load() const;
    void save(const Tokens& tokens) const;
    void logout(const Tokens& tokens);
    const Config& config() const { return config_; }
private:
    Config config_;
    Http& http_;
    Json metadata_;
    void discover();
    Tokens parse_tokens(const Json& body, const Tokens& previous = {}) const;
};
struct Project { std::string id, title; };
struct Thread {
    std::string id, title, project_id, agent_id;
    bool active = false, unread = false;
    bool muted = false;
};
struct Message { std::string id, role, text; bool terminal = false; };
struct ListState {
    std::vector<Thread> threads;
    std::vector<Project> projects;
};
struct SendResult { std::string client_event_id; bool uncertain = false; };
class Client {
public:
    Client(Config config, Http& http, Tokens tokens);
    void set_tokens(Tokens tokens);
    const Tokens& tokens() const { return tokens_; }
    ListState list(bool include_indicators = true);
    Json indicators();
    std::vector<Message> history(const std::string& thread_id);
    void mark_read(const std::string& thread_id);
    bool has_pending_send(const std::string& thread_id) const;
    SendResult send(const Thread& thread, const std::string& text,
                    const std::string& client_event_id = {});
    std::string transcribe(const std::vector<std::int16_t>& pcm,
                           const std::string& previous, double overlap_seconds);
    std::string polish(const std::vector<std::string>& segments);
    Json realtime_token();
    static std::string uuid();
private:
    Config config_;
    Http& http_;
    Tokens tokens_;
    Json api(const std::string& method, const std::string& path, const Json& body = nullptr);
    Json snapshot(const std::string& url, bool ndjson);
};
class Realtime {
public:
    using Notify = std::function<void(const std::string&)>;
    Realtime(std::function<Json()> token_provider, std::string user_id,
             std::string org_id, Notify notify);
    ~Realtime();
    Realtime(const Realtime&) = delete;
    Realtime& operator=(const Realtime&) = delete;
    void start();
    void stop();
    bool connected() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
std::vector<std::uint8_t> wav(const std::vector<std::int16_t>& pcm);
class AudioCapture {
public:
    explicit AudioCapture(std::string device);
    ~AudioCapture();
    void start();
    std::vector<std::int16_t> finish();
    void cancel();
    bool recording() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
Config load_config(const std::string& path = {});
void save_config(const Config& config);
} // namespace okou
