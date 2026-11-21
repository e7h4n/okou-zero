#include "emoji.hpp"
#include "okou/diagnostics.hpp"
#include "okou/dictation.hpp"
#include "okou/ui.hpp"
#include "update_process.hpp"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <thread>
#include <unistd.h>

namespace okou {
namespace {
std::atomic_bool interrupted{false};
void interrupt(int) {
    interrupted.store(true);
}
using Update = std::function<void(UI &)>;
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
    std::shared_ptr<Session> session_;
    std::unique_ptr<Realtime> realtime_;
    std::unique_ptr<AudioCapture> audio_;
    std::unique_ptr<Dictation> dictation_;
    std::string voice_thread_;
    std::atomic_bool sending_{false}, voice_cancelled_{false};
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> tasks_;
    std::deque<Update> updates_;
    std::thread worker_;
    std::atomic_bool stopping_{false}, metadata_pending_{false}, indicators_pending_{false},
        history_pending_{false};
    std::string selected_thread_;
    std::string selected_project_;
    Json indicators_ = Json::object();
    void emit(Update update) {
        std::lock_guard<std::mutex> guard(mutex_);
        updates_.push_back(std::move(update));
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
        emit([text](UI &ui) {
            ui.status = text;
            ui.busy = false;
        });
    }
    static void apply_indicators(UI &ui, const Json &indicators) {
        auto threads = indicators.value("threads", Json::object());
        for (auto &thread : ui.list.threads) {
            auto value = threads.value(thread.id, std::string{});
            thread.active = value == "active";
            thread.unread = value == "unread" && !thread.muted;
        }
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
                if (name == "connection:connected")
                    emit([](UI &ui) { ui.status = "Connected"; });
                else if (name == "connection:disconnected")
                    emit([](UI &ui) { ui.status = "Reconnecting..."; });
                if (name == "connection:gap") {
                    refresh(false);
                    refresh_indicators();
                    refresh_history();
                } else if (name == "threadListChanged")
                    refresh(false);
                else if (name == "chatIndicatorsChanged" || name == "chatThreadReadCursorUpdated")
                    refresh_indicators();
                else if (name.rfind("chatThreadMessageCreated:", 0) == 0 ||
                         name.rfind("chatThreadDetailChanged:", 0) == 0)
                    refresh_history();
            });
        realtime_->start();
    }

  public:
    explicit Engine(Config config) : session_(std::make_shared<Session>(std::move(config))) {
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
                ui.page = Page::threads;
                ui.busy = true;
                ui.status = "Loading chats...";
            });
            enqueue([this, s] {
                start_realtime(s);
                refresh(false);
                refresh_indicators();
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
                indicators_ = Json::object();
                selected_thread_.clear();
                selected_project_.clear();
                auto tokens = s->tokens;
                emit([tokens](UI &ui) {
                    ui.reset_account();
                    ui.user_id = tokens.user_id;
                    ui.org_id = tokens.org_id;
                    ui.page = Page::threads;
                    ui.busy = true;
                    ui.status = "Loading chats...";
                });
                start_realtime(s);
                refresh(false);
                refresh_indicators();
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
    void refresh(bool include_indicators = true) {
        if (metadata_pending_.exchange(true))
            return;
        enqueue([this, include_indicators] {
            metadata_pending_ = false;
            auto s = std::atomic_load(&session_);
            auto project = selected_project_;
            ListState list;
            {
                std::lock_guard<std::mutex> guard(s->io);
                ensure(*s);
                list = s->client->list(false, project);
            }
            auto indicators = indicators_;
            emit([list = std::move(list), indicators, project](UI &ui) mutable {
                if (ui.project_filter != project)
                    return;
                ui.list = std::move(list);
                apply_indicators(ui, indicators);
                ui.busy = false;
                ui.status = "Up to date";
            });
            if (include_indicators)
                refresh_indicators();
        });
    }
    void select_project(std::string project) {
        enqueue([this, project = std::move(project)] {
            selected_project_ = project;
            refresh(false);
        });
    }
    void refresh_indicators() {
        if (indicators_pending_.exchange(true))
            return;
        enqueue([this] {
            indicators_pending_ = false;
            auto s = std::atomic_load(&session_);
            {
                std::lock_guard<std::mutex> guard(s->io);
                ensure(*s);
                indicators_ = s->client->indicators();
            }
            auto indicators = indicators_;
            emit([indicators](UI &ui) { apply_indicators(ui, indicators); });
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
                    }
                }
            });
        });
    }
    void open(const Thread &thread) {
        enqueue([this, thread] {
            selected_thread_ = thread.id;
            refresh_history();
            auto s = std::atomic_load(&session_);
            {
                std::lock_guard<std::mutex> guard(s->io);
                ensure(*s);
                s->client->mark_read(thread.id);
            }
            refresh_indicators();
        });
    }
    void send(const Thread &thread, const std::string &text) {
        if (sending_.exchange(true))
            return;
        enqueue([this, thread, text] {
            try {
                auto s = std::atomic_load(&session_);
                SendResult result;
                {
                    std::lock_guard<std::mutex> guard(s->io);
                    ensure(*s);
                    result = s->client->send(thread, text);
                }
                emit([id = thread.id, text, result = std::move(result)](UI &ui) {
                    const bool uncertain = result.uncertain;
                    ui.complete_send(id, text, uncertain, result.client_event_id);
                    ui.busy = false;
                    ui.status = uncertain ? "Send uncertain; do not resend" : "Message accepted";
                });
                sending_ = false;
                refresh_history();
                refresh_indicators();
            } catch (const std::exception &ex) {
                sending_ = false;
                error(ex, diagnostics::Stage::chat_send);
                emit([id = thread.id, text](UI &ui) {
                    ui.submitting = false;
                    if (ui.current.id == id) {
                        ui.retained_voice_text = text;
                        ui.voice_pending = true;
                        ui.status = "Voice send failed";
                    }
                });
            }
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
                    audio_->start();
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
                    auto pcm = audio_->finish();
                    audio_.reset();
                    dictation_ = std::make_unique<Dictation>(thread_id, std::move(pcm));
                }
                if (!dictation_) {
                    emit([](UI &ui) {
                        ui.recording = ui.voice_pending = ui.voice_working = ui.busy = false;
                    });
                    return;
                }
                emit([](UI &ui) {
                    ui.recording = false;
                    ui.voice_pending = true;
                    ui.busy = true;
                    ui.status = "Transcribing...";
                });
                auto text = dictation_->process(
                    [this, s](const std::vector<std::int16_t> &pcm, const std::string &previous,
                              double overlap) {
                        std::lock_guard<std::mutex> guard(s->io);
                        ensure(*s);
                        return s->client->transcribe(pcm, previous, overlap);
                    },
                    [this, s](const std::vector<std::string> &segments) {
                        if (segments.size() == 1)
                            return segments.front();
                        std::lock_guard<std::mutex> guard(s->io);
                        ensure(*s);
                        return s->client->polish(segments);
                    },
                    [this] { return stopping_.load() || voice_cancelled_.load(); });
                if (voice_cancelled_ || stopping_)
                    return;
                dictation_.reset();
                emit([this, thread, text = std::move(text)](UI &ui) {
                    if (voice_cancelled_)
                        return;
                    ui.recording = ui.voice_pending = ui.voice_working = ui.busy = false;
                    if (text.find_first_not_of(" \t\r\n") == std::string::npos) {
                        ui.status = "No speech recognized";
                    } else {
                        ui.submitting = ui.busy = true;
                        send(thread, text);
                    }
                });
            } catch (const std::exception &ex) {
                if (audio_) {
                    audio_->cancel();
                    audio_.reset();
                }
                bool retained = dictation_ && dictation_->pending() && !voice_cancelled_;
                if (!retained)
                    dictation_.reset();
                error(ex, diagnostics::Stage::voice);
                emit([retained](UI &ui) {
                    ui.recording = ui.voice_working = ui.busy = false;
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
            dictation_.reset();
            voice_thread_.clear();
            emit([](UI &ui) {
                ui.retained_voice_text.clear();
                ui.recording = ui.voice_pending = ui.voice_working = ui.busy = false;
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
            dictation_.reset();
            voice_thread_.clear();
            auto s = std::atomic_load(&session_);
            {
                std::lock_guard<std::mutex> guard(s->io);
                s->auth.logout(s->tokens);
                s->tokens = {};
                s->client.reset();
            }
            selected_thread_.clear();
            selected_project_.clear();
            indicators_ = Json::object();
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
        std::string screen = "threads", screenshot, config_path;
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
                             "login|threads|projects|chat|recording]\n  --screenshot PATH "
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
        std::unique_ptr<Engine> engine;
        if (demo)
            ui.demo(screen);
        else {
            ui.busy = true;
            engine = std::make_unique<Engine>(config);
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
            ui.handle({Key::down, {}, false, false});
            check(ui.selected == 1);
            check(ui.handle({Key::enter, {}, false, false}) == Action::open);
            check(ui.page == Page::chat);
            check(ui.handle({Key::text, "中文 input!", false, false}) == Action::none);
            check(ui.draft.empty());
            check(ui.handle({Key::enter, {}, false, false}) == Action::none);
            ui.complete_send(ui.current.id, "spoken message", false, "voice_event");
            check(ui.messages.back().id == "voice_event");
            ui.update_history({});
            check(ui.messages.size() == 1);
            ui.update_history({{"voice_event", "user", "spoken message", false}});
            check(ui.messages.size() == 1);
            ui.update_history({});
            check(ui.messages.empty());
            ui.complete_send(ui.current.id, "uncertain voice", true);
            check(ui.uncertain && ui.messages.empty());
            ui.reconcile_send(ui.current.id, false);
            check(!ui.uncertain);
            ui.handle({Key::escape, {}, false, false});
            ui.handle({Key::tab, {}, false, false});
            check(ui.page == Page::projects);
            ui.handle({Key::down, {}, false, false});
            ui.handle({Key::down, {}, false, false});
            ui.handle({Key::enter, {}, false, false});
            check(ui.project_filter == "project_demo");
            check(ui.visible_threads().size() == 2);
            ui.select_thread(ui.list.threads[0]);
            check(ui.handle({Key::record, {}, false, false}) == Action::none);
            ui.recording = true;
            ui.render();
            ui.update_recording_clock();
            check(!ui.update_safe());
            check(ui.handle({Key::escape, {}, false, false}) == Action::none);
            ui.recording = false;
            ui.voice_pending = true;
            ui.retained_voice_text = "failed voice message";
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
        UpdateProcess updater;
        constexpr auto enter_hold_delay = std::chrono::milliseconds(350);
        std::chrono::steady_clock::time_point enter_pressed{};
        std::string enter_thread;
        if (!demo)
            updater.start(false, OKOU_ZERO_VERSION);
        while ((running && !interrupted) || updater.installing()) {
            auto now = std::chrono::steady_clock::now();
            lv_tick_inc(static_cast<std::uint32_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(now - last).count()));
            last = now;
            if (!demo && now >= next_battery_read) {
                next_battery_read = now + std::chrono::seconds(5);
                auto battery = read_battery_status();
                if (battery.percent != ui.battery.percent ||
                    battery.charging != ui.battery.charging) {
                    ui.battery = battery;
                    dirty = true;
                }
            }
            if (engine && engine->drain(ui))
                dirty = true;
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
                    inputs = keyboard->poll();
            } catch (const std::exception &) {
                if (!updater.installing())
                    throw;
                running = false; // Finish the confirmed install before leaving.
            }
            for (auto input : inputs) {
                if (updater.installing())
                    continue;
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
                            ui.status = "Transcribing...";
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
                    engine->configure(ui.draft);
                    break;
                case Action::refresh:
                    engine->refresh();
                    engine->refresh_history();
                    break;
                case Action::filter_project:
                    ui.list.threads.clear();
                    ui.status = "Loading chats...";
                    engine->select_project(ui.project_filter);
                    break;
                case Action::open:
                    engine->open(ui.current);
                    break;
                case Action::voice:
                    if (ui.retained_voice_text.empty()) {
                        engine->voice(true, ui.current);
                    } else {
                        auto text = std::move(ui.retained_voice_text);
                        ui.retained_voice_text.clear();
                        ui.voice_pending = ui.voice_working = false;
                        ui.submitting = true;
                        engine->send(ui.current, text);
                    }
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
            if (dirty) {
                ui.render();
                dirty = false;
            }
            ui.update_recording_clock();
            lv_timer_handler();
            display.present();
            if (!screenshot.empty() && now - start > std::chrono::milliseconds(600)) {
                display.screenshot(screenshot);
                running = false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
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
