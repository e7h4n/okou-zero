#include "okou/platform.hpp"
#include <iostream>
#include <linux/input.h>
#include <stdexcept>

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
