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
    space,
    volume_up,
    volume_down,
    mute,
    enter,
    enter_down,
    enter_up,
    enter_cancel,
    backspace,
    escape,
    up,
    down,
    left,
    right,
    page_up,
    page_down,
    retry_voice,
    workspace,
    reload,
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
struct VolumeStatus {
    int percent = -1;
    bool muted = false;
};
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
    std::vector<Input> poll(bool wake_only = false);
    int descriptor() const {
        return fd_;
    }
    bool activity() const {
        return activity_;
    }

  private:
    bool desktop_, shift_ = false, ctrl_ = false, caps_ = false;
    bool input_dropped_ = false, activity_ = false;
    int wake_key_ = -1;
    int fd_ = -1;
    std::unique_ptr<UpdateShortcut> update_shortcut_;
};
} // namespace okou
