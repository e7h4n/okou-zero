#include "okou/ui.hpp"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <thread>

namespace okou {
namespace {
std::atomic_bool interrupted{false};
void interrupt(int) { interrupted.store(true); }
using Update = std::function<void(UI&)>;
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
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> tasks_;
    std::deque<Update> updates_;
    std::thread worker_;
    std::atomic_bool stopping_{false}, metadata_pending_{false}, indicators_pending_{false}, history_pending_{false};
    std::string selected_thread_;
    Json indicators_ = Json::object();
    void emit(Update update) {
        std::lock_guard<std::mutex> guard(mutex_);
        updates_.push_back(std::move(update));
    }
    void enqueue(std::function<void()> task) {
        { std::lock_guard<std::mutex> guard(mutex_); tasks_.push_back(std::move(task)); }
        cv_.notify_all();
    }
    void ensure(Session& s) {
        if (!s.client) throw std::runtime_error("Sign in to continue");
        auto now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        if (s.tokens.expires_at <= now + 60) {
            s.tokens = s.auth.refresh(s.tokens);
            s.auth.save(s.tokens);
            s.client->set_tokens(s.tokens);
        }
    }
    void error(const std::exception& ex) {
        const auto* http = dynamic_cast<const HttpError*>(&ex);
        bool uncertain = http && http->uncertain;
        long status = http ? http->status : 0;
        std::string text = status == 401 ? "Sign-in expired; sign in again" : status == 403
                        ? "Access not granted; check OAuth scopes" : uncertain
                        ? "Send uncertain: refresh history; do not resend" : "Could not complete request; Ctrl+L to refresh";
        emit([text, uncertain](UI& ui) { ui.status = text; ui.busy = false; if (uncertain) ui.uncertain = true; });
    }
    static void apply_indicators(UI& ui, const Json& indicators) {
        auto threads = indicators.value("threads", Json::object());
        for (auto& thread : ui.list.threads) {
            auto value = threads.value(thread.id, std::string{});
            thread.active = value == "active";
            thread.unread = value == "unread" && !thread.muted;
        }
    }
    void stop_realtime() {
        if (realtime_) { realtime_->stop(); realtime_.reset(); }
        emit([](UI& ui) { ui.live = false; });
    }
    void start_realtime(const std::shared_ptr<Session>& s) {
        stop_realtime();
        realtime_ = std::make_unique<Realtime>([s] {
            std::lock_guard<std::mutex> guard(s->io);
            if (s->cancelled.load() || !s->client) throw std::runtime_error("Session closed");
            auto now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
            if (s->tokens.expires_at <= now + 60) {
                s->tokens = s->auth.refresh(s->tokens);
                s->auth.save(s->tokens);
                s->client->set_tokens(s->tokens);
            }
            return s->client->realtime_token();
        }, s->tokens.user_id, s->tokens.org_id, [this, weak = std::weak_ptr<Session>(s)](const std::string& name) {
            auto current = weak.lock();
            if (!current || current != std::atomic_load(&session_) || stopping_) return;
            if (name == "connection:connected") emit([](UI& ui) { ui.live = true; ui.status = "Connected"; });
            else if (name == "connection:disconnected") emit([](UI& ui) { ui.live = false; ui.status = "Reconnecting..."; });
            if (name == "connection:gap") { refresh(false); refresh_indicators(); refresh_history(); }
            else if (name == "threadListChanged") refresh(false);
            else if (name == "chatIndicatorsChanged" || name == "chatThreadReadCursorUpdated") refresh_indicators();
            else if (name.rfind("chatThreadMessageCreated:", 0) == 0 || name.rfind("chatThreadDetailChanged:", 0) == 0) refresh_history();
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
                    if (stopping_) break;
                    task = std::move(tasks_.front()); tasks_.pop_front();
                }
                try { task(); } catch (const std::exception& ex) { if (!stopping_) error(ex); }
            }
        });
    }
    ~Engine() {
        stopping_ = true;
        auto s = std::atomic_load(&session_);
        s->cancelled = true;
        cv_.notify_all();
        if (worker_.joinable()) worker_.join();
        stop_realtime();
        if (audio_) audio_->cancel();
    }
    bool drain(UI& ui) {
        std::deque<Update> batch;
        { std::lock_guard<std::mutex> guard(mutex_); batch.swap(updates_); }
        for (auto& update : batch) update(ui);
        return !batch.empty();
    }
    void initialize() {
        enqueue([this] {
            auto s = std::atomic_load(&session_);
            if (s->config.oauth_client_id.empty()) {
                emit([](UI& ui) { ui.page = Page::setup; ui.busy = false; ui.status = "See docs/auth-setup.md"; });
                return;
            }
            std::lock_guard<std::mutex> guard(s->io);
            s->tokens = s->auth.load();
            if (s->tokens.access_token.empty()) { emit([](UI& ui) { ui.page = Page::login; ui.busy = false; }); return; }
            s->client = std::make_unique<Client>(s->config, s->http, s->tokens);
            ensure(*s);
            auto tokens = s->tokens;
            emit([tokens](UI& ui) { ui.user_id = tokens.user_id; ui.org_id = tokens.org_id; ui.page = Page::threads; ui.busy = true; ui.status = "Loading chats..."; });
            enqueue([this, s] { start_realtime(s); refresh(false); refresh_indicators(); });
        });
    }
    void configure(const std::string& id) {
        enqueue([this, id] {
            auto old = std::atomic_load(&session_);
            Config cfg = old->config;
            cfg.oauth_client_id = id;
            save_config(cfg);
            auto s = std::make_shared<Session>(cfg);
            std::atomic_store(&session_, s);
            emit([](UI& ui) { ui.draft.clear(); ui.page = Page::login; ui.busy = false; ui.status = "Press Enter to sign in"; });
        });
    }
    void login() {
        enqueue([this] {
            stop_realtime();
            auto s = std::atomic_load(&session_);
            s->cancelled = false;
            DeviceCode code;
            { std::lock_guard<std::mutex> guard(s->io); code = s->auth.begin(); }
            emit([code](UI& ui) { ui.page = Page::login; ui.busy = true; ui.user_code = code.user_code; ui.verification_uri = code.verification_uri; ui.status = "Choose your workspace when approving"; });
            auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(code.expires_in);
            int interval = std::max(1, code.interval);
            while (!stopping_ && std::chrono::steady_clock::now() < deadline) {
                {
                    std::unique_lock<std::mutex> lock(mutex_);
                    if (cv_.wait_for(lock, std::chrono::seconds(interval), [this] { return stopping_.load(); })) return;
                }
                PollResult result;
                { std::lock_guard<std::mutex> guard(s->io); result = s->auth.poll(code); }
                if (result.state == PollState::pending) continue;
                if (result.state == PollState::slow_down) { interval += 5; continue; }
                if (result.state == PollState::denied || result.state == PollState::expired) {
                    emit([](UI& ui) { ui.busy = false; ui.user_code.clear(); ui.verification_uri.clear(); ui.status = "Code expired or denied; Enter to try again"; });
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
                indicators_ = Json::object(); selected_thread_.clear();
                auto tokens = s->tokens;
                emit([tokens](UI& ui) { ui.user_id = tokens.user_id; ui.org_id = tokens.org_id; ui.list = {}; ui.messages.clear(); ui.draft.clear(); ui.user_code.clear(); ui.verification_uri.clear(); ui.page = Page::threads; ui.status = "Loading chats..."; });
                start_realtime(s); refresh(false); refresh_indicators(); return;
            }
            if (!stopping_) emit([](UI& ui) { ui.busy = false; ui.user_code.clear(); ui.status = "Code expired; Enter to try again"; });
        });
    }
    void refresh(bool include_indicators = true) {
        if (metadata_pending_.exchange(true)) return;
        enqueue([this, include_indicators] {
            metadata_pending_ = false;
            auto s = std::atomic_load(&session_);
            ListState list;
            { std::lock_guard<std::mutex> guard(s->io); ensure(*s); list = s->client->list(false); }
            auto indicators = indicators_;
            emit([list = std::move(list), indicators](UI& ui) mutable { ui.list = std::move(list); apply_indicators(ui, indicators); ui.busy = false; ui.status = "Up to date"; });
            if (include_indicators) refresh_indicators();
        });
    }
    void refresh_indicators() {
        if (indicators_pending_.exchange(true)) return;
        enqueue([this] {
            indicators_pending_ = false;
            auto s = std::atomic_load(&session_);
            { std::lock_guard<std::mutex> guard(s->io); ensure(*s); indicators_ = s->client->indicators(); }
            auto indicators = indicators_;
            emit([indicators](UI& ui) { apply_indicators(ui, indicators); });
        });
    }
    void refresh_history() {
        if (history_pending_.exchange(true)) return;
        enqueue([this] {
            history_pending_ = false;
            if (selected_thread_.empty()) return;
            auto id = selected_thread_;
            auto s = std::atomic_load(&session_);
            std::vector<Message> messages;
            bool pending = false;
            { std::lock_guard<std::mutex> guard(s->io); ensure(*s); messages = s->client->history(id); pending = s->client->has_pending_send(id); }
            emit([id, messages = std::move(messages), pending](UI& ui) mutable {
                if (ui.current.id == id) { ui.messages = std::move(messages); ui.uncertain = pending; ui.busy = false; ui.status = pending ? "Send uncertain; do not resend" : "Up to date"; }
            });
        });
    }
    void open(const Thread& thread) {
        enqueue([this, thread] {
            selected_thread_ = thread.id;
            refresh_history();
            auto s = std::atomic_load(&session_);
            { std::lock_guard<std::mutex> guard(s->io); ensure(*s); s->client->mark_read(thread.id); }
            refresh_indicators();
        });
    }
    void send(const Thread& thread, const std::string& text) {
        enqueue([this, thread, text] {
            auto s = std::atomic_load(&session_);
            SendResult result;
            { std::lock_guard<std::mutex> guard(s->io); ensure(*s); result = s->client->send(thread, text); }
            emit([id = thread.id, text, uncertain = result.uncertain](UI& ui) {
                if (ui.current.id == id) {
                    if (!uncertain && ui.draft == text) ui.draft.clear();
                    ui.uncertain = uncertain;
                }
                ui.busy = false;
                ui.status = uncertain ? "Send uncertain; do not resend" : "Message accepted";
            });
            refresh_history(); refresh_indicators();
        });
    }
    void voice(bool finish) {
        enqueue([this, finish] {
            auto s = std::atomic_load(&session_);
            if (!finish) {
                audio_ = std::make_unique<AudioCapture>(s->config.capture_device);
                audio_->start();
                emit([](UI& ui) { ui.recording = true; ui.busy = false; ui.status = "Recording; Ctrl+R finish, Esc cancel"; });
                return;
            }
            if (!audio_) return;
            auto pcm = audio_->finish(); audio_.reset();
            emit([](UI& ui) { ui.recording = false; ui.busy = true; ui.status = "Transcribing..."; });
            std::vector<std::string> segments;
            constexpr std::size_t window = 15 * 16000, step = 13 * 16000;
            for (std::size_t start = 0; start < pcm.size() && !stopping_; start += step) {
                auto end = std::min(start + window, pcm.size());
                if (start > 0 && end - start <= 2 * 16000) break;
                std::vector<std::int16_t> slice(pcm.begin() + static_cast<std::ptrdiff_t>(start), pcm.begin() + static_cast<std::ptrdiff_t>(end));
                std::string previous = segments.empty() ? "" : segments.back();
                std::string transcript;
                { std::lock_guard<std::mutex> guard(s->io); ensure(*s); transcript = s->client->transcribe(slice, previous, start ? 2.0 : 0.0); }
                if (!transcript.empty()) segments.push_back(std::move(transcript));
                if (end == pcm.size()) break;
            }
            std::string text;
            if (!segments.empty()) {
                std::lock_guard<std::mutex> guard(s->io); ensure(*s); text = s->client->polish(segments);
            }
            emit([text](UI& ui) { if (!text.empty()) { if (!ui.draft.empty()) ui.draft += ' '; ui.draft += text; } ui.busy = false; ui.status = "Dictation added to draft; review before sending"; });
        });
    }
    void cancel_voice() {
        enqueue([this] { if (audio_) { audio_->cancel(); audio_.reset(); } emit([](UI& ui) { ui.recording = false; ui.busy = false; ui.status = "Recording discarded"; }); });
    }
    void logout(bool switch_workspace) {
        enqueue([this, switch_workspace] {
            stop_realtime();
            if (audio_) { audio_->cancel(); audio_.reset(); }
            auto s = std::atomic_load(&session_);
            { std::lock_guard<std::mutex> guard(s->io); s->auth.logout(s->tokens); s->tokens = {}; s->client.reset(); }
            selected_thread_.clear(); indicators_ = Json::object();
            emit([switch_workspace](UI& ui) { ui.list = {}; ui.messages.clear(); ui.draft.clear(); ui.current = {}; ui.user_id.clear(); ui.org_id.clear(); ui.recording = false; ui.uncertain = false; ui.live = false; ui.busy = false; ui.page = Page::login; ui.status = switch_workspace ? "Sign in and approve the new workspace" : "Signed out"; });
        });
    }
};
} // namespace
} // namespace okou

