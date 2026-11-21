#include "okou/ui.hpp"
#include "emoji.hpp"
#include <algorithm>
#include <cstdio>
#include <filesystem>

namespace okou {
namespace {
constexpr std::uint32_t paper = 0xf8f5f3, ink = 0x292724, muted = 0x74706b;
constexpr std::uint32_t accent = 0xe4c6b8, green = 0x246948, border = 0xe4dfdb;
constexpr std::uint32_t state_blue = 0x0284c7;

lv_obj_t *shape(lv_obj_t *parent, int x, int y, int width, int height, int radius,
                std::uint32_t color) {
    auto *object = lv_obj_create(parent);
    lv_obj_remove_style_all(object);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(object, x, y);
    lv_obj_set_size(object, width, height);
    lv_obj_set_style_radius(object, radius, 0);
    lv_obj_set_style_bg_color(object, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
    return object;
}
void running_indicator(lv_obj_t *parent, int offset) {
    auto *ring = shape(parent, 291, 8, 13, 13, LV_RADIUS_CIRCLE, state_blue);
    lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ring, 1, 0);
    lv_obj_set_style_border_color(ring, lv_color_hex(state_blue), 0);
    shape(parent, 295, 12, 5, 5, LV_RADIUS_CIRCLE, state_blue);
    lv_anim_t animation;
    lv_anim_init(&animation);
    lv_anim_set_var(&animation, ring);
    lv_anim_set_exec_cb(&animation, [](void *object, int32_t opacity) {
        lv_obj_set_style_border_opa(static_cast<lv_obj_t *>(object), static_cast<lv_opa_t>(opacity),
                                    0);
    });
    lv_anim_set_values(&animation, 72, 235);
    lv_anim_set_duration(&animation, 1200);
    lv_anim_set_reverse_duration(&animation, 1200);
    lv_anim_set_delay(&animation, offset * 200);
    lv_anim_set_repeat_count(&animation, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&animation);
}
} // namespace
UI::UI(lv_display_t *display) : emoji_(std::make_unique<EmojiRenderer>()) {
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
            if (font_) {
                detail_font_ = lv_freetype_font_create(path, LV_FREETYPE_FONT_RENDER_MODE_BITMAP,
                                                       11, LV_FREETYPE_FONT_STYLE_NORMAL);
                break;
            }
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
    if (detail_font_)
        lv_freetype_font_delete(detail_font_);
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
lv_obj_t *UI::row(const std::string &title, const std::string &detail, int y, bool chosen,
                  std::uint32_t color, const char *icon, bool emoji_badge) {
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
    auto parts = split_emoji_title(title);
    const bool badge = emoji_badge && !parts.second.empty();
    if (icon) {
        auto *glyph = label(box, icon, 7, 6, 20, muted, 17);
        lv_obj_set_style_text_font(glyph, &lv_font_montserrat_16, 0);
    }
    int text_x = icon ? 33 : 7;
    if (badge && !parts.first.empty()) {
        auto *back = shape(box, 14, 12, 15, 15, LV_RADIUS_CIRCLE, chosen ? accent : paper);
        if (!emoji_->draw(box, parts.first, 15, 13, 12))
            lv_obj_delete(back);
        else
            parts.first.clear();
    } else if (emoji_->draw(box, parts.first, icon ? 30 : 7, 5, 18)) {
        text_x += 23;
        parts.first.clear();
    }
    label(box, parts.first.empty() ? parts.second : title, text_x, detail.empty() ? 5 : 0,
          280 - text_x, color, 17);
    if (!detail.empty()) {
        auto *subtitle = label(box, detail, text_x, 14, 280 - text_x, muted, 14);
        lv_obj_set_style_text_font(subtitle, detail_font_ ? detail_font_ : &lv_font_montserrat_12,
                                   0);
    }
    return box;
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
    retained_voice_text.clear();
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
    acknowledged_messages_.clear();
    history_scroll_ = 0;
    busy = recording = uncertain = submitting = voice_pending = voice_working = false;
}
void UI::complete_send(const std::string &thread_id, const std::string &text, bool was_uncertain,
                       const std::string &event_id) {
    if (current.id == thread_id) {
        uncertain = was_uncertain;
        if (!was_uncertain && !event_id.empty()) {
            Message message{event_id, "user", text, false};
            acknowledged_messages_.push_back(message);
            if (acknowledged_messages_.size() > 30)
                acknowledged_messages_.erase(acknowledged_messages_.begin());
            messages.push_back(std::move(message));
            history_scroll_ = 0;
        }
    }
    submitting = false;
}
void UI::update_history(std::vector<Message> history) {
    const auto visible = [&](const Message &message) {
        return std::any_of(history.begin(), history.end(),
                           [&](const Message &event) { return event.id == message.id; });
    };
    acknowledged_messages_.erase(
        std::remove_if(acknowledged_messages_.begin(), acknowledged_messages_.end(), visible),
        acknowledged_messages_.end());
    history.insert(history.end(), acknowledged_messages_.begin(), acknowledged_messages_.end());
    messages = std::move(history);
}
void UI::reconcile_send(const std::string &thread_id, bool pending) {
    if (current.id == thread_id)
        uncertain = pending;
}
void UI::select_thread(const Thread &thread) {
    current = thread;
    messages.clear();
    acknowledged_messages_.clear();
    history_scroll_ = 0;
    uncertain = false;
    page = Page::chat;
}
Action UI::handle(const Input &input) {
    if (input.key == Key::quit)
        return Action::quit;
    if (input.key == Key::workspace) {
        if (recording || voice_pending || submitting || busy)
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
        if (input.key == Key::escape && !busy && !recording && !voice_pending && !submitting) {
            page = Page::threads;
            selected = 0;
        } else if (input.key == Key::record && voice_pending && !busy && !submitting &&
                   !voice_working && !uncertain) {
            voice_working = true;
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
            auto previous = project_filter;
            project_filter = selected == 0 ? ""
                             : selected == 1
                                 ? "~"
                                 : list.projects.at(static_cast<std::size_t>(selected - 2)).id;
            page = Page::threads;
            selected = 0;
            if (project_filter != previous)
                return Action::filter_project;
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
bool UI::update_safe() const {
    return !busy && !recording && !voice_pending && !voice_working && !submitting && !uncertain &&
           draft.empty() && retained_voice_text.empty();
}
void UI::update_recording_clock() {
    if (!recording_clock_)
        return;
    const auto elapsed =
        recording_started == std::chrono::steady_clock::time_point{}
            ? 0L
            : static_cast<long>(std::chrono::duration_cast<std::chrono::seconds>(
                                    std::chrono::steady_clock::now() - recording_started)
                                    .count());
    if (elapsed == recording_seconds_)
        return;
    recording_seconds_ = elapsed;
    char duration[16];
    std::snprintf(duration, sizeof(duration), "%02ld:%02ld", elapsed / 60, elapsed % 60);
    lv_label_set_text(recording_clock_, duration);
}
void UI::render_recording() {
    if (recording_overlay_)
        return;

    constexpr std::uint32_t voice_background = 0x211c20;
    constexpr std::uint32_t voice_accent = 0xf2ac8e;
    recording_overlay_ = shape(root_, 0, 0, 320, 170, 0, voice_background);
    auto *panel = shape(recording_overlay_, -8, 0, 336, 170, 0, voice_background);
    recording_clock_ = label(panel, "00:00", 8, 30, 320, 0xffeee6, 26);
    lv_obj_set_style_text_align(recording_clock_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(recording_clock_, &lv_font_montserrat_16, 0);
    recording_seconds_ = -1;
    update_recording_clock();

    lv_anim_t animation;
    lv_anim_init(&animation);
    lv_anim_set_var(&animation, panel);
    lv_anim_set_exec_cb(
        &animation, [](void *obj, int32_t y) { lv_obj_set_y(static_cast<lv_obj_t *>(obj), y); });
    lv_anim_set_values(&animation, 24, 0);
    lv_anim_set_duration(&animation, 180);
    lv_anim_set_path_cb(&animation, lv_anim_path_ease_out);
    lv_anim_start(&animation);

    lv_anim_init(&animation);
    lv_anim_set_var(&animation, panel);
    lv_anim_set_exec_cb(
        &animation, [](void *obj, int32_t x) { lv_obj_set_x(static_cast<lv_obj_t *>(obj), x); });
    lv_anim_set_values(&animation, -8, -13);
    lv_anim_set_duration(&animation, 45);
    lv_anim_set_reverse_duration(&animation, 45);
    lv_anim_set_repeat_count(&animation, 2);
    lv_anim_start(&animation);

    constexpr int peak[] = {16, 30, 43, 53, 43, 30, 16};
    for (int i = 0; i < 7; ++i) {
        auto *bar = shape(panel, 129 + 12 * i, 101, 6, 9, LV_RADIUS_CIRCLE, voice_accent);
        lv_anim_init(&animation);
        lv_anim_set_var(&animation, bar);
        lv_anim_set_exec_cb(&animation, [](void *obj, int32_t height) {
            auto *bar = static_cast<lv_obj_t *>(obj);
            lv_obj_set_height(bar, height);
            lv_obj_set_y(bar, 106 - height / 2);
        });
        lv_anim_set_values(&animation, 9, peak[i]);
        lv_anim_set_duration(&animation, 230 + 35 * (i % 3));
        lv_anim_set_reverse_duration(&animation, 260);
        lv_anim_set_delay(&animation, 42 * i);
        lv_anim_set_repeat_count(&animation, LV_ANIM_REPEAT_INFINITE);
        lv_anim_start(&animation);
    }
}
void UI::render() {
    if (page == Page::chat && recording) {
        render_recording();
        return;
    }
    if (recording_overlay_) {
        lv_obj_delete(recording_overlay_);
        recording_overlay_ = recording_clock_ = nullptr;
    }
    lv_obj_clean(header_);
    lv_obj_clean(body_);
    lv_obj_clean(footer_);
    std::string title = "Okou";
    auto project_title = [&](const std::string &id) {
        const auto found = std::find_if(list.projects.begin(), list.projects.end(),
                                        [&](const auto &project) { return project.id == id; });
        return found != list.projects.end() ? found->title : std::string{};
    };
    if (page == Page::threads) {
        auto name = project_title(project_filter);
        title = project_filter.empty()  ? "Okou / chats"
                : project_filter == "~" ? "Without a project"
                : name.empty()          ? "Project chats"
                                        : name;
    } else if (page == Page::projects)
        title = "Okou / projects";
    else if (page == Page::chat)
        title = current.title.empty() ? "Chat" : current.title;
    else if (page == Page::settings)
        title = "Okou / workspace";
    auto heading = split_emoji_title(title);
    constexpr int heading_end = 251;
    if (emoji_->draw(header_, heading.first, 8, 3, 16))
        label(header_, heading.second, 29, 3, heading_end - 29, ink, 20);
    else
        label(header_, title, 8, 3, heading_end - 8, ink, 20);
    const auto power_color = battery.charging                                ? green
                             : battery.percent >= 0 && battery.percent <= 15 ? 0xa8342b
                                                                             : muted;
    if (battery.charging) {
        auto *bolt = label(header_, LV_SYMBOL_CHARGE, 252, 4, 12, power_color, 16);
        lv_obj_set_style_text_font(bolt, &lv_font_montserrat_12, 0);
    }
    const char *level = battery.percent >= 90   ? LV_SYMBOL_BATTERY_FULL
                        : battery.percent >= 65 ? LV_SYMBOL_BATTERY_3
                        : battery.percent >= 40 ? LV_SYMBOL_BATTERY_2
                        : battery.percent >= 15 ? LV_SYMBOL_BATTERY_1
                                                : LV_SYMBOL_BATTERY_EMPTY;
    auto *icon = label(header_, level, 266, 4, 17, power_color, 16);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_12, 0);
    auto percent = battery.percent >= 0 ? std::to_string(battery.percent) + "%" : "--%";
    auto *value = label(header_, percent, 283, 4, 32, power_color, 16);
    lv_obj_set_style_text_font(value, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_RIGHT, 0);
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
                const auto detail = project_filter.empty()
                                        ? split_emoji_title(project_title(thread.project_id)).second
                                        : std::string{};
                auto *box = row(thread.title.empty() ? "Untitled chat" : thread.title, detail,
                                row_index * 29, first + row_index == selected, ink);
                if (thread.active) {
                    running_indicator(box, row_index);
                } else if (thread.muted) {
                    auto *icon = label(box, LV_SYMBOL_BELL, 290, 6, 16, muted, 17);
                    lv_obj_set_style_text_font(icon, &lv_font_montserrat_12, 0);
                    static const lv_point_precise_t slash[] = {{0, 0}, {11, 11}};
                    auto *line = lv_line_create(box);
                    lv_line_set_points(line, slash, 2);
                    lv_obj_set_pos(line, 291, 8);
                    lv_obj_set_style_line_width(line, 2, 0);
                    lv_obj_set_style_line_color(line, lv_color_hex(muted), 0);
                } else if (thread.unread) {
                    shape(box, 294, 11, 7, 7, LV_RADIUS_CIRCLE, state_blue);
                } else if (thread.pinned) {
                    shape(box, 294, 7, 7, 7, 2, muted);
                    shape(box, 297, 12, 1, 7, 0, muted);
                }
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
                auto *box = row(name, "", (i - first) * 29, i == selected, ink,
                                i == 0 ? LV_SYMBOL_LIST : LV_SYMBOL_DIRECTORY, i >= 2);
                auto *arrow = label(box, LV_SYMBOL_RIGHT, 291, 6, 14, muted, 17);
                lv_obj_set_style_text_font(arrow, &lv_font_montserrat_12, 0);
            }
        }
        const bool request_failed = status.rfind("Could not", 0) == 0 ||
                                    status.rfind("Sign-in expired", 0) == 0 ||
                                    status.rfind("Access not granted", 0) == 0;
        help = request_failed ? status : "Up/down   Enter open   Tab switch   Esc menu";
    } else {
        auto *history = lv_obj_create(body_);
        lv_obj_remove_style_all(history);
        lv_obj_set_pos(history, 5, 0);
        lv_obj_set_size(history, 310, 122);
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
            label(history, busy ? "Loading conversation..." : "Hold Enter to start a voice message",
                  3, 8, 299, muted);
        lv_obj_update_layout(history);
        lv_obj_scroll_to_y(history, std::max(y - 122 - history_scroll_, 0), LV_ANIM_OFF);
        const bool voice_feedback = status == "No speech recognized" ||
                                    status == "Voice send failed" || status == "Message accepted" ||
                                    status == "Recording stopped" ||
                                    status == "Recording discarded";
        help = uncertain                ? "Send uncertain: refresh history; do not resend"
               : voice_pending && !busy ? "Voice retained: Ctrl+R retry   Esc discard"
               : submitting             ? "Sending... please wait"
               : recording || busy || voice_feedback ? status
                                                     : "Hold Enter to record";
    }
    if (!update_message.empty() && page != Page::chat)
        help = update_message;
    auto *footer_text = label(footer_, help, 5, 3, 310, muted, 15);
    lv_obj_set_style_text_font(footer_text, &lv_font_montserrat_12, 0);
}
void UI::demo(const std::string &screen) {
    list.projects = {{"project_demo", "💡 Product ideas"}, {"project_work", "🚀 工作台"}};
    list.threads = {{"thread_1", "👩🏽‍💻 Plan a quieter morning", "project_demo",
                     "agent_demo", true, false},
                    {"thread_2", "🚀 研究下这个小机器", "project_demo", "agent_demo", false, true},
                    {"thread_3", "A note for tomorrow", "", "agent_demo", false, false, true},
                    {"thread_4", "Review the weekly update", "project_work", "agent_demo", false,
                     false, false, true}};
    org_id = "Demo workspace";
    battery = {72, screen != "projects"};
    busy = false;
    submitting = recording = voice_pending = uncertain = voice_working = false;
    status = "Demo - no account or network";
    if (screen == "projects")
        page = Page::projects;
    else if (screen == "login") {
        page = Page::login;
        user_code = "DEMO-CODE";
        verification_uri = "https://example.invalid/activate";
        status = "Demo only - this is not a real sign-in code";
    } else if (screen == "chat" || screen == "recording") {
        select_thread(list.threads[1]);
        messages = {{"event_1", "user", "帮我整理今天要做的事", false},
                    {"event_2", "assistant", "当然。我们先从最重要的一件事开始。", true}};
        if (screen == "recording") {
            recording = true;
            recording_started = std::chrono::steady_clock::now();
        }
    } else
        page = Page::threads;
    if (screen == "update")
        update_message = "v0.2.3 Ctrl+Alt+U update";
    selected = 0;
}
} // namespace okou
