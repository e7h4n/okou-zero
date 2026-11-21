#pragma once
#include "okou/core.hpp"
#include "okou/platform.hpp"

namespace okou {
class EmojiRenderer;
enum class Page { login, setup, threads, projects, chat, settings };
enum class Action {
    none,
    login,
    configure,
    refresh,
    filter_project,
    open,
    send,
    voice,
    switch_workspace,
    logout,
    quit
};
class UI {
  public:
    Page page = Page::login;
    ListState list;
    std::vector<Message> messages;
    Thread current;
    std::string draft, status = "Sign in to continue", user_id, org_id;
    std::string user_code, verification_uri;
    std::string project_filter;
    std::string update_message;
    BatteryStatus battery;
    bool busy = false, recording = false, uncertain = false;
    bool submitting = false, voice_pending = false, voice_working = false;
    bool push_to_talk = false;
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
                       bool preserve_draft = false, const std::string &event_id = {});
    void update_history(std::vector<Message> history);
    void append_dictation(const std::string &thread_id, const std::string &text);
    void reconcile_send(const std::string &thread_id, bool pending);
    std::vector<std::size_t> visible_threads() const;

  private:
    lv_obj_t *root_ = nullptr;
    lv_obj_t *header_ = nullptr;
    lv_obj_t *body_ = nullptr;
    lv_obj_t *footer_ = nullptr;
    lv_font_t *font_ = nullptr;
    lv_font_t *detail_font_ = nullptr;
    std::unique_ptr<EmojiRenderer> emoji_;
    int history_scroll_ = 0;
    std::vector<Message> acknowledged_messages_;
    std::map<std::string, std::string> drafts_;
    std::map<std::string, std::string> pending_texts_;
    lv_obj_t *label(lv_obj_t *parent, const std::string &text, int x, int y, int width,
                    std::uint32_t color, int height = 0);
    lv_obj_t *row(const std::string &title, const std::string &detail, int y, bool selected,
                  std::uint32_t color, const char *icon = nullptr, bool emoji_badge = false);
};
} // namespace okou
