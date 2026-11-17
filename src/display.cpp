#include "okou/platform.hpp"
#include <algorithm>
#include <cstring>
#include <fcntl.h>
#include <linux/fb.h>
#include <stdexcept>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#if OKOU_ZERO_DESKTOP
#include <SDL2/SDL.h>
#endif

namespace okou {
struct Display::Impl {
    bool desktop;
    lv_display_t* display = nullptr;
    std::vector<std::uint16_t> buffer = std::vector<std::uint16_t>(320 * 34);
    std::vector<std::uint16_t> pixels = std::vector<std::uint16_t>(320 * 170);
    int fd = -1;
    std::uint8_t* mapped = nullptr;
    std::size_t map_size = 0;
    fb_fix_screeninfo fixed{};
    fb_var_screeninfo variable{};
    bool dirty = true;
#if OKOU_ZERO_DESKTOP
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    SDL_Texture* texture = nullptr;
#endif
    explicit Impl(bool d) : desktop(d) {}
    ~Impl() {
        if (display) lv_display_delete(display);
        if (mapped) munmap(mapped, map_size);
        if (fd >= 0) close(fd);
#if OKOU_ZERO_DESKTOP
        if (texture) SDL_DestroyTexture(texture);
        if (renderer) SDL_DestroyRenderer(renderer);
        if (window) SDL_DestroyWindow(window);
        if (desktop) SDL_Quit();
#endif
    }
    static void flush(lv_display_t* display, const lv_area_t* area, std::uint8_t* data) {
        auto& self = *static_cast<Impl*>(lv_display_get_user_data(display));
        int width = area->x2 - area->x1 + 1;
        auto* input = reinterpret_cast<std::uint16_t*>(data);
        for (int y = area->y1; y <= area->y2; ++y) {
            if (y >= 0 && y < 170 && area->x1 >= 0 && area->x2 < 320) {
                std::memcpy(self.pixels.data() + y * 320 + area->x1,
                            input + (y - area->y1) * width, static_cast<std::size_t>(width) * 2);
                if (self.mapped) {
                    auto offset = static_cast<std::size_t>(y + self.variable.yoffset) * self.fixed.line_length
                                + static_cast<std::size_t>(area->x1 + self.variable.xoffset) * 2;
                    if (offset + static_cast<std::size_t>(width) * 2 <= self.map_size)
                        std::memcpy(self.mapped + offset, input + (y - area->y1) * width,
                                    static_cast<std::size_t>(width) * 2);
                }
            }
        }
        self.dirty = true;
        lv_display_flush_ready(display);
    }
};
Display::Display(bool desktop, const std::string& framebuffer) : impl_(std::make_unique<Impl>(desktop)) {
    auto& p = *impl_;
#if OKOU_ZERO_DESKTOP
    if (desktop) {
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) throw std::runtime_error("Cannot initialize display");
        p.window = SDL_CreateWindow("Okou Zero", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 960, 510, SDL_WINDOW_ALLOW_HIGHDPI);
        if (!p.window) throw std::runtime_error("Cannot create simulator window");
        p.renderer = SDL_CreateRenderer(p.window, -1, SDL_RENDERER_SOFTWARE);
        if (!p.renderer) throw std::runtime_error("Cannot create simulator renderer");
        SDL_RenderSetLogicalSize(p.renderer, 320, 170);
        p.texture = SDL_CreateTexture(p.renderer, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING, 320, 170);
        if (!p.texture) throw std::runtime_error("Cannot create simulator framebuffer");
    }
#else
    if (desktop) throw std::runtime_error("This package has no desktop simulator");
#endif
    if (!desktop) {
        p.fd = open(framebuffer.c_str(), O_RDWR | O_CLOEXEC);
        if (p.fd < 0 || ioctl(p.fd, FBIOGET_FSCREENINFO, &p.fixed) || ioctl(p.fd, FBIOGET_VSCREENINFO, &p.variable))
            throw std::runtime_error("Cannot open LCD framebuffer; check video-group access");
        if (p.variable.xres != 320 || p.variable.yres != 170 || p.variable.bits_per_pixel != 16
            || p.variable.red.offset != 11 || p.variable.green.offset != 5 || p.variable.blue.offset != 0)
            throw std::runtime_error("LCD must be 320x170 RGB565");
        p.map_size = p.fixed.smem_len;
        auto* mapping = mmap(nullptr, p.map_size, PROT_READ | PROT_WRITE, MAP_SHARED, p.fd, 0);
        if (mapping == MAP_FAILED) throw std::runtime_error("Cannot map LCD framebuffer");
        p.mapped = static_cast<std::uint8_t*>(mapping);
    }
    p.display = lv_display_create(320, 170);
    lv_display_set_user_data(p.display, &p);
    lv_display_set_color_format(p.display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(p.display, p.buffer.data(), nullptr, p.buffer.size() * sizeof(std::uint16_t), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(p.display, Impl::flush);
}
Display::~Display() = default;
lv_display_t* Display::get() const { return impl_->display; }
void Display::present() {
#if OKOU_ZERO_DESKTOP
    auto& p = *impl_;
    if (p.desktop && p.dirty) {
        SDL_UpdateTexture(p.texture, nullptr, p.pixels.data(), 320 * 2);
        SDL_RenderClear(p.renderer);
        SDL_RenderCopy(p.renderer, p.texture, nullptr, nullptr);
        SDL_RenderPresent(p.renderer);
        p.dirty = false;
    }
#endif
}
void Display::screenshot(const std::string& path) {
#if OKOU_ZERO_DESKTOP
    auto& p = *impl_;
    auto* surface = SDL_CreateRGBSurfaceWithFormatFrom(p.pixels.data(), 320, 170, 16, 640, SDL_PIXELFORMAT_RGB565);
    if (!surface) throw std::runtime_error("Cannot create screenshot");
    int result = SDL_SaveBMP(surface, path.c_str());
    SDL_FreeSurface(surface);
    if (result != 0) throw std::runtime_error("Cannot write screenshot");
#else
    (void)path;
    throw std::runtime_error("Use the authorized VNC session for physical screenshots");
#endif
}
} // namespace okou
