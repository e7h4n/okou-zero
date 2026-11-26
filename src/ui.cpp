#include "okou/ui.hpp"
#include "orbit.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <stdexcept>

namespace okou {
namespace {
constexpr std::uint32_t paper = 0x111827, ink = 0xf4f4d9, muted = 0xa7b9be;
constexpr std::uint32_t accent = 0x334456, green = 0xdafa8c, border = 0x536677, face = 0xf0f0db,
                        gold = 0xf0bd80, on_sheet = 0x263733, sheet_detail = 0x4c6259;
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
constexpr int fold_size = 11;
lv_obj_t *folded_sheet(lv_obj_t *parent, int x, int y, int width, int height) {
    auto *sheet = shape(parent, x, y, width, height, 0, face);
    // The fold sits outside the text column and remains still while reading.
    lv_obj_add_event_cb(
        sheet,
        [](lv_event_t *e) {
            lv_area_t a;
            lv_obj_get_coords(lv_event_get_target_obj(e), &a);
            lv_draw_triangle_dsc_t d;
            lv_draw_triangle_dsc_init(&d);
            d.p[0] = {lv_value_precise_t(a.x2 - fold_size), lv_value_precise_t(a.y1)};
            d.p[1] = {lv_value_precise_t(a.x2), lv_value_precise_t(a.y1)};
            d.p[2] = {lv_value_precise_t(a.x2), lv_value_precise_t(a.y1 + fold_size)};
            d.color = lv_color_hex(paper);
            lv_draw_triangle(lv_event_get_layer(e), &d);
            d.p[1] = {lv_value_precise_t(a.x2 - fold_size), lv_value_precise_t(a.y1 + fold_size)};
            d.color = lv_color_hex(0xbfc8b3);
            lv_draw_triangle(lv_event_get_layer(e), &d);
        },
        LV_EVENT_DRAW_MAIN, nullptr);
    lv_obj_set_style_text_color(sheet, lv_color_hex(on_sheet), 0);
    return sheet;
}
} // namespace
UI::UI(lv_display_t *display) {
    root_ = lv_display_get_screen_active(display);
    lv_obj_remove_style_all(root_);
    lv_obj_set_style_bg_color(root_, lv_color_hex(paper), 0);
    lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, 0);
    lv_obj_set_size(root_, 320, 170);
    orbit_ = std::make_unique<PocketOrbit>(root_, recording_waveform, playback);
    const auto load = [this](const std::filesystem::path &path, int size,
                             lv_freetype_font_style_t style = LV_FREETYPE_FONT_STYLE_NORMAL) {
        if (!std::filesystem::is_regular_file(path))
            return static_cast<lv_font_t *>(nullptr);
        auto *f =
            lv_freetype_font_create(path.c_str(), LV_FREETYPE_FONT_RENDER_MODE_BITMAP, size, style);
        if (f)
            loaded_fonts_.push_back(f);
        return f;
    };
    const std::filesystem::path noto = "/usr/share/fonts/opentype/noto";
    const std::filesystem::path dejavu = "/usr/share/fonts/truetype/dejavu";
    const auto system_face = [&](int size, bool bold = false) {
        auto *f = load(noto / (bold ? "NotoSansCJK-Bold.ttc" : "NotoSansCJK-Regular.ttc"), size);
        return f ? f : load(dejavu / (bold ? "DejaVuSans-Bold.ttf" : "DejaVuSans.ttf"), size);
    };
    const auto branded = [&](const char *file, int size, lv_font_t *fallback,
                             lv_freetype_font_style_t style = LV_FREETYPE_FONT_STYLE_NORMAL) {
        auto *f = load(orbit_->directory / file, size, style);
        if (f)
            f->fallback = fallback ? fallback : LV_FONT_DEFAULT;
        return f ? f : fallback;
    };
    auto *body_cjk = system_face(14);
    auto *bold_cjk = system_face(14, true);
    auto *title_cjk = system_face(22, true);
    font_ = branded("Geist-Medium.ttf", 14, body_cjk);
    detail_font_ = branded("Geist-Medium.ttf", 11, system_face(11));
    markdown_fonts_.bold = branded("Geist-Bold.ttf", 14, bold_cjk);
    markdown_fonts_.heading = branded("Geist-Bold.ttf", 22, title_cjk);
    markdown_fonts_.italic =
        branded("Geist-Medium.ttf", 14, body_cjk, LV_FREETYPE_FONT_STYLE_ITALIC);
    markdown_fonts_.code = load(dejavu / "DejaVuSansMono.ttf", 12);
    if (markdown_fonts_.code)
        markdown_fonts_.code->fallback = font_ ? font_ : LV_FONT_DEFAULT;
    pixel_font_ = branded("Silkscreen-Regular.ttf", 10, detail_font_);
    greeting_font_ = branded("Silkscreen-Regular.ttf", 22, title_cjk);
    display_font_ = branded("Geist-Bold.ttf", 27, title_cjk);
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
    lv_obj_set_size(header_, 320, 17);
    lv_obj_set_pos(body_, 0, 17);
    lv_obj_set_size(body_, 320, 127);
    lv_obj_set_pos(footer_, 0, 144);
    lv_obj_set_size(footer_, 320, 26);
}
UI::~UI() {
    if (volume_overlay_)
        lv_obj_delete(volume_overlay_);
    orbit_.reset();
    lv_obj_clean(root_);
    for (auto it = loaded_fonts_.rbegin(); it != loaded_fonts_.rend(); ++it)
        lv_freetype_font_delete(*it);
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
lv_obj_t *UI::type(lv_obj_t *parent, const std::string &text, int x, int y, int width,
                   std::uint32_t color, const lv_font_t *font, int height) {
    auto *object = label(parent, text, x, y, width, color, height);
    if (font)
        lv_obj_set_style_text_font(object, font, 0);
    return object;
}
void UI::footer_hint(const std::string &action, const std::string &secondary) {
    shape(footer_, 8, 0, 304, 1, 0, border);
    type(footer_, action, 10, 5, secondary.empty() ? 300 : 212, ink, detail_font_, 20);
    if (!secondary.empty()) {
        auto *end = type(footer_, secondary, 213, 5, 97, muted, detail_font_, 20);
        lv_obj_set_style_text_align(end, LV_TEXT_ALIGN_RIGHT, 0);
    }
}
void UI::settings_row(const std::string &title, const std::string &detail, int y, bool chosen) {
    auto *box = lv_obj_create(body_);
    lv_obj_remove_style_all(box);
    lv_obj_set_pos(box, 5, y);
    lv_obj_set_size(box, 266, 27);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    if (chosen) {
        lv_obj_set_style_bg_color(box, lv_color_hex(green), 0);
        lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(box, 2, 0);
    }
    label(box, title, 7, detail.empty() ? 0 : -3, 250, chosen ? paper : ink,
          detail.empty() ? 27 : 21);
    if (!detail.empty()) {
        auto *subtitle = label(box, detail, 7, 14, 250, chosen ? on_sheet : muted, 14);
        lv_obj_set_style_text_font(subtitle, detail_font_ ? detail_font_ : &lv_font_montserrat_12,
                                   0);
    }
}
void UI::reset_account() {
    messages.clear();
    thread_id.clear();
    pending_reply_to_.clear();
    client_id_input.clear();
    org_id.clear();
    user_code.clear();
    verification_uri.clear();
    selected = 0;
    acknowledged_messages_.clear();
    navigation_ = {};
    turn_direction_ = 0;
    busy = recording = uncertain = submitting = voice_pending = voice_working = false;
}
void UI::complete_send(const std::string &id, const SendResult &result,
                       const std::string &file_id) {
    if (thread_id == id) {
        uncertain = result.uncertain;
        if (!uncertain) {
            pending_reply_to_ = result.client_event_id;
            if (std::none_of(messages.begin(), messages.end(),
                             [&](const Message &m) { return m.id == result.client_event_id; })) {
                Message message{
                    result.client_event_id, "user", "[Voice message]", false, {file_id}};
                acknowledged_messages_.push_back(message);
                if (acknowledged_messages_.size() > 30)
                    acknowledged_messages_.erase(acknowledged_messages_.begin());
                messages.push_back(std::move(message));
            }
            if (messages.back().id == result.client_event_id) {
                navigation_ = {};
                turn_direction_ = 0;
            }
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
bool UI::take_local_reply() {
    if (pending_reply_to_.empty())
        return false;
    auto user = std::find_if(messages.begin(), messages.end(), [&](const Message &message) {
        return message.id == pending_reply_to_;
    });
    if (user == messages.end() || user->run_id.empty())
        return false;
    for (auto it = std::next(user); it != messages.end(); ++it) {
        if (it->role == "assistant" && it->run_id == user->run_id) {
            pending_reply_to_.clear();
            return true;
        }
    }
    if (user->terminal)
        pending_reply_to_.clear();
    return false;
}
void UI::reconcile_send(const std::string &id, bool pending) {
    if (thread_id == id)
        uncertain = pending;
}
void UI::open_thread(const std::string &id) {
    pending_reply_to_.clear();
    thread_id = id;
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
        if ((input.key == Key::text || input.key == Key::space) && client_id_input.size() < 512)
            client_id_input += input.text;
        if (input.key == Key::backspace)
            erase_utf8(client_id_input);
        if (input.key == Key::enter && !client_id_input.empty() && !busy)
            return Action::configure;
        if (input.key == Key::escape)
            return Action::quit;
        return Action::none;
    }
    if (page == Page::login) {
        if (input.key == Key::space && user_code.empty() && !busy)
            orbit_->nudge();
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
        } else if (input.key == Key::retry_voice && voice_pending && !busy && !submitting &&
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
           client_id_input.empty();
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
    char text[24]{};
    std::snprintf(text, sizeof(text), "%02ld:%02ld", elapsed / 60, elapsed % 60);
    lv_label_set_text(recording_clock_, text);
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
    const bool paused = !capturing && playback_status.rfind("Paused", 0) == 0;
    recording_overlay_ = shape(body_, 0, 0, 320, 127, 0, paper);
    orbit_->sky(recording_overlay_);
    shape(recording_overlay_, 12, 16, 5, 5, 1, green);
    type(recording_overlay_,
         capturing ? "REC"
         : paused  ? "PAUSE"
                   : "PLAY",
         21, 10, 120, green, pixel_font_, 18);
    type(recording_overlay_,
         capturing ? "Listening."
         : paused  ? "Paused."
                   : "Playing.",
         10, 28, 171, ink, display_font_, 38);
    if (capturing) {
        recording_clock_ =
            type(recording_overlay_, "00:00", 12, 66, 143, ink, markdown_fonts_.bold, 22);
        recording_seconds_ = -1;
        update_recording_clock();
    } else {
        playback_clock_ =
            type(recording_overlay_, "00:00", 12, 66, 155, ink, markdown_fonts_.bold, 22);
        playback_seconds_ = playback_duration_ = -1;
        update_playback_clock();
    }
    auto *wave = lv_obj_create(recording_overlay_);
    lv_obj_remove_style_all(wave);
    lv_obj_set_pos(wave, 12, 85);
    lv_obj_set_size(wave, 145, 36);
    lv_obj_add_event_cb(
        wave,
        [](lv_event_t *e) {
            const auto *ui = static_cast<UI *>(lv_event_get_user_data(e));
            lv_area_t a;
            lv_obj_get_coords(lv_event_get_target_obj(e), &a);
            const auto peaks =
                (ui->recording ? ui->recording_waveform : ui->playback.waveform).snapshot();
            const auto height = [](int v) {
                return std::lround(13 * std::log1p(std::abs(v) / 48.) / std::log1p(32768. / 48.));
            };
            draw_segment(e, a, 0, 15, 143, 15, border);
            for (std::size_t i = 0; i < peaks.size(); ++i) {
                const int x = static_cast<int>(i) * 3;
                draw_segment(e, a, x, 15 - height(peaks[i].high), x, 15 + height(peaks[i].low),
                             green);
            }
            if (ui->recording) {
                for (int x = 0; x <= 144; x += 36)
                    draw_segment(e, a, x, 31, x, 33, border);
            } else {
                const auto duration = ui->playback.duration_ms.load();
                const int x = duration
                                  ? std::min<uint64_t>(
                                        142, 142ULL * ui->playback.elapsed_ms.load() / duration)
                                  : 0;
                draw_segment(e, a, 0, 31, 142, 31, border);
                draw_segment(e, a, 0, 31, x, 31, green);
                draw_segment(e, a, x, 28, x, 33, green);
            }
        },
        LV_EVENT_DRAW_MAIN, this);
    if (!paused)
        animate_redraw(wave, 20);
    orbit_->show(capturing ? OrbitScene::record : paused ? OrbitScene::pause : OrbitScene::play);
    footer_hint(capturing ? "Release Enter  Send"
                : paused  ? "Space  Resume"
                          : "Space  Pause",
                capturing ? "Esc  Discard" : "Voice note");
}
void UI::update_playback_clock() {
    if (!playback_clock_)
        return;
    const auto seconds = playback.elapsed_ms.load() / 1000;
    const auto duration = playback.duration_ms.load() / 1000;
    // A downloaded clip can become available before the first elapsed second.
    if (seconds == playback_seconds_ && duration == playback_duration_)
        return;
    playback_seconds_ = seconds;
    playback_duration_ = duration;
    char text[36]{};
    if (duration)
        std::snprintf(text, sizeof(text), "%02u:%02u / %02u:%02u", seconds / 60, seconds % 60,
                      duration / 60, duration % 60);
    else
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
    clock_ = type(header_, "--:--", 9, 0, 55, muted, detail_font_, 15);
    clock_minute_ = -1;
    update_clock();
    const char *section =
        page == Page::setup      ? "DEVICE SETUP"
        : page == Page::settings ? "SETTINGS"
        : page == Page::login    ? (user_code.empty() ? "OKOU ZERO" : "DEVICE SIGN-IN")
        : recording              ? "MIC INPUT"
        : playback_status.rfind("Playing", 0) == 0 || playback_status.rfind("Paused", 0) == 0
            ? "VOICE NOTE"
            : "CONVERSATION";
    auto *name = type(header_, section, 63, 0, 194, ink, pixel_font_, 15);
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
    shape(header_, 8, 16, 304, 1, 0, border);
    auto *value =
        type(header_, battery.percent >= 0 ? std::to_string(battery.percent) + "%" : "--%", 245, 0,
             43, muted, detail_font_, 15);
    lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_RIGHT, 0);
    const auto color =
        battery.percent >= 0 && battery.percent <= 15 && !battery.charging ? gold : green;
    shape(header_, 293, 4, 16, 8, 1, muted);
    shape(header_, 294, 5, 14, 6, 0, paper);
    if (battery.percent > 0)
        shape(header_, 295, 6, std::max(1, 12 * std::min(100, battery.percent) / 100), 4, 0, color);
    shape(header_, 309, 6, 2, 4, 0, muted);
    if (battery.charging)
        type(header_, LV_SYMBOL_CHARGE, 237, 0, 12, green, &lv_font_montserrat_12, 15);
}
void UI::render_sending(bool accepted) {
    orbit_->sky(body_);
    type(body_, accepted ? "AWAITING REPLY" : "RELEASED", 12, 12, 180, muted, pixel_font_, 18);
    type(body_, accepted ? "Thinking." : "Sending.", 10, 36, 170, ink, display_font_, 38);
    type(body_, accepted ? "Okou has your message." : "Waiting for receipt", 12, 80, 174, ink,
         detail_font_, 22);
    orbit_->show(accepted ? OrbitScene::wait : OrbitScene::send);
    footer_hint(accepted ? "Message accepted" : "Submitting recording");
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
        render_sending(true);
        return;
    }

    if (pos < 0) {
        orbit_->sky(body_);
        const bool opening = busy;
        const char *title = opening             ? "Opening chat."
                            : thread_id.empty() ? "Chat unavailable."
                            : messages.empty()  ? "Ready when\nyou are."
                                                : "No reply yet.";
        type(body_, opening ? "WORKSPACE" : "CONVERSATION", 12, 12, 175, muted, pixel_font_, 18);
        type(body_, title, 10, 33, 176, ink, markdown_fonts_.heading, 70);
        orbit_->show(opening             ? OrbitScene::wait
                     : thread_id.empty() ? OrbitScene::marker
                                         : OrbitScene::ready);
        return;
    }
    constexpr int sheet_x = 9, sheet_y = 7, sheet_width = 272, sheet_height = 111;
    constexpr int shadow_offset = 3;
    constexpr int sheet_right = sheet_x + sheet_width;
    constexpr int text_width = sheet_width - 22;
    constexpr int rail_x = 293;
    auto *page_layer = lv_obj_create(body_);
    lv_obj_remove_style_all(page_layer);
    lv_obj_remove_flag(page_layer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(page_layer, sheet_x, sheet_y);
    lv_obj_set_size(page_layer, sheet_width + shadow_offset, sheet_height + shadow_offset);
    shape(page_layer, shadow_offset, fold_size, sheet_width,
          sheet_height + shadow_offset - fold_size, 0, accent);
    auto *sheet = folded_sheet(page_layer, 0, 0, sheet_width, sheet_height);
    if (turn_direction_) {
        lv_anim_t turn;
        lv_anim_init(&turn);
        lv_anim_set_var(&turn, page_layer);
        lv_anim_set_exec_cb(
            &turn, [](void *obj, int32_t y) { lv_obj_set_y(static_cast<lv_obj_t *>(obj), y); });
        lv_anim_set_values(&turn, sheet_y + turn_direction_ * 5, sheet_y);
        lv_anim_set_duration(&turn, 180);
        lv_anim_set_path_cb(&turn, lv_anim_path_ease_out);
        lv_anim_start(&turn);
        turn_direction_ = 0;
    }
    {
        const auto &reply = messages[replies[pos]];
        const bool dated = !reply.local_sent_at.empty();
        if (dated) {
            auto *stamp = label(sheet, reply.local_sent_at, 12, 2, text_width, sheet_detail, 16);
            lv_obj_set_style_text_font(stamp, detail_font_ ? detail_font_ : &lv_font_montserrat_12,
                                       0);
        }
        const int body_top = dated ? 21 : 7;
        auto *clip = shape(sheet, 12, body_top, text_width, 105 - body_top, 0, face);
        lv_obj_set_style_text_color(clip, lv_color_hex(on_sheet), 0);
        std::string title;
        auto *content = markdown_body(clip, reply.text, text_width, markdown_fonts_, title);
        if (!title.empty()) {
            auto *heading = label(sheet, title, 12, dated ? 15 : 4, text_width, on_sheet, 34);
            if (markdown_fonts_.heading)
                lv_obj_set_style_text_font(heading, markdown_fonts_.heading, 0);
            const int top = dated ? 44 : 39;
            lv_obj_set_y(clip, top);
            lv_obj_set_height(clip, std::min(reading_line_ * 3, 105 - top));
        }
        lv_obj_update_layout(content);
        navigation_.measure(lv_obj_get_height(content), lv_obj_get_height(clip));
        lv_obj_set_y(content, -navigation_.offset());
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
        const int y = 103 - (visible - 1 - j) * 23;
        if (j) {
            if (k == start + j) {
                shape(body_, rail_x, y - 18, 1, 17, 0, accent);
            } else {
                // A broken trace marks the jump to the pinned newest reply.
                shape(body_, rail_x, y - 18, 1, 6, 0, accent);
                shape(body_, rail_x, y - 11, 1, 7, 0, accent);
            }
        }
        dots(replies[k] - (k ? replies[k - 1] + 1 : 0), y - 10);
        if (k == pos) {
            shape(body_, sheet_right, y, rail_x - sheet_right - 5, 1, 0, muted);
            orbit_->show(OrbitScene::bookmark, 17 + y);
            auto *distance = label(body_, std::to_string(count - 1 - pos), rail_x + 9, y - 8,
                                   320 - rail_x - 9, green, 17);
            lv_obj_set_style_text_font(distance, &lv_font_montserrat_12, 0);
        } else {
            shape(body_, rail_x - 2, y, 5, 1, 0, muted);
        }
    }
    dots(messages.size() - (replies.empty() ? 0 : replies.back() + 1), 119);
}

void UI::render() {
    visible_voice_clips.clear();
    render_header();
    if (page == Page::chat && recording && recording_clock_)
        return;
    recording_overlay_ = recording_clock_ = playback_clock_ = nullptr;
    lv_obj_clean(body_);
    lv_obj_clean(footer_);
    if (page == Page::chat && recording) {
        render_voice_panel(true);
        return;
    }
    if (page == Page::setup) {
        type(body_, "Connect your Okou app", 12, 6, 252, ink, markdown_fonts_.heading, 32);
        label(body_, "Public client ID", 13, 43, 250, muted, 22);
        auto *field =
            label(body_, client_id_input.empty() ? "Type the client ID here" : client_id_input, 12,
                  68, 258, ink, 25);
        lv_obj_set_style_bg_color(field, lv_color_hex(accent), 0);
        lv_obj_set_style_bg_opa(field, LV_OPA_COVER, 0);
        type(body_, status, 13, 100, 297, muted, detail_font_, 24);
        orbit_->show(OrbitScene::marker, 51);
        footer_hint(update_message.empty() ? "Enter  Save" : update_message, "Esc  Exit");
    } else if (page == Page::login) {
        const bool expired =
            status.rfind("Code expired", 0) == 0 || status.rfind("Sign-in expired", 0) == 0;
        if (user_code.empty() && busy) {
            orbit_->sky(body_);
            type(body_, "DEVICE SIGN-IN", 12, 12, 174, muted, pixel_font_, 18);
            type(body_, "Getting a code.", 10, 38, 180, ink, markdown_fonts_.heading, 60);
            orbit_->show(OrbitScene::wait);
        } else if (user_code.empty() && expired) {
            auto *sheet = folded_sheet(body_, 9, 12, 247, 99);
            type(sheet, "Sign in again", 11, 12, 224, on_sheet, markdown_fonts_.heading, 39);
            lv_label_set_long_mode(type(sheet, status, 12, 53, 215, sheet_detail, detail_font_, 40),
                                   LV_LABEL_LONG_MODE_WRAP);
            orbit_->show(OrbitScene::marker, 78);
        } else if (user_code.empty()) {
            orbit_->sky(body_);
            type(body_, "Hello,", 10, 22, 155, ink, greeting_font_, 32);
            type(body_, "traveler.", 10, 51, 159, green, greeting_font_, 32);
            if (!status.empty() && status != "Press Enter to sign in" &&
                status != "Sign in to continue")
                type(body_, status, 12, 90, 216, muted, detail_font_, 36);
            orbit_->show(OrbitScene::welcome);
        } else {
            type(body_, "Approve in browser", 12, 7, 230, ink, markdown_fonts_.bold, 24);
            type(body_, user_code, 11, 32, 232, green, display_font_, 38);
            lv_label_set_long_mode(
                type(body_, verification_uri, 12, 74, 299, ink, detail_font_, 33),
                LV_LABEL_LONG_MODE_WRAP);
            type(body_, status, 12, 107, 300, muted, detail_font_, 20);
            orbit_->show(OrbitScene::pairing);
        }
        footer_hint(!update_message.empty()     ? update_message
                    : user_code.empty() && busy ? "Requesting device code"
                    : expired                   ? "Enter  Try again"
                    : user_code.empty()         ? "Enter  Sign in    Space  Orbit"
                                                : "Approve on your other device",
                    "Esc  Exit");
    } else if (page == Page::settings) {
        const std::vector<std::string> menu{"Switch workspace", "Sign out", "Refresh chat",
                                            "Return to launcher"};
        for (int i = 0; i < 4; ++i)
            settings_row(menu[i], i == 0 ? org_id : "", 4 + i * 29, selected == i);
        orbit_->show(OrbitScene::marker, 34 + selected * 29);
        footer_hint(update_message.empty() ? "Up/down  Select    Enter  Open" : update_message,
                    "Esc  Back");
    } else if (uncertain || voice_pending || submitting || status == "Network unavailable") {
        const bool retained = voice_pending && !busy;
        const bool offline = status == "Network unavailable";
        if (!uncertain && !retained && !offline) {
            render_sending(false);
            return;
        }
        const char *title = uncertain  ? "Delivery unconfirmed"
                            : retained ? "Recording saved"
                                       : "Offline";
        const char *hint = uncertain  ? "Refresh chat to check delivery."
                           : retained ? "Your recording is still on this device."
                                      : "Network unavailable.";
        auto *panel = folded_sheet(body_, 9, 12, 247, 99);
        lv_label_set_long_mode(
            type(panel, title, 11, 10, 225, on_sheet, markdown_fonts_.heading, 54),
            LV_LABEL_LONG_MODE_WRAP);
        type(panel, hint, 12, 65, 220, sheet_detail, detail_font_, 28);
        orbit_->show(OrbitScene::marker, 78);
        footer_hint(uncertain  ? "Ctrl+L  Refresh chat"
                    : retained ? "Ctrl+R  Retry"
                               : "Ctrl+L  Reconnect",
                    uncertain  ? ""
                    : retained ? "Esc  Discard"
                               : "Esc  Chat");
    } else {
        render_reader();
        // Voice and network panels supply their own controls.
        if (lv_obj_get_child_count(footer_))
            return;
        if (!playback_status.empty())
            footer_hint(playback_status);
        else if (!status.empty() && status != "Message accepted")
            footer_hint(status);
        else if (thread_id.empty())
            footer_hint("Ctrl+L  Retry", "Ctrl+W  Settings");
        else if (messages.empty())
            footer_hint("Hold Enter  Record", "Ctrl+W  Settings");
        else
            footer_hint("↑ ↓  Scroll    ← →  History",
                        visible_voice_clips.empty() ? "Hold Enter  Rec" : "Space  Play");
    }
}
void UI::demo(const std::string &screen) {
    org_id = "Demo workspace";
    battery = {72, true};
    busy = false;
    submitting = recording = voice_pending = uncertain = voice_working = false;
    status.clear();
    playback_status.clear();
    update_message.clear();
    user_code.clear();
    verification_uri.clear();
    if (screen == "welcome" || screen == "expired") {
        page = Page::login;
        if (screen == "expired")
            status = "Code expired or denied; Enter to try again";
    } else if (screen == "login") {
        page = Page::login;
        user_code = "DEMO-CODE";
        verification_uri = "https://example.invalid/activate";
        status = "Demo sign-in code";
    } else {
        open_thread("demo-thread");
        messages = {{"event_1", "user", "What should I pack?", false, {"demo-clip"}},
                    {"event_2",
                     "assistant",
                     "## Ridge walk\n\nPack a **wind shell** and water. The ridge gets cold after "
                     "noon. Keep the map dry.",
                     true,
                     {},
                     "2026-11-24 10:42"}};
        if (screen == "empty")
            messages.clear();
        else if (screen == "saved")
            voice_pending = true;
        else if (screen == "settings")
            page = Page::settings;
        else if (screen == "sent")
            messages.push_back({"sent", "user", "[Voice message]", false, {"demo-clip"}});
        else if (screen == "sending")
            busy = submitting = voice_pending = true;
        else if (screen == "uncertain")
            uncertain = true;
        else if (screen == "offline")
            status = "Network unavailable";
        else if (screen == "history") {
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
        } else if (screen == "playback" || screen == "paused")
            playback_status =
                screen == "paused" ? "Paused   Space resume" : "Playing   Space pause";
        else if (screen == "recording") {
            recording = true;
            recording_started = std::chrono::steady_clock::now();
        } else if (screen == "update") {
            page = Page::settings;
            update_message = "v0.2.3 Ctrl+Alt+U update";
        } else if (screen != "chat")
            throw std::invalid_argument("Unknown demo screen: " + screen);
    }
    selected = 0;
}
} // namespace okou
