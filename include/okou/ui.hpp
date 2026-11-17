#pragma once
#include "okou/core.hpp"
#include "okou/platform.hpp"

namespace okou {
enum class Page { login, setup, threads, projects, chat, settings };
enum class Action { none, login, configure, refresh, open, send, voice, switch_workspace, logout, quit };
class UI {
public:
    Page page = Page::login;
    ListState list;
    std::vector<Message> messages;
    Thread current;
    std::string draft, status = "Sign in to continue", user_id, org_id;
    std::string user_code, verification_uri;
    std::string project_filter;
    bool busy = false, recording = false, live = false, uncertain = false;
    int selected = 0;
    explicit UI(lv_display_t* display);
    ~UI();
    Action handle(const Input& input);
    void render();
    void demo(const std::string& screen);
    void select_thread(const Thread& thread);
    std::vector<std::size_t> visible_threads() const;
private:
    lv_obj_t* root_ = nullptr;
    lv_obj_t* header_ = nullptr;
    lv_obj_t* body_ = nullptr;
    lv_obj_t* footer_ = nullptr;
    lv_font_t* font_ = nullptr;
    int history_scroll_ = 0;
    std::map<std::string, std::string> drafts_;
    lv_obj_t* label(lv_obj_t* parent, const std::string& text, int x, int y,
                    int width, std::uint32_t color, int height = 0);
    void row(const std::string& title, const std::string& detail, int y, bool selected, std::uint32_t color);
};
} // namespace okou
