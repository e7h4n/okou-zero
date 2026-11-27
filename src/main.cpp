#include "idle_display.hpp"
#include "okou/diagnostics.hpp"
#include "okou/ui.hpp"
#include "update_process.hpp"
#include "volume_control.hpp"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <curl/curl.h>
#include <deque>
#include <iostream>
#include <limits>
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
// The offline store-review tour uses the same native UI and audio envelope as the
// live device. Its synthetic samples never open a microphone, speaker, or network.
struct DemoStep {
    const char *screen;
    unsigned duration_ms;
};
constexpr DemoStep tour_steps[] = {{"welcome", 2800},   {"login", 2000},   {"empty", 1500},
                                   {"recording", 4200}, {"sending", 1250}, {"sent", 2300},
                                   {"chat", 3000}};
void demo_audio(UI &ui, unsigned time_ms, unsigned &last_bin) {
    if (!ui.recording && !ui.submitting)
        return;
    const unsigned bin = (ui.submitting ? 4200 : time_ms) / 16;
    if (bin == last_bin)
        return;
    auto &wave = ui.recording_waveform;
    unsigned first = last_bin < bin ? last_bin + 1 : 0;
    if (first == 0)
        wave.clear();
    for (unsigned i = first; i <= bin; ++i) {
        const double t = i * .016,
                     syllable = std::max(0., std::sin(11 * t + .4)) * std::abs(std::sin(2.8 * t));
        std::array<std::int16_t, 256> samples{};
        samples[0] = static_cast<std::int16_t>(15000 * syllable * syllable);
        samples[1] = -samples[0];
        wave.append(samples.data(), samples.size());
    }
    last_bin = bin;
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
    void start_realtime(const std::shared_ptr<Session> &s, const std::string &thread_id) {
        realtime_.reset();
        std::lock_guard<std::mutex> guard(s->io);
        realtime_ = std::make_unique<Realtime>(
            [this, s] {
                std::lock_guard<std::mutex> guard(s->io);
                if (s->cancelled.load())
                    throw std::runtime_error("Session closed");
                ensure(*s);
                return s->client->realtime_token();
            },
            s->tokens.user_id, s->tokens.org_id, thread_id,
            [this] {
                if (!stopping_)
                    refresh_history();
            });
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
        if (worker_.joinable())
            worker_.join();
        realtime_.reset();
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
            emit([org_id = s->tokens.org_id](UI &ui) {
                ui.org_id = org_id;
                ui.page = Page::chat;
                ui.busy = true;
                ui.status = "Opening your chat...";
            });
            open_fixed_chat();
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
                ui.client_id_input.clear();
                ui.page = Page::login;
                ui.busy = false;
                ui.status = "Press Enter to sign in";
            });
        });
    }
    void login() {
        enqueue([this] {
            realtime_.reset();
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
                    s->auth.save(s->tokens);
                    s->client = std::make_unique<Client>(s->config, s->http, s->tokens);
                }
                selected_thread_.clear();
                emit([org_id = s->tokens.org_id](UI &ui) {
                    ui.reset_account();
                    ui.org_id = org_id;
                    ui.page = Page::chat;
                    ui.busy = true;
                    ui.status = "Opening your chat...";
                });
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
            std::string id;
            {
                std::lock_guard<std::mutex> guard(s->io);
                ensure(*s);
                id = s->client->fixed_thread_id();
            }
            if (id != selected_thread_ || !realtime_) {
                selected_thread_ = id;
                start_realtime(s, id);
            }
            emit([id](UI &ui) {
                if (ui.thread_id != id)
                    ui.open_thread(id);
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
                if (ui.thread_id == id) {
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
    void voice(bool finish, const std::string &thread_id) {
        if (!finish)
            voice_cancelled_ = false;
        enqueue([this, finish, thread_id] {
            auto s = std::atomic_load(&session_);
            try {
                if (!finish) {
                    voice_thread_ = thread_id;
                    audio_ = std::make_unique<AudioCapture>(s->config.capture_device);
                    if (voice_cancelled_)
                        return;
                    audio_->start(waveform_);
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
                if (voice_cancelled_ || stopping_) {
                    voice_pcm_ = std::vector<std::int16_t>();
                    return;
                }
                if (voice_file_id_.empty()) {
                    if (voice_pcm_.empty()) {
                        voice_pcm_ = std::vector<std::int16_t>();
                        voice_thread_.clear();
                        emit([](UI &ui) {
                            ui.recording = ui.voice_pending = ui.voice_working = ui.submitting =
                                ui.busy = false;
                            ui.status = "No audio recorded";
                        });
                        return;
                    }
                    if (voice_recording_too_short(voice_pcm_.size())) {
                        voice_pcm_ = std::vector<std::int16_t>();
                        voice_thread_.clear();
                        emit([](UI &ui) {
                            ui.recording = ui.voice_pending = ui.voice_working = ui.submitting =
                                ui.busy = false;
                            ui.status = "Recording too short";
                        });
                        return;
                    }
                }
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
                    result = voice_file_id_.empty()
                                 ? SendResult{"", true}
                                 : s->client->send_voice(thread_id, voice_file_id_);
                }
                voice_pcm_ = std::vector<std::int16_t>();
                voice_file_id_.clear();
                emit([id = thread_id, result = std::move(result)](UI &ui) {
                    ui.complete_send(id, result);
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
    void cancel_voice(std::string status = "Recording discarded") {
        voice_cancelled_ = true;
        enqueue([this, status = std::move(status)]() mutable {
            if (audio_) {
                audio_->cancel();
                audio_.reset();
            }
            voice_pcm_ = std::vector<std::int16_t>();
            voice_file_id_.clear();
            voice_thread_.clear();
            emit([status = std::move(status)](UI &ui) {
                ui.recording = ui.voice_pending = ui.voice_working = ui.busy = false;
                ui.submitting = false;
                ui.status = status;
            });
        });
    }
    void logout(bool switch_workspace) {
        enqueue([this, switch_workspace] {
            realtime_.reset();
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
                             "tour|welcome|login|expired|empty|chat|history|recording|"
                             "sending|sent|"
                             "uncertain|saved|offline|settings|update]\n  --screenshot PATH "
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
        const bool tour = demo && screen == "tour";
        std::size_t demo_step = 0;
        unsigned last_demo_bin = std::numeric_limits<unsigned>::max();
        if (demo)
            ui.demo(tour ? tour_steps[0].screen : screen);
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
            ui.demo("chat");
            check(ui.page == Page::chat && !ui.thread_id.empty());
            ui.complete_send(ui.thread_id, {"voice_event", false});
            ui.render();
            check(ui.messages.back().id == "voice_event");
            ui.update_history({});
            check(ui.messages.size() == 1);
            ui.update_history({{"voice_event", "user", "spoken message", false}});
            ui.complete_send(ui.thread_id, {"voice_event", false});
            check(ui.messages.size() == 1);
            check(!ui.take_local_reply());
            std::vector<Message> history{
                {"voice_event", "user", "spoken message", false, {}, "local"},
                {"unrelated", "assistant", "Other reply", true, {}, "other"}};
            ui.update_history(history);
            check(!ui.take_local_reply());
            history.push_back({"answer", "assistant", "Reply", true, {}, "local"});
            ui.update_history(history);
            check(ui.take_local_reply() && !ui.take_local_reply());
            ui.update_history({});
            check(ui.messages.empty());
            ui.complete_send(ui.thread_id, {"voice_event", true});
            check(ui.uncertain && ui.messages.empty());
            ui.reconcile_send(ui.thread_id, false);
            check(!ui.uncertain);
            ui.demo("sent");
            check(ui.handle({Key::enter, {}}) == Action::start_recording && ui.recording);
            check(ui.handle({Key::enter, {}}) == Action::skip_short_recording && !ui.recording &&
                  !ui.voice_pending && !ui.submitting && ui.status == "Recording too short");
            ui.voice_working = ui.busy = false;
            check(ui.handle({Key::enter, {}}) == Action::start_recording && ui.recording);
            ui.recording_started =
                std::chrono::steady_clock::now() - std::chrono::milliseconds(500);
            ui.voice_working = ui.busy = false;
            check(ui.recording_too_short(ui.recording_started + std::chrono::milliseconds(999)));
            check(!ui.recording_too_short(ui.recording_started + std::chrono::seconds(1)));
            check(ui.handle({Key::enter, {}}) == Action::skip_short_recording && !ui.recording &&
                  !ui.voice_pending && !ui.submitting && ui.status == "Recording too short");
            ui.voice_working = ui.busy = false;
            check(ui.handle({Key::enter, {}}) == Action::start_recording && ui.recording);
            ui.recording_started = std::chrono::steady_clock::now() - std::chrono::seconds(1);
            ui.voice_working = ui.busy = false;
            check(ui.handle({Key::enter, {}}) == Action::voice && !ui.recording &&
                  ui.voice_pending);
            check(ui.handle({Key::enter, {}}) == Action::none);
            ui.demo("history");
            check(ui.handle({Key::enter, {}}) == Action::start_recording);
            check(ui.handle({Key::escape, {}}) == Action::discard_recording);
            check(!ui.recording && ui.page == Page::chat);
            ui.demo("history");
            ui.handle({Key::escape, {}, false, false});
            check(ui.page == Page::settings);
            ui.handle({Key::escape, {}, false, false});
            check(ui.page == Page::chat);
            ui.recording = true;
            ui.render();
            ui.update_recording_clock();
            check(!ui.update_safe());
            ui.recording = false;
            ui.voice_pending = true;
            check(ui.handle({Key::workspace, {}, false, false}) == Action::none);
            check(ui.handle({Key::retry_voice, {}, false, false}) == Action::voice);
            check(!ui.update_safe());
            ui.reset_account();
            check(ui.update_safe());
            ui.demo("chat");
        }
        auto last = std::chrono::steady_clock::now();
        auto start = last;
        auto demo_started = start;
        auto next_battery_read = last;
        bool dirty = true, running = true, restart = false;
        // On the LCD the previous frame is the launcher wallpaper. Keep it on
        // glass until initialization produces chat, login, setup, or an error.
        bool awaiting_first_screen = !OKOU_ZERO_DESKTOP && !demo;
        UpdateProcess updater;
        VolumeControl volume;
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
            if (demo && !self_test) {
                auto t =
                    static_cast<unsigned>(duration_cast<milliseconds>(now - demo_started).count());
                if (tour && t >= tour_steps[demo_step].duration_ms) {
                    if (++demo_step == std::size(tour_steps))
                        break;
                    ui.demo(tour_steps[demo_step].screen);
                    demo_started = now;
                    t = 0;
                    last_demo_bin = std::numeric_limits<unsigned>::max();
                    dirty = true;
                }
                demo_audio(ui, t, last_demo_bin);
            }
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
                if (input.key == Key::update) {
                    if (updater.view().status == UpdateStatus::ready) {
                        if (!ui.update_safe())
                            ui.update_message = "Finish the current action before updating";
                        else {
                            updater.start(true, OKOU_ZERO_VERSION);
                            ui.update_message = update_hint(updater.view());
                        }
                        dirty = true;
                    }
                    continue;
                }
                auto action = ui.handle(input);
                dirty = true;
                if (action == Action::quit) {
                    running = false;
                    break;
                }
                if (!engine)
                    continue;
                if (action != Action::none)
                    ui.busy = true;
                switch (action) {
                case Action::login:
                    engine->login();
                    break;
                case Action::configure:
                    engine->configure(ui.client_id_input);
                    break;
                case Action::refresh:
                    engine->open_fixed_chat();
                    break;
                case Action::start_recording:
                    engine->voice(false, ui.thread_id);
                    break;
                case Action::discard_recording:
                    engine->cancel_voice();
                    break;
                case Action::skip_short_recording:
                    engine->cancel_voice("Recording too short");
                    break;
                case Action::voice:
                    engine->voice(true, ui.thread_id);
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
