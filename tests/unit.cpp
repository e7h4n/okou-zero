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
    std::cout << "State and framebuffer metadata tests passed\n";
}
