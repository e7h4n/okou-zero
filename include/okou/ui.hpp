#pragma once
#include "okou/chat_navigation.hpp"
#include "okou/core.hpp"
#include "okou/markdown.hpp"
#include "okou/platform.hpp"
#include "okou/recording_waveform.hpp"
#include <chrono>
#include <ctime>

namespace okou {
class PocketOrbit;
enum class Page { login, setup, chat, settings };
enum class Action {
    none,
    login,
    configure,
    refresh,
    voice,
    start_recording,
    discard_recording,
    skip_short_recording,
    switch_workspace,
    logout,
    quit
};
class UI {
  public:
    Page page = Page::login;
    std::vector<Message> messages;
    std::string thread_id, client_id_input, status = "Sign in to continue", org_id;
    std::string user_code, verification_uri;
    std::string update_message;
    BatteryStatus battery;
    RecordingWaveform recording_waveform;
    bool busy = false, recording = false, uncertain = false;
    bool submitting = false, voice_pending = false, voice_working = false;
    std::chrono::steady_clock::time_point recording_started{};
    int selected = 0;
    explicit UI(lv_display_t *display);
    ~UI();
    Action handle(const Input &input);
    void render();
    void show_volume(VolumeStatus volume);
    void dismiss_expired_volume();
    bool update_safe() const;
    bool recording_too_short(
        std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) const;
    void demo(const std::string &screen);
    void open_thread(const std::string &id);
    void reset_account();
    void complete_send(const std::string &id, const SendResult &result);
    void update_history(std::vector<Message> history);
    void update_recording_clock();
    void update_clock();
    bool take_local_reply();
    std::chrono::steady_clock::time_point volume_deadline() const {
        return volume_overlay_ ? volume_until_ : std::chrono::steady_clock::time_point::max();
    }
    void reconcile_send(const std::string &id, bool pending);

  private:
    lv_obj_t *root_ = nullptr;
    lv_obj_t *header_ = nullptr;
    lv_obj_t *body_ = nullptr;
    lv_obj_t *footer_ = nullptr;
    lv_obj_t *volume_overlay_ = nullptr;
    std::chrono::steady_clock::time_point volume_until_;
    lv_font_t *font_ = nullptr;
    lv_font_t *detail_font_ = nullptr;
    lv_font_t *greeting_font_ = nullptr, *display_font_ = nullptr;
    std::vector<lv_font_t *> loaded_fonts_;
    std::unique_ptr<PocketOrbit> orbit_;
    MarkdownFonts markdown_fonts_;
    std::string pending_reply_to_;
    ChatNavigation navigation_;
    std::vector<Message> acknowledged_messages_;
    lv_obj_t *recording_clock_ = nullptr;
    long recording_seconds_ = -1;
    lv_obj_t *clock_ = nullptr;
    std::time_t clock_minute_ = -1;
    int turn_direction_ = 0;
    int reading_line_ = 22;
    void render_voice();
    RecordingWaveform::Overview sending_wave_;
    uint32_t send_started_ = 0;
    void render_reader();
    void render_thinking();
    void footer_hint(const std::string &action, const std::string &secondary = {});
    lv_obj_t *type(lv_obj_t *parent, const std::string &text, int x, int y, int width,
                   std::uint32_t color, const lv_font_t *font, int height = 0);
    void render_header();
    lv_obj_t *label(lv_obj_t *parent, const std::string &text, int x, int y, int width,
                    std::uint32_t color, int height = 0);
    void settings_row(const std::string &title, const std::string &detail, int y, bool chosen);
};
} // namespace okou
