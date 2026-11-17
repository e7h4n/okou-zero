#include "ui.hpp"
#include "framebuffer.hpp"
#include <iostream>
#include <stdexcept>
void check(bool value) { if (!value) throw std::runtime_error("State check failed"); }
int main() {
    okou::State state;
    check(state.running && state.greetings == 0);
    state.apply(okou::Key::other);
    check(state.greetings == 0);
    state.apply(okou::Key::enter);
    check(state.greetings == 1 && state.running);
    state.apply(okou::Key::escape);
    state.apply(okou::Key::enter);
    check(!state.running && state.greetings == 1);
    okou::State bounded;
    for (int i = 0; i < 10010; ++i) bounded.apply(okou::Key::enter);
    check(bounded.greetings == 9999);
    fb_var_screeninfo v{};
    fb_fix_screeninfo f{};
    v.xres = v.xres_virtual = 320;
    v.yres = v.yres_virtual = 170;
    v.bits_per_pixel = 16;
    v.red.offset = 11; v.red.length = 5;
    v.green.offset = 5; v.green.length = 6;
    v.blue.length = 5;
    f.type = FB_TYPE_PACKED_PIXELS;
    f.visual = FB_VISUAL_TRUECOLOR;
    f.line_length = 640; f.smem_len = 108800;
    check(okou::compatible_framebuffer(v, f));
    v.yoffset = 170;
    check(!okou::compatible_framebuffer(v, f));
    v.yoffset = 0; v.green.length = 5;
    check(!okou::compatible_framebuffer(v, f));
    v.green.length = 6; f.smem_len = 1;
    check(!okou::compatible_framebuffer(v, f));
    f.smem_len = 108800; f.line_length = 639;
    check(!okou::compatible_framebuffer(v, f));
    // fbdev allocates its RGB565 screen buffer with lv_malloc, unlike the
    // headless RGB888 renderer's std::vector. Cover that exact allocation path.
    lv_init();
    auto *display = lv_display_create(320, 170);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    auto *pixels = lv_malloc(320 * 170 * 2);
    check(pixels != nullptr);
    lv_display_set_buffers(display, pixels, nullptr, 320 * 170 * 2, LV_DISPLAY_RENDER_MODE_DIRECT);
    bool flushed = false;
    lv_display_set_user_data(display, &flushed);
    lv_display_set_flush_cb(display, [](lv_display_t *d, const lv_area_t *, uint8_t *) {
        *static_cast<bool *>(lv_display_get_user_data(d)) = true;
        lv_display_flush_ready(d);
    });
    okou::Ui ui;
    ui.render(okou::State{});
    lv_refr_now(display);
    check(flushed);
    lv_display_delete(display);
    lv_free(pixels);
    lv_deinit();
    std::cout << "State, framebuffer metadata and device-buffer render tests passed\n";
}
