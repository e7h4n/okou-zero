#include "okou/ui.hpp"
#include <algorithm>
#include <filesystem>

namespace okou {
namespace {
constexpr std::uint32_t paper = 0xf8f5f3, ink = 0x292724, muted = 0x74706b;
constexpr std::uint32_t accent = 0xe4c6b8, green = 0x246948, border = 0xe4dfdb;
} // namespace
UI::UI(lv_display_t *display) {
    root_ = lv_display_get_screen_active(display);
    lv_obj_remove_style_all(root_);
    lv_obj_set_style_bg_color(root_, lv_color_hex(paper), 0);
    lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, 0);
    lv_obj_set_size(root_, 320, 170);
    const char *paths[] = {"/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
                           "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"};
    for (const auto *path : paths)
        if (std::filesystem::exists(path)) {
            font_ = lv_freetype_font_create(path, LV_FREETYPE_FONT_RENDER_MODE_BITMAP, 13,
                                            LV_FREETYPE_FONT_STYLE_NORMAL);
            if (font_)
                break;
        }
    lv_obj_set_style_text_font(root_, font_ ? font_ : LV_FONT_DEFAULT, 0);
    lv_obj_set_style_text_color(root_, lv_color_hex(ink), 0);
    header_ = lv_obj_create(root_);
    body_ = lv_obj_create(root_);
    footer_ = lv_obj_create(root_);
    for (auto *object : {header_, body_, footer_}) {
        lv_obj_remove_style_all(object);
        lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE);
    }
    lv_obj_set_pos(header_, 0, 0);
    lv_obj_set_size(header_, 320, 25);
    lv_obj_set_pos(body_, 0, 25);
    lv_obj_set_size(body_, 320, 126);
    lv_obj_set_pos(footer_, 0, 151);
    lv_obj_set_size(footer_, 320, 19);
    lv_obj_set_style_bg_color(footer_, lv_color_hex(border), 0);
    lv_obj_set_style_bg_opa(footer_, LV_OPA_COVER, 0);
}
UI::~UI() {
    lv_obj_clean(root_);
    if (font_)
        lv_freetype_font_delete(font_);
}
lv_obj_t *UI::label(lv_obj_t *parent, const std::string &text, int x, int y, int width,
                    std::uint32_t color, int height) {
    auto *object = lv_label_create(parent);
    lv_obj_set_pos(object, x, y);
    lv_obj_set_width(object, width);
    lv_label_set_text(object, text.c_str());
    lv_obj_set_style_text_color(object, lv_color_hex(color), 0);
    if (height > 0) {
        lv_obj_set_height(object, height);
        lv_label_set_long_mode(object, LV_LABEL_LONG_CLIP);
    }
    return object;
}
void UI::row(const std::string &title, const std::string &detail, int y, bool chosen,
             std::uint32_t color) {
    auto *box = lv_obj_create(body_);
    lv_obj_remove_style_all(box);
    lv_obj_set_pos(box, 5, y);
    lv_obj_set_size(box, 310, 28);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    if (chosen) {
        lv_obj_set_style_bg_color(box, lv_color_hex(accent), 0);
        lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(box, 5, 0);
    }
    label(box, title, 7, 1, 290, color, 17);
    if (!detail.empty()) {
        auto *text = label(box, detail, 7, 16, 290, muted, 11);
        lv_obj_set_style_text_font(text, &lv_font_montserrat_12, 0);
    }
}
std::vector<std::size_t> UI::visible_threads() const {
    std::vector<std::size_t> result;
    for (std::size_t i = 0; i < list.threads.size(); ++i) {
        if (project_filter.empty() ||
            (project_filter == "~" && list.threads[i].project_id.empty()) ||
            list.threads[i].project_id == project_filter)
            result.push_back(i);
    }
    return result;
}
void UI::reset_account() {
    drafts_.clear();
    pending_texts_.clear();
    list = {};
    messages.clear();
    current = {};
    draft.clear();
    user_id.clear();
    org_id.clear();
    project_filter.clear();
    user_code.clear();
    verification_uri.clear();
    selected = 0;
    history_scroll_ = 0;
    busy = recording = live = uncertain = submitting = voice_pending = voice_working = false;
}
void UI::complete_send(const std::string &thread_id, const std::string &text, bool was_uncertain) {
    if (current.id == thread_id) {
        if (!was_uncertain && draft == text)
            draft.clear();
        uncertain = was_uncertain;
    }
    auto cached = drafts_.find(thread_id);
    if (!was_uncertain && cached != drafts_.end() && cached->second == text)
        drafts_.erase(cached);
    if (was_uncertain)
        pending_texts_[thread_id] = text;
    else
        pending_texts_.erase(thread_id);
    submitting = false;
}
void UI::reconcile_send(const std::string &thread_id, bool pending) {
    if (current.id == thread_id)
        uncertain = pending;
    auto it = pending_texts_.find(thread_id);
    if (pending || it == pending_texts_.end())
        return;
    if (current.id == thread_id && draft == it->second)
        draft.clear();
    auto cached = drafts_.find(thread_id);
    if (cached != drafts_.end() && cached->second == it->second)
        drafts_.erase(cached);
    pending_texts_.erase(it);
}
void UI::append_dictation(const std::string &thread_id, const std::string &text) {
    if (text.empty())
        return;
    auto &target = current.id == thread_id ? draft : drafts_[thread_id];
    if (target.size() + text.size() + 1 > 128 * 1024)
        return;
    if (!target.empty())
        target += ' ';
    target += text;
}
void UI::select_thread(const Thread &thread) {
    if (!current.id.empty())
        drafts_[current.id] = draft;
    while (drafts_.size() > 128)
        drafts_.erase(drafts_.begin());
    current = thread;
    draft = drafts_[current.id];
    messages.clear();
    history_scroll_ = 0;
    uncertain = false;
    page = Page::chat;
}
Action UI::handle(const Input &input) {
    if (input.key == Key::quit)
        return Action::quit;
    if (input.key == Key::workspace) {
        if (recording || voice_pending)
            return Action::none;
        page = Page::settings;
        selected = 0;
        return Action::none;
    }
    if (input.key == Key::reload && !busy && !recording && !voice_pending)
        return Action::refresh;
    if (page == Page::setup) {
        if (input.key == Key::text && draft.size() < 512)
            draft += input.text;
        if (input.key == Key::backspace)
            erase_utf8(draft);
        if (input.key == Key::enter && !draft.empty() && !busy)
            return Action::configure;
        if (input.key == Key::escape)
            return Action::quit;
        return Action::none;
    }
    if (page == Page::login) {
        if (input.key == Key::enter && !busy)
            return Action::login;
        if (input.key == Key::escape)
            return Action::quit;
        return Action::none;
    }
    if (page == Page::chat) {
        if (input.key == Key::escape && !recording && !voice_pending) {
            drafts_[current.id] = draft;
            page = Page::threads;
            selected = 0;
        } else if (input.key == Key::text && !recording && !submitting && !uncertain &&
                   draft.size() + input.text.size() <= 16000) {
            draft += input.text;
        } else if (input.key == Key::backspace && !recording && !submitting && !uncertain)
            erase_utf8(draft);
        else if (input.key == Key::enter && input.shift && !recording && !submitting &&
                 !uncertain && draft.size() < 16000)
            draft += '\n';
        else if (input.key == Key::enter && !busy && !recording && !voice_pending && !submitting &&
                 !draft.empty() && !uncertain) {
            submitting = true;
            return Action::send;
        } else if (input.key == Key::record && !busy && !submitting && !voice_working) {
            voice_working = true;
            if (!voice_pending)
                recording = true;
            return Action::voice;
        } else if (input.key == Key::up || input.key == Key::page_up)
            history_scroll_ = std::min(history_scroll_ + 48, 100000);
        else if (input.key == Key::down || input.key == Key::page_down)
            history_scroll_ = std::max(history_scroll_ - 48, 0);
        return Action::none;
    }
    if (input.key == Key::escape) {
        page = page == Page::settings ? Page::threads : Page::settings;
        selected = 0;
        return Action::none;
    }
    if (input.key == Key::tab && page != Page::settings) {
        page = page == Page::projects ? Page::threads : Page::projects;
        selected = 0;
        return Action::none;
    }
    int count = page == Page::settings   ? 4
                : page == Page::projects ? static_cast<int>(list.projects.size()) + 2
                                         : static_cast<int>(visible_threads().size());
    if (input.key == Key::up)
        selected = std::max(selected - 1, 0);
    if (input.key == Key::down)
        selected = std::min(selected + 1, std::max(count - 1, 0));
    if (input.key == Key::enter && !busy && count) {
        if (page == Page::settings) {
            if (selected == 0)
                return Action::switch_workspace;
            if (selected == 1)
                return Action::logout;
            if (selected == 2)
                return Action::refresh;
            return Action::quit;
        }
        if (page == Page::projects) {
            project_filter = selected == 0 ? ""
                             : selected == 1
                                 ? "~"
                                 : list.projects.at(static_cast<std::size_t>(selected - 2)).id;
            page = Page::threads;
            selected = 0;
        } else {
            auto visible = visible_threads();
            if (selected < static_cast<int>(visible.size())) {
                select_thread(list.threads[visible[static_cast<std::size_t>(selected)]]);
                return Action::open;
            }
        }
    }
    return Action::none;
}
void UI::render() {
    lv_obj_clean(header_);
    lv_obj_clean(body_);
    lv_obj_clean(footer_);
    std::string title = "Okou";
    if (page == Page::threads)
        title = project_filter.empty() ? "Okou / chats" : "Okou / project chats";
    else if (page == Page::projects)
        title = "Okou / projects";
    else if (page == Page::chat)
        title = current.title.empty() ? "Chat" : current.title;
    else if (page == Page::settings)
        title = "Okou / workspace";
    label(header_, title, 8, 3, 240, ink, 20);
    label(header_,
          recording ? "REC"
          : busy    ? "..."
          : live    ? "Live"
                    : "Offline",
          257, 3, 58,
          recording ? 0xa8342b
          : live    ? green
                    : muted,
          20);
    std::string help;
    if (page == Page::setup) {
        label(body_, "Connect your Okou OAuth app", 8, 4, 304, ink);
        label(body_, "Public client ID (not a secret)", 8, 28, 304, muted);
        auto *field =
            label(body_, draft.empty() ? "Type the client ID here" : draft, 8, 54, 304, ink, 38);
        lv_obj_set_style_bg_color(field, lv_color_hex(0xffffff), 0);
        lv_obj_set_style_bg_opa(field, LV_OPA_COVER, 0);
        label(body_, status, 8, 98, 304, muted, 22);
        help = "Enter save   Esc exit";
    } else if (page == Page::login) {
        label(body_,
              user_code.empty() ? "Your chats, on your Cardputer"
                                : "Approve sign-in in your browser",
              8, 5, 304, ink);
        if (!user_code.empty()) {
            auto *code = label(body_, user_code, 8, 31, 304, ink);
            lv_obj_set_style_text_font(code, &lv_font_montserrat_16, 0);
            label(body_, verification_uri, 8, 58, 304, muted, 38);
        } else
            label(body_, "Sign in with a code. No password\nis entered on this device.", 8, 35, 304,
                  muted);
        label(body_, status, 8, 101, 304, muted, 22);
        help = "Enter sign in   Esc exit";
    } else if (page == Page::settings) {
        const std::vector<std::string> menu{"Switch workspace", "Sign out", "Refresh chats",
                                            "Return to launcher"};
        for (int i = 0; i < 4; ++i)
            row(menu[static_cast<std::size_t>(i)], i == 0 ? org_id : "", i * 29, selected == i,
                ink);
        help = "Up/down select   Enter   Esc back";
    } else if (page == Page::threads || page == Page::projects) {
        if (page == Page::threads) {
            auto visible = visible_threads();
            selected = std::clamp(selected, 0, std::max(0, static_cast<int>(visible.size()) - 1));
            int first = (selected / 4) * 4;
            for (int row_index = 0;
                 row_index < 4 && first + row_index < static_cast<int>(visible.size());
                 ++row_index) {
                const auto &thread =
                    list.threads[visible[static_cast<std::size_t>(first + row_index)]];
                row((thread.active   ? "~ "
                     : thread.unread ? "* "
                                     : "  ") +
                        (thread.title.empty() ? "Untitled chat" : thread.title),
                    thread.active   ? "Okou is working"
                    : thread.unread ? "New reply"
                                    : "",
                    row_index * 29, first + row_index == selected, thread.active ? green : ink);
            }
            if (visible.empty())
                label(body_,
                      busy ? "Loading your chats..."
                           : "No chats here yet\nCreate a chat at app.okou.ai",
                      8, 16, 304, muted);
        } else {
            int count = static_cast<int>(list.projects.size()) + 2;
            selected = std::clamp(selected, 0, count - 1);
            int first = (selected / 4) * 4;
            for (int i = first; i < std::min(first + 4, count); ++i) {
                std::string name = i == 0   ? "All chats"
                                   : i == 1 ? "Without a project"
                                            : list.projects[static_cast<std::size_t>(i - 2)].title;
                row(name, "", (i - first) * 29, i == selected, ink);
            }
        }
        label(body_, status, 8, 115, 304, muted, 11);
        help = "Up/down   Enter open   Tab switch   Esc menu";
    } else {
        auto *history = lv_obj_create(body_);
        lv_obj_remove_style_all(history);
        lv_obj_set_pos(history, 5, 0);
        lv_obj_set_size(history, 310, 82);
        lv_obj_set_scroll_dir(history, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(history, LV_SCROLLBAR_MODE_OFF);
        int y = 0;
        std::size_t start = messages.size() > 30 ? messages.size() - 30 : 0;
        for (std::size_t i = start; i < messages.size(); ++i) {
            const auto &message = messages[i];
            label(history, message.role == "user" ? "You" : "Okou", 3, y, 299, muted, 17);
            y += 18;
            auto *text = label(history, message.text, 3, y, 299, ink);
            lv_obj_update_layout(text);
            y += lv_obj_get_height(text) + 8;
        }
        if (messages.empty())
            label(history, busy ? "Loading conversation..." : "Write to Okou below", 3, 8, 299,
                  muted);
        lv_obj_update_layout(history);
        lv_obj_scroll_to_y(history, std::max(y - 82 - history_scroll_, 0), LV_ANIM_OFF);
        auto *composer = lv_obj_create(body_);
        lv_obj_remove_style_all(composer);
        lv_obj_set_pos(composer, 5, 84);
        lv_obj_set_size(composer, 310, 39);
        lv_obj_set_style_bg_color(composer, lv_color_hex(0xffffff), 0);
        lv_obj_set_style_bg_opa(composer, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(composer, 5, 0);
        lv_obj_remove_flag(composer, LV_OBJ_FLAG_SCROLLABLE);
        std::string text = recording       ? "Recording... Ctrl+R to finish"
                           : draft.empty() ? "Message Okou..."
                                           : draft;
        auto *compose_text = label(composer, text, 6, 3, 298, draft.empty() ? muted : ink);
        lv_obj_update_layout(compose_text);
        if (lv_obj_get_height(compose_text) > 33)
            lv_obj_set_y(compose_text, 33 - lv_obj_get_height(compose_text));
        help = uncertain                ? "Send uncertain: refresh history; do not resend"
               : voice_pending && !busy ? "Voice retained: Ctrl+R retry   Esc discard"
               : submitting             ? "Sending... please wait"
               : busy                   ? status
                                        : "Enter send   Ctrl+R voice   Esc back";
    }
    auto *footer_text = label(footer_, help, 5, 3, 310, muted, 15);
    lv_obj_set_style_text_font(footer_text, &lv_font_montserrat_12, 0);
}
void UI::demo(const std::string &screen) {
    list.projects = {{"project_demo", "Product ideas"}, {"project_work", "Work"}};
    list.threads = {
        {"thread_1", "Plan a quieter morning", "project_demo", "agent_demo", true, false},
        {"thread_2", "研究下这个小机器", "project_demo", "agent_demo", false, true},
        {"thread_3", "A note for tomorrow", "", "agent_demo", false, false},
        {"thread_4", "Review the weekly update", "project_work", "agent_demo", false, false}};
    org_id = "Demo workspace";
    live = true;
    busy = false;
    submitting = recording = voice_pending = uncertain = voice_working = false;
    status = "Demo - no account or network";
    if (screen == "projects")
        page = Page::projects;
    else if (screen == "login") {
        page = Page::login;
        live = false;
        user_code = "DEMO-CODE";
        verification_uri = "https://example.invalid/activate";
        status = "Demo only - this is not a real sign-in code";
    } else if (screen == "chat") {
        select_thread(list.threads[1]);
        messages = {{"event_1", "user", "帮我整理今天要做的事", false},
                    {"event_2", "assistant", "当然。我们先从最重要的一件事开始。", true}};
        draft = "Let's start with the prototype";
    } else
        page = Page::threads;
    selected = 0;
}
} // namespace okou
