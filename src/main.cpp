#include "disk_file.hpp"
#include "emoji.hpp"
#include "idle_display.hpp"
#include "okou/diagnostics.hpp"
#include "okou/ui.hpp"
#include "update_process.hpp"
#include "volume_control.hpp"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <curl/curl.h>
#include <deque>
#include <iostream>
#include <poll.h>
#include <sys/eventfd.h>
#include <thread>
#include <unistd.h>
#include <utility>

namespace okou {
namespace {
std::atomic_bool interrupted{false};
void interrupt(int) {
    interrupted.store(true);
}
using Update = std::function<void(UI &)>;
class UiWake {
    int fd_ = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);

  public:
    UiWake() {
        if (fd_ < 0)
            throw std::runtime_error("Cannot create UI wake descriptor");
    }
    ~UiWake() {
        close(fd_);
    }
    int descriptor() const {
        return fd_;
    }
    void notify() const {
        while (eventfd_write(fd_, 1) < 0 && errno == EINTR) {
        }
    }
    void consume() const {
        eventfd_t value;
        while (eventfd_read(fd_, &value) < 0 && errno == EINTR) {
        }
    }
};
class Engine {
    struct Session {
        Config config;
        std::atomic_bool cancelled{false};
        Http http{&cancelled};
        OAuth auth;
        Tokens tokens;
        std::unique_ptr<Client> client;
        std::mutex io;
        explicit Session(Config cfg) : config(std::move(cfg)), auth(config, http) {}
    };
    UiWake &wake_;
    std::shared_ptr<Session> session_;
    std::unique_ptr<Realtime> realtime_;
    RecordingWaveform &waveform_;
    std::unique_ptr<AudioCapture> audio_;
    std::vector<std::int16_t> voice_pcm_;
    std::string voice_file_id_;
    std::string voice_thread_;
    std::atomic_bool voice_cancelled_{false};
    std::thread playback_;
    std::atomic_bool playback_active_{false}, playback_paused_{false}, playback_stop_{false};
    std::atomic_bool playback_failed_{false};
    std::atomic_uint playback_seconds_{0};
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> tasks_;
    std::deque<Update> updates_;
    std::thread worker_;
    std::atomic_bool stopping_{false}, history_pending_{false};
    std::string selected_thread_;
    void emit(Update update) {
        std::lock_guard<std::mutex> guard(mutex_);
        updates_.push_back(std::move(update));
        wake_.notify();
    }
    void enqueue(std::function<void()> task) {
        {
            std::lock_guard<std::mutex> guard(mutex_);
            tasks_.push_back(std::move(task));
        }
        cv_.notify_all();
    }
    void ensure(Session &s) {
        if (!s.client)
            throw std::runtime_error("Sign in to continue");
        auto now = std::chrono::duration_cast<std::chrono::seconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();
        if (s.tokens.expires_at <= now + 60) {
            s.tokens = s.auth.refresh(s.tokens);
            s.auth.save(s.tokens);
            s.client->set_tokens(s.tokens);
        }
    }
    void error(const std::exception &ex,
               diagnostics::Stage stage = diagnostics::Stage::application) {
        diagnostics::log_error(stage, ex);
        const auto *http = dynamic_cast<const HttpError *>(&ex);
        long status = http ? http->status : 0;
        std::string text = status == 401   ? "Sign-in expired; sign in again"
                           : status == 403 ? "Access not granted; check OAuth scopes"
                                           : "Could not complete request; Ctrl+L to refresh";
        if (http && (http->curl_code == CURLE_COULDNT_CONNECT ||
                     http->curl_code == CURLE_COULDNT_RESOLVE_HOST ||
                     http->curl_code == CURLE_COULDNT_RESOLVE_PROXY))
            text = "Network unavailable";
        emit([text](UI &ui) {
            ui.status = text;
            ui.busy = false;
        });
    }
    void stop_realtime() {
        if (realtime_) {
            realtime_->stop();
            realtime_.reset();
        }
    }
    void start_realtime(const std::shared_ptr<Session> &s) {
        stop_realtime();
        realtime_ = std::make_unique<Realtime>(
            [s] {
                std::lock_guard<std::mutex> guard(s->io);
                if (s->cancelled.load() || !s->client)
                    throw std::runtime_error("Session closed");
                auto now = std::chrono::duration_cast<std::chrono::seconds>(
                               std::chrono::system_clock::now().time_since_epoch())
                               .count();
                if (s->tokens.expires_at <= now + 60) {
                    s->tokens = s->auth.refresh(s->tokens);
                    s->auth.save(s->tokens);
                    s->client->set_tokens(s->tokens);
                }
                return s->client->realtime_token();
            },
            s->tokens.user_id, s->tokens.org_id,
            [this, weak = std::weak_ptr<Session>(s)](const std::string &name) {
                auto current = weak.lock();
                if (!current || current != std::atomic_load(&session_) || stopping_)
                    return;
                if (name == "connection:gap" || name.rfind("chatThreadMessageCreated:", 0) == 0 ||
                    name.rfind("chatThreadDetailChanged:", 0) == 0)
                    refresh_history();
            });
        realtime_->start();
    }

