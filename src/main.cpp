#include "ui.hpp"
#include "framebuffer.hpp"
#include <array>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <linux/fb.h>
#include <linux/input.h>
#include <stdexcept>
#include <string>
#include <sys/ioctl.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
constexpr int width = 320, height = 170;
volatile std::sig_atomic_t stopped = 0;
void stop(int) { stopped = 1; }
class File {
  public:
    explicit File(const char *path, int flags) : fd_(open(path, flags | O_CLOEXEC)) {
        if (fd_ < 0) throw std::runtime_error(std::string("Cannot open ") + path + ": " + std::strerror(errno));
    }
    ~File() { close(fd_); }
    File(const File &) = delete;
    File &operator=(const File &) = delete;
    int get() const { return fd_; }
  private:
    int fd_;
};
struct Options {
    bool headless = false, version = false;
    std::string output, keys;
    std::string framebuffer = "/dev/fb0";
    std::string input = "/dev/input/by-path/platform-3f804000.i2c-event";
};
Options parse(int argc, char **argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--headless") o.headless = true;
        else if (arg == "--version") o.version = true;
        else if ((arg == "--output" || arg == "--keys" || arg == "--framebuffer" || arg == "--input") && i + 1 < argc) {
            const std::string value = argv[++i];
            if (arg == "--output") o.output = value;
            if (arg == "--keys") o.keys = value;
            if (arg == "--framebuffer") o.framebuffer = value;
            if (arg == "--input") o.input = value;
        } else throw std::runtime_error("Usage: okou-zero [--version] [--headless --output image.ppm [--keys enter,esc]] [--framebuffer PATH] [--input PATH]");
    }
    if (!o.headless && (!o.output.empty() || !o.keys.empty())) throw std::runtime_error("--output and --keys require --headless");
    return o;
}
void headless(const Options &o) {
    // Same UI and reducer as the device; no framebuffer/input device is opened.
    auto *display = lv_display_create(width, height);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB888);
    std::vector<lv_color_t> buffer(width * height), pixels(width * height);
    lv_display_set_user_data(display, &pixels);
    lv_display_set_buffers(display, buffer.data(), nullptr, buffer.size() * sizeof(lv_color_t), LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(display, [](lv_display_t *d, const lv_area_t *, uint8_t *data) {
        auto &out = *static_cast<std::vector<lv_color_t> *>(lv_display_get_user_data(d));
        std::memcpy(out.data(), data, out.size() * sizeof(lv_color_t));
        lv_display_flush_ready(d);
    });
    okou::State state;
    okou::Ui ui;
    std::size_t start = 0;
    while (start < o.keys.size()) {
        const auto end = o.keys.find(',', start);
        const auto key = o.keys.substr(start, end - start);
        if (key != "enter" && key != "esc") throw std::runtime_error("Unknown headless key");
        state.apply(key == "enter" ? okou::Key::enter : okou::Key::escape);
        if (end == std::string::npos) break;
        start = end + 1;
    }
    ui.render(state);
    lv_refr_now(display);
    if (!o.output.empty()) {
        std::ofstream file(o.output, std::ios::binary);
        file << "P6\n320 170\n255\n";
        for (const auto &p : pixels) {
            const std::array<char, 3> rgb{static_cast<char>(p.red), static_cast<char>(p.green), static_cast<char>(p.blue)};
            file.write(rgb.data(), rgb.size());
        }
        if (!file) throw std::runtime_error("Cannot write screenshot");
    }
    std::cout << "greetings=" << state.greetings << " running=" << state.running << '\n';
    lv_display_delete(display);
}
bool device(const Options &o) {
    File fb(o.framebuffer.c_str(), O_RDWR);
    fb_var_screeninfo info{};
    fb_fix_screeninfo fixed{};
    if (ioctl(fb.get(), FBIOGET_VSCREENINFO, &info) < 0 ||
        ioctl(fb.get(), FBIOGET_FSCREENINFO, &fixed) < 0 ||
        !okou::compatible_framebuffer(info, fixed))
        throw std::runtime_error("Requires a valid 320x170 RGB565 framebuffer viewport");
    File keyboard(o.input.c_str(), O_RDONLY | O_NONBLOCK);
    auto *display = lv_linux_fbdev_create();
    if (lv_linux_fbdev_set_file(display, o.framebuffer.c_str()) != LV_RESULT_OK) throw std::runtime_error("Cannot initialize framebuffer");
    lv_linux_fbdev_set_force_refresh(display, true);
    okou::State state;
    okou::Ui ui;
    ui.render(state);
    lv_refr_now(display);
    std::cout << "Okou " << OKOU_VERSION << " ready (320x170)" << std::endl;
    okou::UpdateShortcut shortcut;
    okou::UpdateProcess updater;
    updater.start(false, OKOU_VERSION);
    ui.update(updater.view());
    while ((state.running && !stopped) || updater.installing()) {
        input_event event{};
        ssize_t count;
        while ((count = read(keyboard.get(), &event, sizeof(event))) == sizeof(event)) {
            if (event.type != EV_KEY) continue;
            if (shortcut.event(event.code, event.value) && updater.view().status == okou::UpdateStatus::ready) {
                updater.start(true, OKOU_VERSION);
                ui.update(updater.view());
            }
            if (event.value != 1 || updater.installing()) continue;
            state.apply(event.code == KEY_ENTER ? okou::Key::enter : event.code == KEY_ESC ? okou::Key::escape : okou::Key::other);
            ui.render(state);
        }
        if (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            if (!updater.installing()) throw std::runtime_error("Keyboard read failed");
            state.running = false; // Defer exit until the confirmed install finishes.
        }
        if (updater.poll()) ui.update(updater.view());
        if (updater.view().status == okou::UpdateStatus::installed) {
            lv_display_delete(display);
            return state.running && !stopped;
        }
        // fbdev owns LVGL's monotonic clock callback; do not also tick it here.
        lv_timer_handler();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    lv_display_delete(display);
    std::cout << "Okou exited cleanly; greetings=" << state.greetings << std::endl;
    return false;
}
} // namespace
int main(int argc, char **argv) {
    try {
        const auto options = parse(argc, argv);
        if (options.version) { std::cout << "Okou " << OKOU_VERSION << '\n'; return 0; }
        std::signal(SIGINT, stop);
        std::signal(SIGTERM, stop);
        lv_init();
        bool restart = false;
        if (options.headless) headless(options); else restart = device(options);
        lv_deinit();
        if (restart) {
            execl("/usr/bin/okou-zero", "okou-zero", "--framebuffer", options.framebuffer.c_str(), "--input", options.input.c_str(), static_cast<char *>(nullptr));
            throw std::runtime_error("Update installed; reopen Okou from the launcher");
        }
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "okou-zero: " << error.what() << '\n';
        return 1;
    }
}
