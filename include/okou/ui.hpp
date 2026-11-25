#pragma once
#include "okou/chat_navigation.hpp"
#include "okou/core.hpp"
#include "okou/markdown.hpp"
#include "okou/platform.hpp"
#include "okou/recording_waveform.hpp"
#include <chrono>
#include <ctime>

namespace okou {
class EmojiRenderer;
enum class Page { login, setup, chat, settings };
enum class Action {
    none,
    login,
    configure,
    refresh,
    voice,
    playback,
    switch_workspace,
    logout,
    quit
};
class UI {
  public:
    Page page = Page::login;
    std::vector<Message> messages;
    Thread current;
    std::string draft, status = "Sign in to continue", user_id, org_id;
    std::string user_code, verification_uri;
    std::string update_message, playback_status;
    std::vector<std::string> visible_voice_clips;
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
    void demo(const std::string &screen);
    void select_thread(const Thread &thread);
    void reset_account();
    void complete_send(const std::string &thread_id, const std::string &text, bool was_uncertain,
                       const std::string &event_id = {}, const std::string &voice_file_id = {});
    void update_history(std::vector<Message> history);
    void update_recording_clock();
    void update_clock();
    void update_playback_clock(unsigned seconds);
    bool take_local_reply();
    std::chrono::steady_clock::time_point volume_deadline() const {
        return volume_overlay_ ? volume_until_ : std::chrono::steady_clock::time_point::max();
    }
    void reconcile_send(const std::string &thread_id, bool pending);

  private:
    lv_obj_t *root_ = nullptr;
    lv_obj_t *header_ = nullptr;
    lv_obj_t *body_ = nullptr;
    lv_obj_t *footer_ = nullptr;
    lv_obj_t *volume_overlay_ = nullptr;
    std::chrono::steady_clock::time_point volume_until_;
    lv_font_t *font_ = nullptr;
    lv_font_t *detail_font_ = nullptr;
    MarkdownFonts markdown_fonts_;
    std::unique_ptr<EmojiRenderer> emoji_;
    std::string pending_reply_to_;
    ChatNavigation navigation_;
    std::vector<Message> acknowledged_messages_;
    lv_obj_t *recording_overlay_ = nullptr;
    lv_obj_t *recording_clock_ = nullptr;
    lv_obj_t *playback_clock_ = nullptr;
    long playback_seconds_ = -1;
    long recording_seconds_ = -1;
    lv_obj_t *clock_ = nullptr;
    std::time_t clock_minute_ = -1;
    int turn_direction_ = 0;
    int reading_line_ = 22;
    void render_voice_panel(bool capturing);
    void render_reader();
    lv_obj_t *voice_state_label(lv_obj_t *parent, const char *text, int y, std::uint32_t color);
    void render_sending(const char *state);
    void render_header();
    lv_obj_t *label(lv_obj_t *parent, const std::string &text, int x, int y, int width,
                    std::uint32_t color, int height = 0);
    lv_obj_t *row(const std::string &title, const std::string &detail, int y, bool selected,
                  std::uint32_t color, const char *icon = nullptr, bool emoji_badge = false);
};
} // namespace okou