  public:
    Engine(Config config, RecordingWaveform &waveform, UiWake &wake)
        : wake_(wake), session_(std::make_shared<Session>(std::move(config))), waveform_(waveform) {
        worker_ = std::thread([this] {
            while (!stopping_) {
                std::function<void()> task;
                {
                    std::unique_lock<std::mutex> lock(mutex_);
                    cv_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });
                    if (stopping_)
                        break;
                    task = std::move(tasks_.front());
                    tasks_.pop_front();
                }
                try {
                    task();
                } catch (const std::exception &ex) {
                    if (!stopping_)
                        error(ex);
                }
            }
        });
    }
    ~Engine() {
        stopping_ = true;
        auto s = std::atomic_load(&session_);
        s->cancelled = true;
        cv_.notify_all();
        stop_playback();
        if (playback_.joinable())
            playback_.join();
        if (worker_.joinable())
            worker_.join();
        stop_realtime();
        if (audio_)
            audio_->cancel();
    }
    bool drain(UI &ui) {
        std::deque<Update> batch;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            batch.swap(updates_);
        }
        for (auto &update : batch)
            update(ui);
        return !batch.empty();
    }
    void initialize() {
        enqueue([this] {
            auto s = std::atomic_load(&session_);
            if (s->config.oauth_client_id.empty()) {
                emit([](UI &ui) {
                    ui.page = Page::setup;
                    ui.busy = false;
                    ui.status = "See docs/auth-setup.md";
                });
                return;
            }
            std::lock_guard<std::mutex> guard(s->io);
            s->tokens = s->auth.load();
            if (s->tokens.access_token.empty()) {
                emit([](UI &ui) {
                    ui.page = Page::login;
                    ui.busy = false;
                });
                return;
            }
            s->client = std::make_unique<Client>(s->config, s->http, s->tokens);
            ensure(*s);
            auto tokens = s->tokens;
            emit([tokens](UI &ui) {
                ui.user_id = tokens.user_id;
                ui.org_id = tokens.org_id;
                ui.page = Page::chat;
                ui.busy = true;
                ui.status = "Opening your chat...";
            });
            enqueue([this, s] {
                start_realtime(s);
                open_fixed_chat();
            });
        });
    }
    void configure(const std::string &id) {
        enqueue([this, id] {
            auto old = std::atomic_load(&session_);
            Config cfg = old->config;
            cfg.oauth_client_id = id;
            save_config(cfg);
            auto s = std::make_shared<Session>(cfg);
            std::atomic_store(&session_, s);
            emit([](UI &ui) {
                ui.draft.clear();
                ui.page = Page::login;
                ui.busy = false;
                ui.status = "Press Enter to sign in";
            });
        });
    }
    void login() {
        enqueue([this] {
            stop_realtime();
            auto s = std::atomic_load(&session_);
            s->cancelled = false;
            DeviceCode code;
            {
                std::lock_guard<std::mutex> guard(s->io);
                code = s->auth.begin();
            }
            emit([code](UI &ui) {
                ui.page = Page::login;
                ui.busy = true;
                ui.user_code = code.user_code;
                ui.verification_uri = code.verification_uri;
                ui.status = "Choose your workspace when approving";
            });
            auto deadline =
                std::chrono::steady_clock::now() + std::chrono::seconds(code.expires_in);
            int interval = std::max(1, code.interval);
            while (!stopping_ && std::chrono::steady_clock::now() < deadline) {
                {
                    std::unique_lock<std::mutex> lock(mutex_);
                    auto next_poll =
                        std::chrono::steady_clock::now() + std::chrono::seconds(interval);
                    if (cv_.wait_until(lock, std::min(next_poll, deadline),
                                       [this] { return stopping_.load(); }))
                        return;
                }
                PollResult result;
                {
                    std::lock_guard<std::mutex> guard(s->io);
                    if (std::chrono::steady_clock::now() >= deadline)
                        break;
                    result = s->auth.poll(code);
                }
                if (result.state == PollState::pending)
                    continue;
                if (result.state == PollState::slow_down) {
                    interval += 5;
                    continue;
                }
                if (result.state == PollState::denied || result.state == PollState::expired) {
                    emit([](UI &ui) {
                        ui.busy = false;
                        ui.user_code.clear();
                        ui.verification_uri.clear();
                        ui.status = "Code expired or denied; Enter to try again";
                    });
                    return;
                }
                {
                    std::lock_guard<std::mutex> guard(s->io);
                    s->tokens = result.tokens;
                    if (s->tokens.org_id.empty() || s->tokens.user_id.empty())
                        throw std::runtime_error("OAuth grant has no workspace");
                    s->auth.save(s->tokens);
                    s->client = std::make_unique<Client>(s->config, s->http, s->tokens);
                }
                selected_thread_.clear();
                auto tokens = s->tokens;
                emit([tokens](UI &ui) {
                    ui.reset_account();
                    ui.user_id = tokens.user_id;
                    ui.org_id = tokens.org_id;
                    ui.page = Page::chat;
                    ui.busy = true;
                    ui.status = "Opening your chat...";
                });
                start_realtime(s);
                open_fixed_chat();
                return;
            }
            if (!stopping_)
                emit([](UI &ui) {
                    ui.busy = false;
                    ui.user_code.clear();
                    ui.status = "Code expired; Enter to try again";
                });
        });
    }
    void open_fixed_chat() {
        enqueue([this] {
            auto s = std::atomic_load(&session_);
            Thread thread;
            {
                std::lock_guard<std::mutex> guard(s->io);
                ensure(*s);
                thread = s->client->fixed_thread();
            }
            const bool changed = selected_thread_ != thread.id;
            selected_thread_ = thread.id;
            emit([thread, changed](UI &ui) {
                if (changed || ui.current.id != thread.id)
                    ui.select_thread(thread);
                else
                    ui.current.title = thread.title;
                ui.busy = true;
                ui.status = "Loading conversation...";
            });
            refresh_history();
        });
    }
    void refresh_history() {
        if (history_pending_.exchange(true))
            return;
        enqueue([this] {
            history_pending_ = false;
            if (selected_thread_.empty())
                return;
            auto id = selected_thread_;
            auto s = std::atomic_load(&session_);
            std::vector<Message> messages;
            bool pending = false;
            {
                std::lock_guard<std::mutex> guard(s->io);
                ensure(*s);
                messages = s->client->history(id);
                pending = s->client->has_pending_send(id);
            }
            emit([id, messages = std::move(messages), pending](UI &ui) mutable {
                ui.reconcile_send(id, pending);
                if (ui.current.id == id) {
                    ui.update_history(std::move(messages));
                    if (!ui.recording && !ui.voice_pending && !ui.submitting) {
                        ui.busy = false;
                        if (pending)
                            ui.status = "Send uncertain; do not resend";
                        else if (ui.status == "Loading conversation..." ||
                                 ui.status == "Network unavailable" ||
                                 ui.status == "Send uncertain; do not resend")
                            ui.status.clear();
                    }
                }
            });
        });
    }
    void stop_playback() {
        playback_stop_ = true;
        playback_failed_ = false;
    }
    unsigned playback_seconds() const {
        return playback_seconds_;
    }
    std::string playback_status() const {
        if (playback_stop_)
            return {};
        if (playback_active_)
            return playback_paused_ ? "Paused   Space resume" : "Playing   Space pause";
        return playback_failed_ ? "Could not play recording" : "";
    }
    void toggle_playback(const std::vector<std::string> &clips) {
        if (playback_active_) {
            if (!playback_stop_)
                playback_paused_ = !playback_paused_.load();
            return;
        }
        if (clips.empty())
            return;
        if (playback_.joinable())
            playback_.join();
        playback_stop_ = playback_paused_ = playback_failed_ = false;
        playback_seconds_ = 0;
        playback_active_ = true;
        auto s = std::atomic_load(&session_);
        playback_ = std::thread([this, s, clips] {
            try {
                Http storage(&playback_stop_);
                for (const auto &id : clips) {
                    if (playback_stop_)
                        break;
                    std::string url;
                    {
                        std::lock_guard<std::mutex> guard(s->io);
                        if (playback_stop_)
                            break;
                        ensure(*s);
                        url = s->client->voice_playback_url(id);
                    }
                    DiskFile spool;
                    auto close_file = [](std::FILE *file) { std::fclose(file); };
                    std::unique_ptr<std::FILE, decltype(close_file)> audio(spool.stream(),
                                                                           close_file);
                    auto response = storage.request("GET", url, {}, {}, {}, {}, audio.get(),
                                                    25U * 1024U * 1024U);
                    if (response.status != 200)
                        throw HttpError(response.status, "Recording download failed");
                    if (std::fflush(audio.get()) != 0 || std::fseek(audio.get(), 0, SEEK_SET) != 0)
                        throw std::runtime_error("Could not read recording");
                    if (!playback_stop_)
                        play_wav(audio.get(), playback_paused_, playback_stop_, playback_seconds_);
                }
            } catch (const std::exception &ex) {
                if (!playback_stop_) {
                    diagnostics::log_error(diagnostics::Stage::voice, ex);
                    playback_failed_ = true;
                }
            }
            playback_active_ = false;
            wake_.notify();
        });
    }
    void voice(bool finish, const Thread &thread) {
        if (!finish)
            voice_cancelled_ = false;
        enqueue([this, finish, thread] {
            const auto &thread_id = thread.id;
            auto s = std::atomic_load(&session_);
            try {
                if (!finish) {
                    voice_thread_ = thread_id;
                    audio_ = std::make_unique<AudioCapture>(s->config.capture_device);
                    if (voice_cancelled_)
                        return;
                    audio_->start(&waveform_);
                    emit([this, started = std::chrono::steady_clock::now()](UI &ui) {
                        if (voice_cancelled_ || !ui.recording)
                            return;
                        ui.recording_started = started;
                        ui.voice_working = false;
                        ui.busy = false;
                    });
                    return;
                }
                if (voice_thread_ != thread_id)
                    throw std::runtime_error("Recording belongs to another chat");
                if (audio_) {
                    voice_pcm_ = audio_->finish();
                    audio_.reset();
                }
                if (voice_pcm_.empty() && voice_file_id_.empty()) {
                    emit([](UI &ui) {
                        ui.recording = ui.voice_pending = ui.voice_working = ui.busy = false;
                        ui.status = "No audio recorded";
                    });
                    return;
                }
                if (voice_cancelled_ || stopping_)
                    return;
                emit([](UI &ui) {
                    ui.recording = ui.voice_pending = ui.voice_working = false;
                    ui.submitting = ui.busy = true;
                    ui.status = "Sending voice message...";
                });
                SendResult result;
                {
                    std::lock_guard<std::mutex> guard(s->io);
                    ensure(*s);
                    if (!s->client->has_pending_send(thread_id) && voice_file_id_.empty()) {
                        voice_file_id_ = s->client->upload_voice(voice_pcm_);
                        voice_pcm_ = std::vector<std::int16_t>();
                    }
                    if (voice_cancelled_ || stopping_)
                        return;
                    result = voice_file_id_.empty() ? SendResult{"", true}
                                                    : s->client->send_voice(thread, voice_file_id_);
                }
                voice_pcm_ = std::vector<std::int16_t>();
                emit([id = thread_id, result = std::move(result),
                      file_id = std::exchange(voice_file_id_, {})](UI &ui) {
                    ui.complete_send(id, "[Voice message]", result.uncertain,
                                     result.client_event_id, file_id);
                    ui.busy = false;
                    ui.status =
                        result.uncertain ? "Send uncertain; do not resend" : "Message accepted";
                });
                refresh_history();
            } catch (const std::exception &ex) {
                if (audio_) {
                    audio_->cancel();
                    audio_.reset();
                }
                bool retained =
                    (!voice_pcm_.empty() || !voice_file_id_.empty()) && !voice_cancelled_;
                error(ex, diagnostics::Stage::voice);
                emit([retained](UI &ui) {
                    ui.recording = ui.voice_working = ui.submitting = ui.busy = false;
                    ui.voice_pending = retained;
                    ui.status = retained ? "Voice retained: Ctrl+R retry; Esc discard"
                                         : "Recording stopped";
                });
            }
        });
    }
    void cancel_voice() {
        voice_cancelled_ = true;
        enqueue([this] {
            if (audio_) {
                audio_->cancel();
                audio_.reset();
            }
            voice_pcm_ = std::vector<std::int16_t>();
            voice_file_id_.clear();
            voice_thread_.clear();
            emit([](UI &ui) {
                ui.recording = ui.voice_pending = ui.voice_working = ui.busy = false;
                ui.submitting = false;
                ui.status = "Recording discarded";
            });
        });
    }
    void logout(bool switch_workspace) {
        enqueue([this, switch_workspace] {
            stop_realtime();
            if (audio_) {
                audio_->cancel();
                audio_.reset();
            }
            voice_pcm_ = std::vector<std::int16_t>();
            voice_file_id_.clear();
            voice_thread_.clear();
            auto s = std::atomic_load(&session_);
            {
                std::lock_guard<std::mutex> guard(s->io);
                s->auth.logout(s->tokens);
                s->tokens = {};
                s->client.reset();
            }
            selected_thread_.clear();
            emit([switch_workspace](UI &ui) {
                ui.reset_account();
                ui.page = Page::login;
                ui.status =
                    switch_workspace ? "Sign in and approve the new workspace" : "Signed out";
            });
        });
    }
};
} // namespace
} // namespace okou

