#include "lvgl.h"
#include "okou/framebuffer.hpp"
#include <csignal>
#include <iostream>
#include <stdexcept>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

static_assert(LV_USE_STDLIB_MALLOC == LV_STDLIB_CLIB,
              "Use the system allocator, not LVGL's small fixed heap");
int main() {
    try {
        auto check = [](bool value) {
            if (!value)
                throw std::runtime_error("Framebuffer regression");
        };
        fb_var_screeninfo variable{};
        fb_fix_screeninfo fixed{};
        variable.xres = variable.xres_virtual = 320;
        variable.yres = variable.yres_virtual = 170;
        variable.bits_per_pixel = 16;
        variable.red.offset = 11;
        variable.red.length = 5;
        variable.green.offset = 5;
        variable.green.length = 6;
        variable.blue.offset = 0;
        variable.blue.length = 5;
        fixed.line_length = 640;
        fixed.smem_len = 108800;
        check(okou::compatible_framebuffer(variable, fixed));
        --fixed.smem_len;
        check(!okou::compatible_framebuffer(variable, fixed));
        ++fixed.smem_len;
        --fixed.line_length;
        check(!okou::compatible_framebuffer(variable, fixed));
        ++fixed.line_length;
        variable.red.length = 8;
        check(!okou::compatible_framebuffer(variable, fixed));
        variable.red.length = 5;
        variable.xoffset = 1;
        check(!okou::compatible_framebuffer(variable, fixed));
        variable.xoffset = 0;
        variable.bits_per_pixel = 32;
        check(!okou::compatible_framebuffer(variable, fixed));
        lv_init();
        auto *display = lv_display_create(320, 170);
        auto *pixels = lv_malloc(320 * 170 * 2);
        check(display != nullptr && pixels != nullptr);
        lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
        lv_display_set_buffers(display, pixels, nullptr, 320 * 170 * 2,
                               LV_DISPLAY_RENDER_MODE_DIRECT);
        bool flushed = false;
        lv_display_set_user_data(display, &flushed);
        lv_display_set_flush_cb(display, [](lv_display_t *target, const lv_area_t *, uint8_t *) {
            *static_cast<bool *>(lv_display_get_user_data(target)) = true;
            lv_display_flush_ready(target);
        });
        auto *text = lv_label_create(lv_display_get_screen_active(display));
        lv_label_set_text(text, "Okou framebuffer allocation regression");
        lv_refr_now(display);
        check(flushed);
        lv_display_delete(display);
        lv_free(pixels);
        lv_deinit();
        auto child = fork();
        check(child >= 0);
        if (child == 0) {
            rlimit no_core{0, 0};
            setrlimit(RLIMIT_CORE, &no_core);
            LV_ASSERT_NULL(nullptr);
            _exit(2);
        }
        int status = 0;
        bool exited = false;
        for (int attempt = 0; attempt < 100; ++attempt) {
            if (waitpid(child, &status, WNOHANG) == child) {
                exited = true;
                break;
            }
            usleep(10000);
        }
        if (!exited) {
            kill(child, SIGKILL);
            waitpid(child, &status, 0);
        }
        check(exited && WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
        std::cout << "Framebuffer layout, allocation/render and non-spinning assertions passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
