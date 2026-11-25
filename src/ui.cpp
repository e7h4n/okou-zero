#include "okou/ui.hpp"
#include "emoji.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>

namespace okou {
namespace {
constexpr std::uint32_t paper = 0x10191a, ink = 0xeef0d5, muted = 0x79a799;
constexpr std::uint32_t accent = 0x334940, green = 0x9ef1ce, border = 0x455f58, face = 0x192626,
                        gold = 0xf2bb79;

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
void draw_segment(lv_event_t *event, const lv_area_t &origin, int x1, int y1, int x2, int y2,
                  uint32_t color, int width = 1) {
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.p1.x = origin.x1 + x1;
    d.p1.y = origin.y1 + y1;
    d.p2.x = origin.x1 + x2;
    d.p2.y = origin.y1 + y2;
    d.color = lv_color_hex(color);
    d.width = width;
    lv_draw_line(lv_event_get_layer(event), &d);
}
// Four fixed-width seven-segment digits share the right inset of the waveform.
void draw_elapsed(lv_event_t *event, int seconds) {
    lv_area_t area;
    lv_obj_get_coords(lv_event_get_target_obj(event), &area);
    struct Segment {
        int x1, y1, x2, y2;
    };
    constexpr Segment strokes[] = {{3, 2, 12, 2},   {14, 4, 14, 10}, {14, 14, 14, 20},
                                   {3, 22, 12, 22}, {1, 14, 1, 20},  {1, 4, 1, 10},
                                   {3, 12, 12, 12}};
    constexpr unsigned masks[] = {0x3f, 0x06, 0x5b, 0x4f, 0x66, 0x6d, 0x7d, 0x07, 0x7f, 0x6f};
    const int digits[] = {seconds / 600 % 10, seconds / 60 % 10, seconds % 60 / 10, seconds % 10};
    constexpr int offsets[] = {0, 19, 48, 67};
    for (int i = 0; i < 4; ++i) {
        for (unsigned bit = 0; bit < std::size(strokes); ++bit) {
            if (!(masks[digits[i]] & (1u << bit)))
                continue;
            const auto &stroke = strokes[bit];
            draw_segment(event, area, offsets[i] + stroke.x1, stroke.y1, offsets[i] + stroke.x2,
                         stroke.y2, ink, 2);
        }
    }
    for (int y : {8, 16})
        draw_segment(event, area, 40, y, 42, y, gold, 2);
}
// A tesseract has 16 sign combinations; edges differ in exactly one coordinate.
void draw_tesseract(lv_event_t *event) {
    lv_area_t bounds;
    lv_obj_get_coords(lv_event_get_target_obj(event), &bounds);
    constexpr unsigned period_ms = 9600;
    const double angle = 6.283185307179586 * (lv_tick_get() % period_ms) / period_ms;
    const double cw = std::cos(angle), sw = std::sin(angle);
    const double cy = std::cos(angle + 0.42), sy = std::sin(angle + 0.42);
    struct Vertex {
        int x, y;
        double depth;
    };
    Vertex vertices[16];
    for (int i = 0; i < 16; ++i) {
        const int x = (i & 1) ? 1 : -1, y = (i & 2) ? 1 : -1, z = (i & 4) ? 1 : -1,
                  w = (i & 8) ? 1 : -1;
        // Rotate in the XW and YZ planes, then tilt the 3D projection toward the viewer.
        const double rx = cw * x - sw * w, rw = sw * x + cw * w;
        const double ry = cy * y - sy * z, rz = sy * y + cy * z;
        const double scale4 = 3.8 / (3.8 - rw);
        const double vx = 0.92 * rx + 0.39 * rz, vz = -0.39 * rx + 0.92 * rz;
        const double vy = 0.96 * ry - 0.28 * vz, depth = scale4 * (0.28 * ry + 0.96 * vz);
        const double scale = 22.0 * scale4 * 6.0 / (6.0 - depth);
        vertices[i] = {lv_area_get_width(&bounds) / 2 + static_cast<int>(std::lround(vx * scale)),
                       lv_area_get_height(&bounds) / 2 + static_cast<int>(std::lround(vy * scale)),
                       depth};
    }
    // Draw distant edges first so near edges remain legible at crossings.
    for (int layer = 0; layer < 3; ++layer) {
        for (int i = 0; i < 16; ++i) {
            for (int axis : {1, 2, 4, 8}) {
                if (i & axis)
                    continue;
                const auto &a = vertices[i], &b = vertices[i | axis];
                const double depth = (a.depth + b.depth) * 0.5;
                const int edge_layer = depth < -0.35 ? 0 : depth < 0.45 ? 1 : 2;
                if (edge_layer == layer)
                    draw_segment(event, bounds, a.x, a.y, b.x, b.y,
                                 layer == 0   ? border
                                 : layer == 1 ? muted
                                              : green);
            }
        }
    }
}

void animate_redraw(lv_obj_t *object, int frames_per_second) {
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, object);
    lv_anim_set_exec_cb(
        &anim, [](void *obj, int32_t) { lv_obj_invalidate(static_cast<lv_obj_t *>(obj)); });
    lv_anim_set_values(&anim, 0, frames_per_second);
    lv_anim_set_duration(&anim, 1000);
    lv_anim_set_repeat_count(&anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&anim);
}
lv_obj_t *folded_sheet(lv_obj_t *parent, int x, int y, int width, int height) {
    auto *sheet = shape(parent, x, y, width, height, 0, face);
    lv_obj_add_event_cb(
        sheet,
        [](lv_event_t *e) {
            lv_area_t a;
            lv_obj_get_coords(lv_event_get_target_obj(e), &a);
            const int w = lv_area_get_width(&a) - 1, h = lv_area_get_height(&a) - 1;
            const lv_point_t points[] = {{0, 11},     {11, 0},     {w - 6, 0}, {w, 6},
                                         {w, h - 12}, {w - 12, h}, {0, h},     {0, 11}};
            const auto cut = [&](lv_point_t corner, lv_point_t first, lv_point_t last) {
                lv_draw_triangle_dsc_t d;
                lv_draw_triangle_dsc_init(&d);
                const lv_point_t triangle[] = {corner, first, last};
                for (int i = 0; i < 3; ++i) {
                    d.p[i].x = a.x1 + triangle[i].x;
                    d.p[i].y = a.y1 + triangle[i].y;
                }
                d.color = lv_color_hex(paper);
                lv_draw_triangle(lv_event_get_layer(e), &d);
            };
            cut({0, 0}, points[0], points[1]);
            cut({w, 0}, points[2], points[3]);
            cut({w, h}, points[4], points[5]);
            for (std::size_t i = 1; i < std::size(points); ++i)
                draw_segment(e, a, points[i - 1].x, points[i - 1].y, points[i].x, points[i].y,
                             i == 1 ? green : muted);
        },
        LV_EVENT_DRAW_MAIN, nullptr);
    return sheet;
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
    const char *bold_paths[] = {"/usr/share/fonts/opentype/noto/NotoSansCJK-Bold.ttc",
                                "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"};
    for (const auto *path : paths)
        if (std::filesystem::exists(path)) {
            font_ = lv_freetype_font_create(path, LV_FREETYPE_FONT_RENDER_MODE_BITMAP, 16,
                                            LV_FREETYPE_FONT_STYLE_NORMAL);
            if (font_) {
                detail_font_ = lv_freetype_font_create(path, LV_FREETYPE_FONT_RENDER_MODE_BITMAP,
                                                       11, LV_FREETYPE_FONT_STYLE_NORMAL);
                for (const auto *bold_path : bold_paths) {
                    if (!std::filesystem::exists(bold_path))
                        continue;
                    markdown_fonts_.bold =
                        lv_freetype_font_create(bold_path, LV_FREETYPE_FONT_RENDER_MODE_BITMAP, 16,
                                                LV_FREETYPE_FONT_STYLE_NORMAL);
                    markdown_fonts_.heading =
                        lv_freetype_font_create(bold_path, LV_FREETYPE_FONT_RENDER_MODE_BITMAP, 21,
                                                LV_FREETYPE_FONT_STYLE_NORMAL);
                    break;
                }
                markdown_fonts_.italic = lv_freetype_font_create(
                    path, LV_FREETYPE_FONT_RENDER_MODE_BITMAP, 16, LV_FREETYPE_FONT_STYLE_ITALIC);
                const char *mono = "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf";
                if (std::filesystem::exists(mono)) {
                    markdown_fonts_.code =
                        lv_freetype_font_create(mono, LV_FREETYPE_FONT_RENDER_MODE_BITMAP, 12,
                                                LV_FREETYPE_FONT_STYLE_NORMAL);
                    if (markdown_fonts_.code)
                        markdown_fonts_.code->fallback = font_;
                }
                break;
            }
        }
    lv_obj_set_style_text_font(root_, font_ ? font_ : LV_FONT_DEFAULT, 0);
    lv_obj_set_style_text_color(root_, lv_color_hex(ink), 0);
    reading_line_ = lv_font_get_line_height(font_ ? font_ : LV_FONT_DEFAULT);
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
    if (volume_overlay_)
        lv_obj_delete(volume_overlay_);
    lv_obj_clean(root_);
    for (auto *face : {markdown_fonts_.code, markdown_fonts_.heading, markdown_fonts_.italic,
                       markdown_fonts_.bold})
        if (face)
            lv_freetype_font_delete(face);
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
    label(box, parts.first.empty() ? parts.second : title, text_x, detail.empty() ? 0 : -3,
          280 - text_x, color, detail.empty() ? 27 : 21);
    if (!detail.empty()) {
        auto *subtitle = label(box, detail, text_x, 14, 280 - text_x, muted, 14);
        lv_obj_set_style_text_font(subtitle, detail_font_ ? detail_font_ : &lv_font_montserrat_12,
                                   0);
    }
    return box;
}
void UI::reset_account() {
    messages.clear();
    current = {};
    draft.clear();
    user_id.clear();
    org_id.clear();
    user_code.clear();
    verification_uri.clear();
    selected = 0;
    acknowledged_messages_.clear();
    navigation_ = {};
    turn_direction_ = 0;
    busy = recording = uncertain = submitting = voice_pending = voice_working = false;
}
void UI::complete_send(const std::string &thread_id, const std::string &text, bool was_uncertain,
                       const std::string &event_id, const std::string &voice_file_id) {
    if (current.id == thread_id) {
        uncertain = was_uncertain;
        if (!was_uncertain && !event_id.empty() &&
            std::none_of(messages.begin(), messages.end(),
                         [&](const Message &m) { return m.id == event_id; })) {
            Message message{event_id, "user", text, false};
            if (!voice_file_id.empty())
                message.voice_clips.push_back(voice_file_id);
            acknowledged_messages_.push_back(message);
            if (acknowledged_messages_.size() > 30)
                acknowledged_messages_.erase(acknowledged_messages_.begin());
            messages.push_back(std::move(message));
        }
        if (!was_uncertain && !event_id.empty() && !messages.empty() &&
            messages.back().id == event_id) {
            navigation_ = {};
            turn_direction_ = 0;
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
    navigation_ = {};
    turn_direction_ = 0;
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
        if ((input.key == Key::text || input.key == Key::space) && draft.size() < 512)
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
        if (input.key == Key::space && !recording && !voice_pending && !submitting)
            return Action::playback;
        if (input.key == Key::escape && status == "Network unavailable" && !busy) {
            status.clear();
            return Action::none;
        }
        if (input.key == Key::escape && !busy && !recording && !voice_pending && !submitting) {
            page = Page::settings;
            selected = 0;
        } else if (input.key == Key::record && voice_pending && !busy && !submitting &&
                   !voice_working && !uncertain) {
            voice_working = true;
            return Action::voice;
        }
        const bool reading = !recording && (playback_status.empty() ||
                                            playback_status == "Could not play recording");
        if (!reading)
            return Action::none;
        if (turn_direction_)
            render();
        if (input.key == Key::up || input.key == Key::page_up)
            navigation_.scroll(input.key == Key::page_up ? -3 * reading_line_ : -reading_line_);
        else if (input.key == Key::down || input.key == Key::page_down)
            navigation_.scroll(input.key == Key::page_down ? 3 * reading_line_ : reading_line_);
        else if (input.key == Key::left || input.key == Key::right) {
            const int previous = navigation_.position();
            navigation_.jump(input.key == Key::right);
            turn_direction_ = navigation_.position() - previous;
        }
        return Action::none;
    }
    if (input.key == Key::escape) {
        page = Page::chat;
        return Action::none;
    }
    if (input.key == Key::up)
        selected = std::max(selected - 1, 0);
    if (input.key == Key::down)
        selected = std::min(selected + 1, 3);
    if (input.key == Key::enter && !busy) {
        if (selected == 0)
            return Action::switch_workspace;
        if (selected == 1)
            return Action::logout;
        if (selected == 2) {
            page = Page::chat;
            return Action::refresh;
        }
        return Action::quit;
    }
    return Action::none;
}
bool UI::update_safe() const {
    return !busy && !recording && !voice_pending && !voice_working && !submitting && !uncertain &&
           draft.empty();
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
    lv_obj_invalidate(recording_clock_);
}
void UI::show_volume(VolumeStatus volume) {
    if (volume_overlay_)
        lv_obj_delete(volume_overlay_);
    auto *top = lv_display_get_layer_top(lv_obj_get_display(root_));
    volume_overlay_ = shape(top, 96, 62, 131, 25, 5, ink);
    bool quiet = volume.muted || volume.percent == 0;
    const char *symbol = quiet ? LV_SYMBOL_MUTE : LV_SYMBOL_VOLUME_MAX;
    auto *icon = label(volume_overlay_, symbol, 6, 4, 17, paper, 16);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_12, 0);
    std::string caption = volume.percent < 0 ? "Audio unavailable"
                                             : (volume.muted ? "Muted  " : "Volume  ") +
                                                   std::to_string(volume.percent) + "%";
    auto *text = label(volume_overlay_, caption, 24, 2, 104, paper, 18);
    lv_obj_set_style_text_font(text, detail_font_ ? detail_font_ : &lv_font_montserrat_12, 0);
    volume_until_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
}
void UI::dismiss_expired_volume() {
    if (volume_overlay_ && std::chrono::steady_clock::now() >= volume_until_) {
        lv_obj_delete(volume_overlay_);
        volume_overlay_ = nullptr;
    }
}
void UI::render_voice_panel(bool capturing) {
    if (recording_overlay_)
        return;
    recording_overlay_ = shape(root_, 0, 22, 320, 148, 0, paper);
    const bool paused = !capturing && playback_status.rfind("Paused", 0) == 0;
    if (capturing) {
        render_voice_state(recording_overlay_, "REC", gold);
        recording_clock_ = shape(recording_overlay_, 219, 10, 84, 25, 0, paper);
        lv_obj_add_event_cb(
            recording_clock_,
            [](lv_event_t *e) {
                const auto *ui = static_cast<UI *>(lv_event_get_user_data(e));
                draw_elapsed(e, static_cast<int>(ui->recording_seconds_));
            },
            LV_EVENT_DRAW_MAIN, this);
        recording_seconds_ = -1;
        update_recording_clock();
        // The scale marks the time window of the rolling sample history.
        for (int i = 0; i <= 12; ++i) {
            const int height = i % 3 == 0 ? 5 : 2;
            shape(recording_overlay_, 18 + i * 282 / 12, 132 - height, 1, height, 0, border);
        }
    } else {
        auto *title =
            label(recording_overlay_, paused ? "Paused" : "Voice playback", 14, 4, 165, green, 30);
        if (markdown_fonts_.heading)
            lv_obj_set_style_text_font(title, markdown_fonts_.heading, 0);
        playback_clock_ = label(recording_overlay_, "00:00", 224, 8, 80, green, 24);
        lv_obj_set_style_text_font(playback_clock_, &lv_font_montserrat_16, 0);
        playback_seconds_ = -1;
    }
    auto *wave = shape(recording_overlay_, 18, capturing ? 48 : 37, 284, 72, 0, paper);
    lv_obj_add_event_cb(
        wave,
        [](lv_event_t *e) {
            auto *ui = static_cast<UI *>(lv_event_get_user_data(e));
            auto *obj = lv_event_get_target_obj(e);
            lv_area_t a;
            lv_obj_get_coords(obj, &a);
            if (ui->recording) {
                const auto peaks = ui->recording_waveform.snapshot();
                // Compress the display range so quiet speech is visible without changing the audio.
                const auto height = [](int amplitude) {
                    return static_cast<int>(std::lround(
                        32 * std::log1p(std::abs(amplitude) / 48.0) / std::log1p(32768.0 / 48.0)));
                };
                int prev_high = 0, prev_low = 0;
                for (std::size_t i = 0; i < peaks.size(); ++i) {
                    const int x = static_cast<int>(i) * 6;
                    const int high = height(peaks[i].high), low = height(peaks[i].low);
                    if (i) {
                        draw_segment(e, a, x - 6, 36 - prev_high / 2, x, 36 - high / 2, border);
                        draw_segment(e, a, x - 6, 36 + prev_low / 2, x, 36 + low / 2, border);
                        draw_segment(e, a, x - 6, 36 - prev_high, x, 36 - high, gold);
                        draw_segment(e, a, x - 6, 36 + prev_low, x, 36 + low, gold);
                    }
                    prev_high = high;
                    prev_low = low;
                }
                return;
            }
            double phase = ui->playback_status.rfind("Paused", 0) == 0 ? 0. : lv_tick_get() / 470.;
            for (int layer = 0; layer < 4; ++layer) {
                int prev = 0;
                for (int j = 0; j <= 46; ++j) {
                    int y =
                        10 + layer * 14 +
                        static_cast<int>((4 + 3 * layer) * std::sin(j * .54 - phase + layer * .8) *
                                         std::cos(j * .14) * std::sin(j * 3.14159 / 46));
                    if (j)
                        draw_segment(e, a, (j - 1) * 6, prev, j * 6, y, layer < 3 ? border : green);
                    prev = y;
                }
            }
        },
        LV_EVENT_DRAW_MAIN, this);
    if (!paused)
        animate_redraw(wave, 16);
    if (!capturing)
        label(recording_overlay_, paused ? "Space to resume" : "Space to pause", 14, 115, 285, ink,
              26);
}
void UI::update_playback_clock(unsigned seconds) {
    if (!playback_clock_ || playback_seconds_ == static_cast<long>(seconds))
        return;
    playback_seconds_ = seconds;
    char text[20]{};
    std::snprintf(text, sizeof(text), "%02u:%02u", seconds / 60, seconds % 60);
    lv_label_set_text(playback_clock_, text);
}

void UI::update_clock() {
    const auto now = std::time(nullptr);
    if (!clock_ || now / 60 == clock_minute_)
        return;
    std::tm local{};
    if (!localtime_r(&now, &local))
        return;
    char text[8]{};
    std::strftime(text, sizeof(text), "%H:%M", &local);
    lv_label_set_text(clock_, text);
    clock_minute_ = now / 60;
}

void UI::render_header() {
    lv_obj_clean(header_);
    clock_ = label(header_, "--:--", 8, 1, 68, ink, 18);
    lv_obj_set_style_text_font(clock_, &lv_font_montserrat_14, 0);
    clock_minute_ = -1;
    update_clock();
    shape(header_, 8, 20, 304, 1, 0, border);
    const auto color =
        battery.percent >= 0 && battery.percent <= 15 && !battery.charging ? gold : green;
    const std::string percent =
        battery.percent >= 0 ? std::to_string(battery.percent) + "%" : "--%";
    auto *value = label(header_, percent, 244, 1, 40, ink, 18);
    lv_obj_set_style_text_font(value, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_RIGHT, 0);
    shape(header_, 288, 5, 20, 10, 0, muted);
    shape(header_, 289, 6, 18, 8, 0, paper);
    if (battery.percent > 0)
        shape(header_, 291, 8, std::max(1, 14 * std::min(100, battery.percent) / 100), 4, 0, color);
    shape(header_, 308, 8, 3, 4, 0, muted);
    if (battery.charging) {
        auto *bolt = label(header_, LV_SYMBOL_CHARGE, 231, 3, 13, green, 15);
        lv_obj_set_style_text_font(bolt, &lv_font_montserrat_12, 0);
    }
}

void UI::render_voice_state(lv_obj_t *parent, const char *text, std::uint32_t color) {
    shape(parent, 18, 19, 6, 6, 3, color);
    auto *state = label(parent, text, 32, 15, 120, color, 18);
    lv_obj_set_style_text_font(state, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_letter_space(state, 2, 0);
}
void UI::render_sending(const char *state) {
    render_voice_state(body_, state, green);
    auto *model = shape(body_, 88, 30, 144, 114, 0, paper);
    lv_obj_add_event_cb(model, draw_tesseract, LV_EVENT_DRAW_MAIN, nullptr);
    animate_redraw(model, 24);
}
void UI::render_reader() {
    // Keep the original message positions to associate user events with the following reply.
    std::vector<std::size_t> replies;
    std::vector<std::string> ids;
    for (std::size_t i = 0; i < messages.size(); ++i) {
        if (messages[i].role == "user")
            continue;
        replies.push_back(i);
        ids.push_back(messages[i].id);
    }
    navigation_.layout(std::move(ids));
    const int pos = navigation_.position(), count = static_cast<int>(replies.size());
    const bool latest = pos == count - 1;
    const auto after = pos < 0 ? 0 : replies[pos] + 1;
    const std::size_t first = pos > 0 ? replies[pos - 1] + 1 : 0;
    const auto add_voice = [&](std::size_t start, std::size_t end) {
        for (auto i = start; i < end; ++i)
            visible_voice_clips.insert(visible_voice_clips.end(), messages[i].voice_clips.begin(),
                                       messages[i].voice_clips.end());
    };
    if (latest && after < messages.size())
        add_voice(after, messages.size());
    if (visible_voice_clips.empty())
        add_voice(first, after);
    if (!playback_status.empty() && playback_status != "Could not play recording") {
        render_voice_panel(false);
        return;
    }

    // Stay on the animated panel after acknowledgement until a reply or terminal event arrives.
    if (latest && !messages.empty() && messages.back().role == "user" &&
        !messages.back().terminal && playback_status.empty() &&
        (status.empty() || status == "Message accepted")) {
        render_sending("SENT");
        return;
    }

    constexpr int sheet_x = 7, sheet_width = 272;
    constexpr int sheet_right = sheet_x + sheet_width;
    constexpr int text_width = sheet_width - 22;
    constexpr int rail_x = sheet_right + 12;
    for (int i = 0; i < 2; ++i) {
        shape(body_, 20 + 5 * i, 10 - 4 * i, sheet_right - 20, 1, 0, border);
        shape(body_, sheet_right - 1 + 5 * i, 10 - 4 * i, 1, 99, 0, border);
    }
    auto *sheet = folded_sheet(body_, sheet_x, 14, sheet_width, 118);
    if (turn_direction_) {
        lv_anim_t turn;
        lv_anim_init(&turn);
        lv_anim_set_var(&turn, sheet);
        lv_anim_set_exec_cb(
            &turn, [](void *obj, int32_t y) { lv_obj_set_y(static_cast<lv_obj_t *>(obj), y); });
        lv_anim_set_values(&turn, 14 + turn_direction_ * 6, 14);
        lv_anim_set_duration(&turn, 180);
        lv_anim_set_path_cb(&turn, lv_anim_path_ease_out);
        lv_anim_start(&turn);
        turn_direction_ = 0;
    }
    if (pos >= 0) {
        const auto &reply = messages[replies[pos]];
        const bool dated = !reply.local_sent_at.empty();
        if (dated) {
            auto *stamp = label(sheet, reply.local_sent_at, 12, 2, text_width, muted, 16);
            lv_obj_set_style_text_font(stamp, &lv_font_montserrat_12, 0);
        }
        const int body_top = dated ? 21 : 7;
        auto *clip = shape(sheet, 12, body_top, text_width, 111 - body_top, 0, face);
        std::string title;
        auto *content = markdown_body(clip, reply.text, 0, 0, text_width, markdown_fonts_, &title);
        if (!title.empty()) {
            auto *heading = label(sheet, title, 12, dated ? 15 : 4, text_width, ink, 34);
            if (markdown_fonts_.heading)
                lv_obj_set_style_text_font(heading, markdown_fonts_.heading, 0);
            const int top = dated ? 44 : 39;
            lv_obj_set_y(clip, top);
            lv_obj_set_height(clip, std::min(reading_line_ * 3, 114 - top));
        }
        lv_obj_update_layout(content);
        navigation_.measure(lv_obj_get_height(content), lv_obj_get_height(clip));
        lv_obj_set_y(content, -navigation_.offset());
    } else {
        const auto empty = current.id.empty() ? status
                           : busy             ? "Loading conversation..."
                           : messages.empty() ? "Hold Enter to record"
                                              : "No reply";
        label(sheet, empty, 13, 30, text_width, ink);
    }
    // For deep history, pin the newest reply at the bottom and keep the selection in view.
    const int start = std::max(0, std::min(pos - 1, count - 4));
    const int visible = std::min(4, count);
    const auto dots = [&](std::size_t n, int y) {
        for (std::size_t j = 0; j < std::min<std::size_t>(n, 6); ++j)
            shape(body_, rail_x + 3 + static_cast<int>(j) * 3, y, 2, 2, 0, gold);
        if (n > 6)
            shape(body_, rail_x + 22, y, 3, 1, 0, gold);
    };
    for (int j = 0; j < visible; ++j) {
        const int k = j == visible - 1 ? count - 1 : start + j;
        const int y = 121 - (visible - 1 - j) * 26;
        if (j) {
            if (k == start + j) {
                shape(body_, rail_x, y - 21, 1, 17, 0, accent);
            } else {
                // A broken trace marks the jump to the pinned newest reply.
                shape(body_, rail_x, y - 21, 1, 6, 0, accent);
                shape(body_, rail_x, y - 11, 1, 7, 0, accent);
            }
        }
        dots(replies[k] - (k ? replies[k - 1] + 1 : 0), y - 13);
        if (k == pos) {
            // A hollow reticle connects the selected reply directly to the sheet edge.
            shape(body_, sheet_right, y, rail_x - sheet_right - 3, 1, 0, muted);
            shape(body_, rail_x - 3, y - 3, 7, 7, 1, green);
            shape(body_, rail_x - 2, y - 2, 5, 5, 0, paper);
            shape(body_, rail_x, y, 1, 1, 0, green);
            auto *distance = label(body_, std::to_string(count - 1 - pos), rail_x + 6, y - 8,
                                   320 - rail_x - 6, green, 17);
            lv_obj_set_style_text_font(distance, &lv_font_montserrat_12, 0);
        } else {
            shape(body_, rail_x - 2, y, 5, 1, 0, muted);
        }
    }
    dots(messages.size() - (replies.empty() ? 0 : replies.back() + 1), 133);
}

void UI::render() {
    visible_voice_clips.clear();
    render_header();
    if (page == Page::chat && recording && recording_clock_)
        return;
    if (recording_overlay_) {
        lv_obj_delete(recording_overlay_);
        recording_overlay_ = recording_clock_ = playback_clock_ = nullptr;
    }
    lv_obj_clean(body_);
    lv_obj_clean(footer_);
    const bool chat = page == Page::chat;
    lv_obj_set_pos(body_, 0, chat ? 22 : 25);
    lv_obj_set_height(body_, chat ? 148 : 126);
    lv_obj_set_width(footer_, chat ? 250 : 320);
    lv_obj_set_style_bg_opa(footer_, chat ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
    if (chat && recording) {
        render_voice_panel(true);
        return;
    }
    std::string help;
    if (page == Page::setup) {
        label(body_, "Connect your Okou OAuth app", 8, 4, 304, ink);
        label(body_, "Public client ID (not a secret)", 8, 28, 304, muted);
        auto *field =
            label(body_, draft.empty() ? "Type the client ID here" : draft, 8, 54, 304, ink, 38);
        lv_obj_set_style_bg_color(field, lv_color_hex(face), 0);
        lv_obj_set_style_bg_opa(field, LV_OPA_COVER, 0);
        label(body_, status, 8, 98, 304, muted, 22);
        help = "Enter save   Esc exit";
    } else if (page == Page::login) {
        label(body_,
              user_code.empty() ? "Your chat, on your Cardputer"
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
        const std::vector<std::string> menu{"Switch workspace", "Sign out", "Refresh chat",
                                            "Return to launcher"};
        for (int i = 0; i < 4; ++i)
            row(menu[static_cast<std::size_t>(i)], i == 0 ? org_id : "", i * 29, selected == i,
                ink);
        help = "Up/down select   Enter   Esc back";
    } else if (uncertain || voice_pending || submitting || status == "Network unavailable") {
        const bool retained = voice_pending && !busy;
        const bool offline = status == "Network unavailable";
        if (!uncertain && !retained && !offline) {
            render_sending("SENDING");
            return;
        }
        const char *title = uncertain  ? "Delivery unconfirmed"
                            : retained ? "Recording saved"
                                       : "Offline";
        const char *hint = uncertain  ? "Ctrl+L to refresh chat"
                           : retained ? "Ctrl+R to retry   Esc to discard"
                                      : "Ctrl+L to reconnect   Esc for chat";
        auto *panel = folded_sheet(body_, 10, 11, 295, 108);
        auto *heading = label(panel, title, 16, 14, 265, ink, 37);
        if (markdown_fonts_.heading)
            lv_obj_set_style_text_font(heading, markdown_fonts_.heading, 0);
        label(panel, hint, 16, 61, 265, green, 36);
        return;
    } else {
        render_reader();
        if (current.id.empty())
            help = "Ctrl+L retry   Ctrl+W settings";
        else if (!recording_overlay_ && !playback_status.empty())
            help = playback_status;
        else if (!status.empty() && status != "Message accepted" && !current.id.empty())
            help = status;
        if (help.empty() || recording_overlay_)
            return;
        lv_obj_set_style_bg_color(footer_, lv_color_hex(paper), 0);
    }
    if (!update_message.empty() && page != Page::chat)
        help = update_message;
    auto *footer_text = label(footer_, help, 6, 1, chat ? 241 : 308, chat ? gold : ink, 17);
    lv_obj_set_style_text_font(footer_text, detail_font_ ? detail_font_ : &lv_font_montserrat_12,
                               0);
}
void UI::demo(const std::string &screen) {
    org_id = "Demo workspace";
    battery = {72, true};
    busy = false;
    submitting = recording = voice_pending = uncertain = voice_working = false;
    status.clear();
    playback_status.clear();
    update_message.clear();
    if (screen == "login") {
        page = Page::login;
        user_code = "DEMO-CODE";
        verification_uri = "https://example.invalid/activate";
        status = "Demo sign-in code";
    } else {
        Thread demo_thread;
        demo_thread.id = "demo-thread";
        demo_thread.title = "Cardputer Zero";
        select_thread(demo_thread);
        messages = {
            {"event_1", "user", "Help me plan my day", false},
            {"event_2",
             "assistant",
             "## Today’s plan\n\n1. **Confirm the time** with the team.\n2. Read `README.md`.",
             true,
             {},
             "2026-11-24 10:42"}};
        if (screen == "waiting")
            messages.push_back({"waiting", "user", "See you later", false});
        if (screen == "sending")
            busy = submitting = voice_pending = true;
        if (screen == "uncertain")
            uncertain = true;
        if (screen == "offline")
            status = "Network unavailable";
        if (screen == "history") {
            messages = {
                {"u1", "user", "Trip itinerary", false, {"demo-clip"}},
                {"a1", "assistant", "## Before leaving\n\nCheck the route and weather.", false},
                {"a2", "assistant", "The lake is our first stop.", false},
                {"u2", "user", "One more thing", false},
                {"u3", "user", "Confirm the time", false},
                {"a3", "assistant", "## Rest stop\n\nCharge the camera.", false},
                {"a4", "assistant", "## Next stop\n\nFollow the path east.", false},
                {"a5", "assistant", "## Meet at dusk\n\nWait at the station.", false},
                {"u4", "user", "Packing list", false},
                {"a6",
                 "assistant",
                 "## "
                 "Trip checklist\n\nPack water and a jacket.\nSave an offline map.\nCheck the "
                 "camera battery.\nConfirm your train ticket.\nMessage your team.\nPick up your "
                 "keys.",
                 false,
                 {},
                 "2026-11-24 11:08"}};
        }
        if (screen == "playback" || screen == "paused")
            playback_status =
                screen == "paused" ? "Paused   Space resume" : "Playing   Space pause";
        if (screen == "recording") {
            recording = true;
            recording_started = std::chrono::steady_clock::now();
        }
    }
    if (screen == "update") {
        page = Page::settings;
        update_message = "v0.2.3 Ctrl+Alt+U update";
    }
    selected = 0;
}
} // namespace okou