int main(int argc, char **argv) {
    using namespace okou;
    try {
        bool demo = false, self_test = false, headless = false;
        std::string screen = "chat", screenshot, config_path;
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--version") {
                std::cout << "Okou " << OKOU_ZERO_VERSION << '\n';
                return 0;
            }
            if (arg == "--demo")
                demo = true;
            else if (arg == "--headless") {
                headless = demo = true;
            } else if (arg == "--demo-screen" && i + 1 < argc) {
                demo = true;
                screen = argv[++i];
            } else if (arg == "--self-test-ui") {
                demo = self_test = true;
            } else if ((arg == "--screenshot" || arg == "--output") && i + 1 < argc)
                screenshot = argv[++i];
            else if (arg == "--config" && i + 1 < argc)
                config_path = argv[++i];
            else if (arg == "--help") {
                std::cout << "Okou Zero\n  --version\n  --config PATH\n  --demo [--demo-screen "
                             "login|chat|history|recording|playback|paused|sending|waiting|"
                             "uncertain|offline|update]\n  --screenshot PATH "
                             "(simulator only)\n "
                             " --self-test-ui\n";
                return 0;
            } else
                throw std::runtime_error("Unknown command-line option");
        }
        if (!config_path.empty())
            setenv("OKOU_ZERO_CONFIG", config_path.c_str(), 1);
        auto config = load_config(config_path);
        std::signal(SIGINT, interrupt);
        std::signal(SIGTERM, interrupt);
        lv_init();
        Display display(OKOU_ZERO_DESKTOP, config.framebuffer, headless);
        std::unique_ptr<Keyboard> keyboard;
        if (!headless)
            keyboard = std::make_unique<Keyboard>(OKOU_ZERO_DESKTOP, config.keyboard);
        if (headless && screenshot.empty())
            screenshot = "okou-zero.ppm";
        UI ui(display.get());
        UiWake wake;
        std::unique_ptr<Engine> engine;
        if (demo)
            ui.demo(screen);
        else {
            ui.busy = true;
            engine = std::make_unique<Engine>(config, ui.recording_waveform, wake);
            engine->initialize();
        }
        if (self_test) {
            auto check = [](bool condition) {
                if (!condition)
                    throw std::runtime_error("UI keyboard test failed");
            };
            check(split_emoji_title("工作 👩🏽‍💻 计划") ==
                  std::make_pair(std::string("👩🏽‍💻"), std::string("工作 计划")));
            check(split_emoji_title("🇨🇳 项目").first == "🇨🇳");
            check(split_emoji_title("1️⃣ Project").second == "Project");
            check(split_emoji_title("Plan 2026").first.empty());
            ui.demo("chat");
            check(ui.page == Page::chat && !ui.current.id.empty());
            check(ui.handle({Key::tab, {}, false, false}) == Action::none);
            check(ui.page == Page::chat);
            check(ui.handle({Key::text, "中文 input!", false, false}) == Action::none);
            check(ui.draft.empty());
            check(ui.handle({Key::enter, {}, false, false}) == Action::none);
            ui.complete_send(ui.current.id, "spoken message", false, "voice_event", "new-clip");
            ui.render();
            check(ui.visible_voice_clips == std::vector<std::string>{"new-clip"});
            check(ui.messages.back().id == "voice_event");
            ui.update_history({});
            check(ui.messages.size() == 1);
            ui.update_history({{"voice_event", "user", "spoken message", false}});
            ui.complete_send(ui.current.id, "spoken message", false, "voice_event");
            check(ui.messages.size() == 1);
            check(!ui.take_local_reply());
            ui.update_history({{"voice_event", "user", "spoken message", false},
                               {"answer", "assistant", "Reply", true}});
            check(ui.take_local_reply() && !ui.take_local_reply());
            ui.update_history({});
            check(ui.messages.empty());
            ui.complete_send(ui.current.id, "uncertain voice", true);
            check(ui.uncertain && ui.messages.empty());
            ui.reconcile_send(ui.current.id, false);
            check(!ui.uncertain);
            ui.messages = {{"old", "user", "[Voice message]", false, {"old-clip"}},
                           {"middle", "assistant", "1\n2\n3\n4\n5\n6\n7\n8\n9", false},
                           {"current", "user", "[Voice message]", false, {"current-clip"}},
                           {"latest", "assistant", "Current reply", false}};
            ui.render();
            check(ui.visible_voice_clips == std::vector<std::string>{"current-clip"});
            check(ui.handle({Key::space, " "}) == Action::playback);
            ui.handle({Key::left, {}});
            ui.render();
            ui.handle({Key::down, {}});
            ui.render();
            check(ui.visible_voice_clips == std::vector<std::string>{"old-clip"});
            ui.handle({Key::right, {}});
            ui.render();
            check(ui.visible_voice_clips == std::vector<std::string>{"old-clip"});
            ui.handle({Key::right, {}});
            ui.render();
            check(ui.visible_voice_clips == std::vector<std::string>{"current-clip"});
            for (int i = 0; i < 8; ++i)
                ui.handle({Key::left, {}});
            ui.render();
            check(ui.visible_voice_clips == std::vector<std::string>{"old-clip"});
            ui.handle({Key::escape, {}, false, false});
            check(ui.page == Page::settings);
            ui.handle({Key::escape, {}, false, false});
            check(ui.page == Page::chat);
            check(ui.handle({Key::record, {}, false, false}) == Action::none);
            ui.recording = true;
            ui.render();
            ui.update_recording_clock();
            check(!ui.update_safe());
            check(ui.handle({Key::escape, {}, false, false}) == Action::none);
            ui.recording = false;
            ui.voice_pending = true;
            check(ui.handle({Key::workspace, {}, false, false}) == Action::none);
            check(ui.handle({Key::record, {}, false, false}) == Action::voice);
            check(!ui.update_safe());
            ui.reset_account();
            check(ui.update_safe());
            ui.demo("chat");
        }
        auto last = std::chrono::steady_clock::now();
        auto start = last;
        auto next_battery_read = last;
        bool dirty = true, running = true, restart = false;
        // On the LCD the previous frame is the launcher wallpaper. Keep it on
        // glass until initialization produces chat, login, setup, or an error.
        bool awaiting_first_screen = !OKOU_ZERO_DESKTOP && !demo;
        UpdateProcess updater;
        VolumeControl volume;
        constexpr auto enter_hold_delay = std::chrono::milliseconds(350);
        std::chrono::steady_clock::time_point enter_pressed{};
        std::string enter_thread;
        if (!demo)
            updater.start(false, OKOU_ZERO_VERSION);
        using namespace std::chrono;
        const auto *home = std::getenv("HOME");
        const bool device = !OKOU_ZERO_DESKTOP && !demo;
        IdleDisplay backlight(
            "/sys/class/backlight/backlight/brightness",
            device ? launcher_dark_time(std::filesystem::path(home ? home : "/root") /
                                        ".config/cardputerzero/config.json")
                   : seconds(0));
        auto wait = [&](steady_clock::time_point deadline) {
            pollfd sources[] = {{keyboard ? keyboard->descriptor() : -1, POLLIN, 0},
                                {wake.descriptor(), POLLIN, 0},
                                {updater.descriptor(), POLLIN, 0}};
            auto timeout = ceil<milliseconds>(deadline - steady_clock::now()).count();
            if (::poll(sources, std::size(sources),
                       static_cast<int>(std::clamp<int64_t>(timeout, 0, 5000))) < 0 &&
                errno != EINTR)
                throw std::runtime_error("UI event wait failed");
            if (sources[1].revents & POLLIN)
                wake.consume();
        };
        while ((running && !interrupted) || updater.installing()) {
            auto now = std::chrono::steady_clock::now();
            const auto elapsed = duration_cast<milliseconds>(now - last);
            lv_tick_inc(static_cast<std::uint32_t>(elapsed.count()));
            last += elapsed;
            if (!demo && now >= next_battery_read) {
                next_battery_read = now + std::chrono::seconds(5);
                auto battery = read_battery_status();
                if (battery.percent != ui.battery.percent ||
                    battery.charging != ui.battery.charging) {
                    ui.battery = battery;
                    dirty = true;
                }
            }
            bool reply = false;
            if (engine && engine->drain(ui)) {
                dirty = true;
                reply = ui.take_local_reply();
            }
            if (engine) {
                auto playback = engine->playback_status();
                if (ui.playback_status != playback) {
                    ui.playback_status = std::move(playback);
                    dirty = true;
                }
            }
            if (updater.poll()) {
                ui.update_message = update_hint(updater.view());
                dirty = true;
            }
            if (updater.view().status == UpdateStatus::installed) {
                restart = running && !interrupted;
                break;
            }
            std::vector<Input> inputs;
            try {
                if (keyboard)
                    inputs = keyboard->poll(backlight.asleep());
            } catch (const std::exception &) {
                if (!updater.installing())
                    throw;
                keyboard.reset();
                running = false; // Finish the confirmed install before leaving.
            }
            if ((keyboard && keyboard->activity()) || reply) {
                dirty |= backlight.asleep();
                backlight.wake();
            }
            if (awaiting_first_screen && ui.busy) {
                for (const auto &input : inputs) {
                    if (input.key == Key::quit || input.key == Key::escape)
                        running = false;
                }
                if (running)
                    wait(next_battery_read);
                continue;
            }
            if (awaiting_first_screen)
                backlight.wake();
            awaiting_first_screen = false;
            for (auto input : inputs) {
                if (updater.installing())
                    continue;
                if (input.key == Key::volume_up || input.key == Key::volume_down ||
                    input.key == Key::mute) {
                    if (!demo)
                        volume.press(input.key);
                    continue;
                }
                if (input.key == Key::escape)
                    enter_thread.clear();
                if (input.key == Key::enter_cancel) {
                    enter_thread.clear();
                    if (engine && ui.recording)
                        engine->cancel_voice();
                    continue;
                }
                if (input.key == Key::enter_down) {
                    if (ui.page == Page::chat) {
                        if (engine && !input.shift && !input.ctrl && !ui.current.id.empty() &&
                            !ui.busy && !ui.recording && !ui.voice_pending && !ui.submitting &&
                            !ui.uncertain && enter_thread.empty()) {
                            enter_thread = ui.current.id;
                            enter_pressed = std::chrono::steady_clock::now();
                            ui.recording_started = {};
                            ui.recording = ui.voice_working = ui.busy = true;
                            engine->stop_playback();
                            engine->voice(false, ui.current);
                            dirty = true;
                        }
                        continue;
                    }
                    input.key = Key::enter;
                } else if (input.key == Key::enter_up) {
                    const bool same_chat = ui.page == Page::chat && !enter_thread.empty() &&
                                           enter_thread == ui.current.id;
                    enter_thread.clear();
                    if (engine && same_chat && ui.recording) {
                        ui.recording = false;
                        ui.voice_pending = ui.voice_working = ui.busy = true;
                        if (std::chrono::steady_clock::now() - enter_pressed < enter_hold_delay) {
                            engine->cancel_voice();
                        } else {
                            ui.status = "Sending voice message...";
                            engine->voice(true, ui.current);
                        }
                        dirty = true;
                    }
                    continue;
                }
                if (input.key == Key::update) {
                    if (updater.view().status == UpdateStatus::ready) {
                        if (!ui.update_safe())
                            ui.update_message = "Finish draft/voice before updating";
                        else {
                            updater.start(true, OKOU_ZERO_VERSION);
                            ui.update_message = update_hint(updater.view());
                        }
                        dirty = true;
                    }
                    continue;
                }
                if ((ui.recording || ui.voice_pending) && input.key == Key::escape) {
                    if (engine) {
                        ui.busy = true;
                        engine->cancel_voice();
                    }
                    dirty = true;
                    continue;
                }
                const auto prev_page = ui.page;
                auto action = ui.handle(input);
                if (engine && prev_page == Page::chat && ui.page != Page::chat)
                    engine->stop_playback();
                dirty = true;
                if (action == Action::quit) {
                    running = false;
                    break;
                }
                if (!engine)
                    continue;
                if (action != Action::none && action != Action::playback)
                    ui.busy = true;
                switch (action) {
                case Action::login:
                    engine->login();
                    break;
                case Action::configure:
                    engine->configure(ui.draft);
                    break;
                case Action::refresh:
                    engine->open_fixed_chat();
                    break;
                case Action::playback:
                    ui.render();
                    engine->toggle_playback(ui.visible_voice_clips);
                    break;
                case Action::voice:
                    engine->voice(true, ui.current);
                    break;
                case Action::logout:
                    engine->logout(false);
                    break;
                case Action::switch_workspace:
                    engine->logout(true);
                    break;
                default:
                    break;
                }
            }
            if (ui.recording || updater.installing())
                backlight.wake();
            else
                backlight.expire(now);
            if (backlight.asleep()) {
                wait(next_battery_read);
                continue;
            }
            if (dirty) {
                ui.render();
                dirty = false;
            }
            if (auto current_volume = volume.poll())
                ui.show_volume(*current_volume);
            ui.dismiss_expired_volume();
            ui.update_clock();
            if (engine)
                ui.update_playback_clock(engine->playback_seconds());
            ui.update_recording_clock();
            const auto lv_delay = lv_timer_handler();
            display.present();
            if (!screenshot.empty() && now - start > std::chrono::milliseconds(600)) {
                display.screenshot(screenshot);
                running = false;
            }
            // The device can block on evdev; SDL events have no pollable descriptor.
            auto deadline =
                std::min({device ? next_battery_read : now + milliseconds(33), backlight.deadline(),
                          ui.volume_deadline(), steady_clock::now() + milliseconds(lv_delay)});
            const auto wall = duration_cast<milliseconds>(system_clock::now().time_since_epoch());
            deadline = std::min(deadline, steady_clock::now() + (minutes(1) - wall % minutes(1)));
            if (volume.pending())
                deadline = std::min(deadline, now + milliseconds(10));
            if (running || updater.installing())
                wait(deadline);
        }
        engine.reset();
        if (restart && !interrupted) {
            keyboard.reset();
            std::vector<char *> arguments;
            arguments.push_back(const_cast<char *>("okou-zero"));
            for (int i = 1; i < argc; ++i)
                arguments.push_back(argv[i]);
            arguments.push_back(nullptr);
            execv("/usr/bin/okou-zero", arguments.data());
            throw std::runtime_error("Update installed; reopen Okou from the launcher");
        }
        return 0;
    } catch (const std::exception &error) {
        diagnostics::log_error(diagnostics::Stage::startup, error);
        std::cerr
            << "Okou Zero could not start. Check configuration and display/input permissions.\n";
        return 1;
    }
}
