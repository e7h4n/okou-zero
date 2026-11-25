#include "okou/platform.hpp"
#include <iostream>
#include <linux/input.h>
#include <stdexcept>
#include <unistd.h>

int main() {
    using namespace okou;
    try {
        auto check = [](bool condition) {
            if (!condition)
                throw std::runtime_error("Keyboard mapping failed");
        };
        check(map_key(KEY_A, false, false, false).text == "a");
        check(map_key(KEY_A, true, false, false).text == "A");
        check(map_key(KEY_A, false, true, false).text == "A");
        check(map_key(KEY_A, true, true, false).text == "a");
        check(map_key(KEY_1, true, false, false).text == "!");
        check(map_key(KEY_APOSTROPHE, true, false, false).text == "\"");
        check(map_key(KEY_BACKSLASH, false, false, false).text == "\\");
        check(map_key(KEY_R, false, false, true).key == Key::record);
        check(map_key(KEY_W, false, false, true).key == Key::workspace);
        check(map_key(KEY_ENTER, true, false, false).shift);
        check(map_key(KEY_ENTER, false, false, true).key == Key::enter);
        check(map_key(KEY_UP, false, false, false).key == Key::up);
        check(map_key(KEY_F12, false, false, false).key == Key::none);
        int ports[2];
        check(pipe(ports) == 0);
        Keyboard keyboard(false, "/proc/self/fd/" + std::to_string(ports[0]));
        const auto emit = [&](unsigned short type, unsigned short code, int value) {
            input_event event{};
            event.type = type;
            event.code = code;
            event.value = value;
            check(write(ports[1], &event, sizeof(event)) == sizeof(event));
        };
        emit(EV_KEY, KEY_ENTER, 1);
        emit(EV_KEY, KEY_ENTER, 2);
        auto events = keyboard.poll();
        check(events.size() == 1 && events[0].key == Key::enter_down);
        emit(EV_KEY, KEY_ENTER, 0);
        events = keyboard.poll();
        check(events.size() == 1 && events[0].key == Key::enter_up);
        emit(EV_SYN, SYN_DROPPED, 0);
        emit(EV_KEY, KEY_ENTER, 0); // Discard the out-of-sync release.
        emit(EV_SYN, SYN_REPORT, 0);
        emit(EV_KEY, KEY_RIGHTSHIFT, 1);
        emit(EV_KEY, KEY_KPENTER, 1);
        events = keyboard.poll();
        check(events.size() == 2 && events[0].key == Key::enter_cancel &&
              events[1].key == Key::enter_down && events[1].shift);
        emit(EV_KEY, KEY_VOLUMEUP, 1);
        emit(EV_KEY, KEY_VOLUMEUP, 2);
        emit(EV_KEY, KEY_VOLUMEUP, 0);
        emit(EV_KEY, KEY_VOLUMEDOWN, 1);
        emit(EV_KEY, KEY_MUTE, 1);
        emit(EV_KEY, KEY_MUTE, 2);
        emit(EV_KEY, KEY_MUTE, 0);
        events = keyboard.poll();
        check(events.size() == 4 && events[0].key == Key::volume_up &&
              events[1].key == Key::volume_up && events[2].key == Key::volume_down &&
              events[3].key == Key::mute);
        // The wake gesture may span several event batches. Neither repeats nor release
        // may leak through to push-to-talk, but the next deliberate press must work.
        emit(EV_KEY, KEY_ENTER, 1);
        check(keyboard.poll(true).empty() && keyboard.activity());
        emit(EV_KEY, KEY_ENTER, 2);
        check(keyboard.poll().empty());
        emit(EV_KEY, KEY_ENTER, 0);
        emit(EV_KEY, KEY_ENTER, 1);
        emit(EV_KEY, KEY_ENTER, 0);
        events = keyboard.poll();
        check(events.size() == 2 && events[0].key == Key::enter_down &&
              events[1].key == Key::enter_up);
        close(ports[0]);
        close(ports[1]);
        std::string text = "a中🙂";
        erase_utf8(text);
        check(text == "a中");
        erase_utf8(text);
        check(text == "a");
        erase_utf8(text);
        check(text.empty());
        erase_utf8(text);
        check(text.empty());
        std::cout << "Keyboard and UTF-8 tests passed\n";
        return 0;
    } catch (const std::exception &ex) {
        std::cerr << ex.what() << '\n';
        return 1;
    }
}
