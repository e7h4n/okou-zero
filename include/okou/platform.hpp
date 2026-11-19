#pragma once
#include "lvgl.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace okou {
enum class Key {
    none,
    text,
    enter,
    backspace,
    escape,
    up,
    down,
    left,
    right,
    tab,
    page_up,
    page_down,
    record,
    workspace,
    reload,
    logout,
    update,
    quit
};
struct Input {
    Key key = Key::none;
    std::string text;
    bool shift = false;
    bool ctrl = false;
};
Input map_key(std::uint16_t code, bool shift, bool caps, bool ctrl);
void erase_utf8(std::string &text);
struct BatteryStatus {
    int percent = -1;
    bool charging = false;
};
BatteryStatus read_battery_status();
class Display {
  public:
    Display(bool desktop, const std::string &framebuffer, bool headless = false);
    ~Display();
    void present();
    void screenshot(const std::string &path);
    lv_display_t *get() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
class UpdateShortcut;
class Keyboard {
  public:
    Keyboard(bool desktop, const std::string &path);
    ~Keyboard();
    std::vector<Input> poll();

  private:
    bool desktop_, shift_ = false, ctrl_ = false, caps_ = false;
    int fd_ = -1;
    std::unique_ptr<UpdateShortcut> update_shortcut_;
};
} // namespace okou
