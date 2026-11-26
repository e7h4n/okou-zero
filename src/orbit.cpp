// SPDX-License-Identifier: MIT
#include "orbit.hpp"
#include <algorithm>
#include <cmath>

namespace okou {
namespace {
constexpr double pi = 3.141592653589793, tau = 2 * pi;
constexpr uint32_t lime = 0xdafa8c, pale = 0xb5c899, moss = 0x647c76;
constexpr int left = 147, top = 17;
std::filesystem::path asset_directory() {
    const auto bin = std::filesystem::read_symlink("/proc/self/exe").parent_path();
    const auto installed = bin.parent_path() / "share/okou-zero/orbit";
    return std::filesystem::is_directory(bin / "orbit") ? bin / "orbit" : installed;
}
double smooth(double t) {
    t = std::clamp(t, 0., 1.);
    return t * t * (3 - 2 * t);
}
struct Point {
    double x, y;
};
void line(lv_layer_t *layer, Point a, Point b, uint32_t color, int width = 1) {
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.p1 = {static_cast<lv_value_precise_t>(std::lround(a.x)),
            static_cast<lv_value_precise_t>(std::lround(a.y))};
    d.p2 = {static_cast<lv_value_precise_t>(std::lround(b.x)),
            static_cast<lv_value_precise_t>(std::lround(b.y))};
    d.color = lv_color_hex(color);
    d.width = width;
    lv_draw_line(layer, &d);
}
void dot(lv_layer_t *layer, Point p, double radius, uint32_t color) {
    const int x = std::lround(p.x), y = std::lround(p.y), r = std::max(1L, std::lround(radius));
    lv_area_t area{x - r, y - r, x + r, y + r};
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = lv_color_hex(color);
    d.radius = LV_RADIUS_CIRCLE;
    lv_draw_rect(layer, &d, &area);
}
Point along(Point c, double rx, double ry, double a) {
    // Project a tilted ellipse, shared by the guide line and its moving satellite.
    double x = rx * std::cos(a), y = ry * std::sin(a);
    return {c.x + .928 * x + .373 * y, c.y - .373 * x + .928 * y};
}
void ring(lv_layer_t *layer, Point c, double rx, double ry, double begin, double end,
          uint32_t color) {
    const int steps = std::max(8, static_cast<int>((end - begin) * std::sqrt(rx)));
    Point last = along(c, rx, ry, begin);
    for (int i = 1; i <= steps; ++i) {
        Point next = along(c, rx, ry, begin + (end - begin) * i / steps);
        line(layer, last, next, color);
        last = next;
    }
}
void star(lv_layer_t *layer, Point c, double size) {
    line(layer, {c.x - size, c.y}, {c.x + size, c.y}, lime);
    line(layer, {c.x, c.y - size}, {c.x, c.y + size}, lime);
    dot(layer, c, std::max(1., size * .36), 0xeaffaf);
}
} // namespace

PocketOrbit::PocketOrbit(lv_obj_t *root, const RecordingWaveform &mic)
    : directory(asset_directory()), microphone_(mic),
      sky_path_("A:" + (directory / "sky.png").string()),
      avatar_path_("A:" + (directory / "avatar.png").string()),
      blink_path_("A:" + (directory / "avatar-blink.png").string()) {
    object_ = lv_obj_create(root);
    lv_obj_remove_style_all(object_);
    lv_obj_remove_flag(object_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(object_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(object_, left, top);
    lv_obj_set_size(object_, 320 - left, 127);
    lv_obj_add_event_cb(
        object_,
        [](lv_event_t *event) {
            static_cast<PocketOrbit *>(lv_event_get_user_data(event))->draw(event);
        },
        LV_EVENT_DRAW_MAIN, this);
    timer_ = lv_timer_create(
        [](lv_timer_t *timer) {
            auto &art = *static_cast<PocketOrbit *>(lv_timer_get_user_data(timer));
            lv_obj_invalidate(art.object_);
            if (!art.continuous() && lv_tick_elaps(art.entered_) >= 340U)
                lv_timer_pause(timer);
        },
        50, this);
    lv_timer_pause(timer_);
}
PocketOrbit::~PocketOrbit() {
    lv_timer_delete(timer_);
    lv_obj_delete(object_);
}
void PocketOrbit::nudge() {
    if (scene_ == OrbitScene::welcome) {
        nudged_ = lv_tick_get();
        nudging_ = true;
    }
}
void PocketOrbit::sky(lv_obj_t *parent) const {
    auto *image = lv_image_create(parent);
    lv_image_set_src(image, sky_path_.c_str());
    lv_obj_set_pos(image, 0, 0);
}
bool PocketOrbit::continuous() const {
    return scene_ == OrbitScene::welcome || scene_ == OrbitScene::record ||
           scene_ == OrbitScene::send || scene_ == OrbitScene::wait;
}
PocketOrbit::Pose PocketOrbit::pose(uint32_t age) const {
    Pose to{};
    switch (scene_) {
    case OrbitScene::welcome:
        to = {231, 78, 46};
        break;
    case OrbitScene::pairing:
        to = {271, 70, 24};
        break;
    case OrbitScene::ready:
        to = {233, 78, 35};
        break;
    case OrbitScene::record:
        to = {233, 77, 39};
        break;
    case OrbitScene::send:
        to = {244, 73, 36};
        break;
    case OrbitScene::wait:
        to = {233, 77, 35};
        break;
    case OrbitScene::bookmark:
        to = {293, double(marker_y_), 6};
        break;
    case OrbitScene::marker:
        to = {286, double(marker_y_), 8};
        break;
    }
    if (!shown_)
        return to;
    const double f = smooth(double(age) / (scene_ == OrbitScene::send ? 900 : 340));
    return {from_.x + (to.x - from_.x) * f, from_.y + (to.y - from_.y) * f,
            from_.radius + (to.radius - from_.radius) * f};
}
void PocketOrbit::show(OrbitScene next, int marker_y) {
    lv_obj_move_foreground(object_);
    if (shown_ && scene_ == next && marker_y_ == marker_y)
        return;
    const auto previous = pose(lv_tick_elaps(entered_));
    scene_ = next;
    nudging_ = false;
    marker_y_ = marker_y;
    entered_ = lv_tick_get();
    from_ = shown_ ? previous : pose(0);
    shown_ = true;
    lv_obj_invalidate(object_);
    lv_timer_reset(timer_);
    lv_timer_resume(timer_);
}
void PocketOrbit::draw(lv_event_t *event) const {
    if (!shown_)
        return;
    const auto age = lv_tick_elaps(entered_);
    const auto p = pose(age);
    lv_area_t area;
    lv_obj_get_coords(object_, &area);
    Point c{p.x + area.x1 - left, p.y + area.y1 - top};
    auto *layer = lv_event_get_layer(event);
    const double r = p.radius;
    double phase = .75, level = 0;
    if (scene_ == OrbitScene::record) {
        const auto peaks = microphone_.snapshot();
        int amplitude = 0;
        for (size_t i = peaks.size() - 4; i < peaks.size(); ++i)
            amplitude = std::max({amplitude, -int(peaks[i].low), int(peaks[i].high)});
        level = std::log1p(amplitude / 120.) / std::log1p(32768. / 120.);
        phase += age * .0012;
    } else if (scene_ == OrbitScene::wait || scene_ == OrbitScene::welcome)
        phase += age * (scene_ == OrbitScene::wait ? .001 : .0008);
    if (scene_ == OrbitScene::send) {
        phase += age * .004;
        c.x += 5 * std::sin(phase);
        c.y -= 3 * std::sin(2 * phase);
    }
    if (r < 14) {
        ring(layer, c, r * 1.18, r * .46, 0, tau, pale);
        dot(layer, c, std::max(1., r * .20), lime);
        dot(layer, along(c, r * 1.18, r * .46, .1), 1, lime);
        return;
    }
    const bool welcome = scene_ == OrbitScene::welcome;
    if (welcome) {
        c.y += std::sin(phase * 1.7) * 2;
        const auto elapsed = lv_tick_elaps(nudged_);
        if (nudging_ && elapsed < 850) {
            double t = elapsed / 850., excursion = 13 * std::sin(t * pi);
            c.x += excursion * std::cos(t * tau);
            c.y -= excursion * std::sin(t * tau);
            phase += tau * smooth(t);
        }
    }
    dot(layer, c, r * .82, 0x263847);
    for (int h = -int(r * .72); h < int(r * .75); h += 5) {
        const double edge = std::sqrt(std::max(0., r * r * .64 - h * h));
        line(layer, {c.x - edge, c.y + h}, {c.x + edge, c.y + h}, 0x344653);
    }
    Point center{c.x, c.y + r * .10};
    const double rx = r * (1.34 + .28 * level), ry = r * (.40 + .24 * level);
    ring(layer, center, rx * .91, ry * .8, 0, tau, moss);
    ring(layer, center, rx, ry, 0, tau, pale);
    const double side = r * (scene_ == OrbitScene::wait ? 1.40 : 1.85);
    const int x = std::lround(c.x) - 48, y = std::lround(c.y - r * .08) - 48;
    lv_area_t image_area{x, y, x + 95, y + 95};
    lv_draw_image_dsc_t face;
    lv_draw_image_dsc_init(&face);
    const unsigned blink = age % 4800;
    const bool eyes_closed =
        welcome && ((blink >= 3440 && blink < 3550) || (blink >= 3790 && blink < 3870));
    face.src = eyes_closed ? blink_path_.c_str() : avatar_path_.c_str();
    face.pivot = {48, 48};
    face.scale_x = face.scale_y = std::lround(LV_SCALE_NONE * side / 96.);
    face.rotation = std::lround(-3 * 10 + (welcome ? 22 * std::sin(phase) : 0));
    lv_draw_image(layer, &face, &image_area);
    ring(layer, center, rx, ry, .02, pi, 0xd9ecab);
    star(layer, along(center, rx, ry, phase), std::max(2., r * .09));
}
} // namespace okou
