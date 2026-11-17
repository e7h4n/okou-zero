#include "update_process.hpp"
#include <linux/input.h>
#include <stdexcept>

void check(bool value) {
    if (!value)
        throw std::runtime_error("Update shortcut check failed");
}
int main() {
    okou::UpdateShortcut chord;
    check(!chord.event(KEY_U, 1));
    check(!chord.event(KEY_LEFTCTRL, 1));
    check(!chord.event(KEY_U, 1));
    check(!chord.event(KEY_RIGHTALT, 1));
    check(chord.event(KEY_U, 1));
    check(!chord.event(KEY_U, 2));
    check(!chord.event(KEY_U, 0));
    check(!chord.event(KEY_LEFTCTRL, 0));
    check(!chord.event(KEY_U, 1));
    check(!chord.event(KEY_RIGHTCTRL, 1));
    check(chord.event(KEY_U, 1));
    check(!chord.event(KEY_RIGHTALT, 0));
    check(!chord.event(KEY_U, 1));
    check(okou::update_hint({okou::UpdateStatus::ready, "0.2.2"}).find("Ctrl+Alt+U") !=
          std::string::npos);
}
