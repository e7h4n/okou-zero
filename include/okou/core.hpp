#pragma once

#include "okou/build_config.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace okou {
using Json = nlohmann::json;
struct Config {
    std::string api_origin = build::api_origin;
    std::string oauth_issuer;
    std::string oauth_client_id = build::oauth_client_id;
    std::string oauth_resource = std::string(build::api_origin) + "/mcp";
    std::string scopes = "openid profile offline_access user:org:read okou:chat:read "
                         "okou:chat:send";
    std::string keyboard = "/dev/input/by-path/platform-3f804000.i2c-event";
    std::string framebuffer = "/dev/fb0";
    std::string capture_device = "pulse";
    std::string data_dir;
};
struct HttpResponse {
    long status = 0;
    std::string body;
    std::map<std::string, std::string> headers;
    Json json() const;
};
class HttpError : public std::runtime_error {
  public:
    long status;
    bool uncertain;
    int curl_code;
    std::string oauth_error;
    HttpError(long status, std::string message, bool uncertain = false, int curl_code = 0,
              std::string_view oauth_error = "none");
};
class Http {
  public:
    explicit Http(std::atomic_bool *cancelled = nullptr);
    HttpResponse request(const std::string &method, const std::string &url,
                         std::string_view body = {}, const std::string &bearer = {},
                         const std::map<std::string, std::string> &headers = {},
                         std::FILE *output = nullptr, std::size_t body_limit = 16U * 1024U * 1024U);
    static std::string encode(const std::string &value);

  private:
    std::atomic_bool *cancelled_;
};
struct Tokens {
    std::string access_token;
    std::string refresh_token;
    std::string user_id;
    std::string org_id;
    std::int64_t expires_at = 0;
};
struct DeviceCode {
    std::string device_code, user_code, verification_uri;
    int interval = 5;
    int expires_in = 600;
};
enum class PollState { pending, slow_down, authorized, denied, expired };
struct PollResult {
    PollState state = PollState::pending;
    Tokens tokens;
};
class OAuth {
  public:
    OAuth(Config config, Http &http);
    DeviceCode begin();
    PollResult poll(const DeviceCode &code);
    Tokens refresh(const Tokens &previous);
    Tokens load() const;
    void save(const Tokens &tokens) const;
    void logout(const Tokens &tokens);

  private:
    Config config_;
    Http &http_;
    Json metadata_;
    void discover();
    Tokens parse_tokens(const Json &body, const Tokens &previous = {}) const;
};
struct Message {
    std::string id, role, text;
    bool terminal = false;
    std::vector<std::string> voice_clips{};
    std::string local_sent_at{};
    std::string run_id{};
};
struct SendResult {
    std::string client_event_id;
    bool uncertain = false;
};
class Client {
  public:
    Client(Config config, Http &http, Tokens tokens);
    void set_tokens(Tokens tokens);
    std::string fixed_thread_id();
    std::vector<Message> history(const std::string &thread_id);
    bool has_pending_send(const std::string &thread_id) const;
    std::string voice_playback_url(const std::string &file_id);
    std::string upload_voice(const std::vector<std::int16_t> &pcm);
    SendResult send_voice(const std::string &thread_id, const std::string &file_id);
    Json realtime_token();
    static std::string uuid();

  private:
    Config config_;
    Http &http_;
    Tokens tokens_;
    Json api(const std::string &method, const std::string &path, const Json &body = nullptr);
    Json history_snapshot(const std::string &url);
};
class Realtime {
  public:
    using Notify = std::function<void()>;
    Realtime(std::function<Json()> token_provider, std::string user_id, std::string org_id,
             std::string thread_id, Notify refresh_history);
    ~Realtime();
    Realtime(const Realtime &) = delete;
    Realtime &operator=(const Realtime &) = delete;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
std::vector<std::uint8_t> wav(const std::vector<std::int16_t> &pcm);
void play_wav(std::FILE *file, const std::atomic_bool &paused, const std::atomic_bool &stop,
              std::atomic_uint &seconds);
class RecordingWaveform;
class AudioCapture {
  public:
    explicit AudioCapture(std::string device);
    ~AudioCapture();
    void start(RecordingWaveform &waveform);
    std::vector<std::int16_t> finish();
    void cancel();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
Config load_config(const std::string &path = {});
void save_config(const Config &config);
} // namespace okou