int main(int argc, char** argv) {
    using namespace okou;
    try {
        bool demo = false, self_test = false;
        std::string screen = "threads", screenshot, config_path;
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--version") { std::cout << "okou-zero " << OKOU_ZERO_VERSION << '\n'; return 0; }
            if (arg == "--demo") demo = true;
            else if (arg == "--demo-screen" && i + 1 < argc) { demo = true; screen = argv[++i]; }
            else if (arg == "--self-test-ui") { demo = self_test = true; }
            else if (arg == "--screenshot" && i + 1 < argc) screenshot = argv[++i];
            else if (arg == "--config" && i + 1 < argc) config_path = argv[++i];
            else if (arg == "--help") {
                std::cout << "Okou Zero\n  --version\n  --config PATH\n  --demo [--demo-screen login|threads|projects|chat]\n  --screenshot PATH (simulator only)\n  --self-test-ui\n"; return 0;
            } else throw std::runtime_error("Unknown command-line option");
        }
        if (!config_path.empty()) setenv("OKOU_ZERO_CONFIG", config_path.c_str(), 1);
        auto config = load_config(config_path);
        std::signal(SIGINT, interrupt); std::signal(SIGTERM, interrupt);
        lv_init();
        Display display(OKOU_ZERO_DESKTOP, config.framebuffer);
        Keyboard keyboard(OKOU_ZERO_DESKTOP, config.keyboard);
        UI ui(display.get());
        std::unique_ptr<Engine> engine;
        if (demo) ui.demo(screen);
        else { ui.busy = true; engine = std::make_unique<Engine>(config); engine->initialize(); }
        if (self_test) {
            auto check = [](bool condition) { if (!condition) throw std::runtime_error("UI keyboard test failed"); };
            ui.handle({Key::down, {}, false, false}); check(ui.selected == 1);
            check(ui.handle({Key::enter, {}, false, false}) == Action::open); check(ui.page == Page::chat);
            ui.handle({Key::text, "中文 input!", false, false});
            check(ui.draft == "中文 input!"); ui.handle({Key::backspace, {}, false, false}); check(ui.draft == "中文 input");
            check(ui.handle({Key::enter, {}, false, false}) == Action::send);
            ui.uncertain = true; check(ui.handle({Key::enter, {}, false, false}) == Action::none); ui.uncertain = false;
            ui.handle({Key::escape, {}, false, false}); ui.handle({Key::tab, {}, false, false}); check(ui.page == Page::projects);
            ui.handle({Key::down, {}, false, false}); ui.handle({Key::down, {}, false, false});
            ui.handle({Key::enter, {}, false, false}); check(ui.project_filter == "project_demo"); check(ui.visible_threads().size() == 2);
            ui.demo("chat");
        }
        auto last = std::chrono::steady_clock::now();
        auto start = last;
        bool dirty = true, running = true;
        while (running && !interrupted) {
            auto now = std::chrono::steady_clock::now();
            lv_tick_inc(static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now - last).count())); last = now;
            if (engine && engine->drain(ui)) dirty = true;
            for (const auto& input : keyboard.poll()) {
                if (ui.recording && input.key == Key::escape) {
                    if (engine) { ui.busy = true; engine->cancel_voice(); }
                    dirty = true; continue;
                }
                auto action = ui.handle(input);
                dirty = true;
                if (action == Action::quit) { running = false; break; }
                if (!engine) continue;
                if (action != Action::none) ui.busy = true;
                switch (action) {
                case Action::login: engine->login(); break;
                case Action::configure: engine->configure(ui.draft); break;
                case Action::refresh: engine->refresh(); engine->refresh_history(); break;
                case Action::open: engine->open(ui.current); break;
                case Action::send: engine->send(ui.current, ui.draft); break;
                case Action::voice: engine->voice(ui.recording); break;
                case Action::logout: engine->logout(false); break;
                case Action::switch_workspace: engine->logout(true); break;
                default: break;
                }
            }
            if (dirty) { ui.render(); dirty = false; }
            lv_timer_handler(); display.present();
            if (!screenshot.empty() && now - start > std::chrono::milliseconds(600)) {
                display.screenshot(screenshot); running = false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        engine.reset();
        return 0;
    } catch (const std::exception&) {
        std::cerr << "Okou Zero could not start. Check configuration and display/input permissions.\n";
        return 1;
    }
}
