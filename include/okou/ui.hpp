#pragma once
#include "okou/chat_navigation.hpp"
#include "okou/core.hpp"
#include "okou/markdown.hpp"
#include "okou/platform.hpp"
#include <chrono>

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
    bool busy = false, recording = false, uncertain = false;
    bool submitting = false, voice_pending = false, voice_working = false;
    std::chrono::steady_clock::time_point recording_started{};
    int selected = 0;
    explicit UI(lv_display_t *display);
    ~UI();
    Action handle(const Input &input);
    void render();
    bool update_safe() const;
    void demo(const std::string &screen);
    void select_thread(const Thread &thread);
    void reset_account();
    void complete_send(const std::string &thread_id, const std::string &text, bool was_uncertain,
                       const std::string &event_id = {}, const std::string &voice_file_id = {});
    void update_history(std::vector<Message> history);
    void update_recording_clock();
    void reconcile_send(const std::string &thread_id, bool pending);

  private:
    lv_obj_t *root_ = nullptr;
    lv_obj_t *header_ = nullptr;
    lv_obj_t *body_ = nullptr;
    lv_obj_t *footer_ = nullptr;
    lv_font_t *font_ = nullptr;
    lv_font_t *detail_font_ = nullptr;
    MarkdownFonts markdown_fonts_;
    std::unique_ptr<EmojiRenderer> emoji_;
    ChatNavigation navigation_;
    std::vector<Message> acknowledged_messages_;
    lv_obj_t *recording_overlay_ = nullptr;
    lv_obj_t *recording_clock_ = nullptr;
    long recording_seconds_ = -1;
    void render_recording();
    lv_obj_t *label(lv_obj_t *parent, const std::string &text, int x, int y, int width,
                    std::uint32_t color, int height = 0);
    lv_obj_t *row(const std::string &title, const std::string &detail, int y, bool selected,
                  std::uint32_t color, const char *icon = nullptr, bool emoji_badge = false);
};
} // namespace okou
