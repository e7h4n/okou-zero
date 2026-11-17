#include "ui.hpp"
#include <string>

namespace okou {
void State::apply(Key key) {
    if (!running) return;
    if (key == Key::escape) running = false;
    if (key == Key::enter && greetings < 9999) ++greetings;
}
namespace {
lv_obj_t *label(const char *text, int x, int y, const lv_font_t *font, unsigned color) {
    auto *obj = lv_label_create(lv_screen_active());
    lv_label_set_text(obj, text);
    lv_obj_set_style_text_font(obj, font, 0);
    lv_obj_set_style_text_color(obj, lv_color_hex(color), 0);
    lv_obj_set_pos(obj, x, y);
    return obj;
}
} // namespace
Ui::Ui() {
    auto *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0xF8F5F3), 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    label("Okou", 16, 12, &lv_font_montserrat_14, 0x5D6658);
    label("Hello, world!", 16, 40, &lv_font_montserrat_28, 0x202820);
    message_ = label("Ready on your Cardputer.", 16, 83, &lv_font_montserrat_14, 0x202820);
    counter_ = label("Offline demo / no account needed", 16, 106, &lv_font_montserrat_12, 0x5D6658);
    update_ = label("", 16, 124, &lv_font_montserrat_12, 0x5D6658);
    label("ENTER  Say hello     ESC  Back", 16, 145, &lv_font_montserrat_12, 0x202820);
}
void Ui::render(const State &state) {
    if (state.greetings) {
        lv_label_set_text(message_, "Hello from Okou!");
        const auto text = "Keyboard check: " + std::to_string(state.greetings);
        lv_label_set_text(counter_, text.c_str());
    }
    lv_obj_invalidate(lv_screen_active());
}
void Ui::update(const UpdateView &view) {
    std::string text;
    switch (view.status) {
    case UpdateStatus::checking: text = "Checking for updates..."; break;
    case UpdateStatus::downloading: text = "Downloading update..."; break;
    case UpdateStatus::ready: text = "v" + view.version + " ready: Ctrl+Alt+U"; break;
    case UpdateStatus::installing: text = "Updating... Do not power off"; break;
    case UpdateStatus::current: text = "Up to date"; break;
    case UpdateStatus::failed: text = "Update unavailable / try next launch"; break;
    case UpdateStatus::installed: text = "Updated. Restarting..."; break;
    case UpdateStatus::idle: break;
    }
    lv_label_set_text(update_, text.c_str());
}
} // namespace okou
