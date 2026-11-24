#include "okou/chat_navigation.hpp"
#include <cstdlib>

static void check(bool value) {
    if (!value)
        std::abort();
}

int main() {
    okou::ChatNavigation nav;
    const std::vector<okou::MessageBounds> rows{{"a", 0, 220}, {"b", 228, 260}, {"c", 268, 600}};
    nav.layout(rows, 100);
    check(nav.offset() == 500);
    nav.jump(false);
    check(nav.selected() == "c" && nav.offset() == 268);
    nav.jump(false);
    check(nav.selected() == "b" && nav.offset() == 228);
    nav.jump(false);
    check(nav.selected() == "a" && nav.offset() == 120);
    nav.jump(false);
    nav.jump(false);
    check(nav.selected() == "a" && nav.offset() == 0);
    nav.jump(true);
    check(nav.selected() == "a" && nav.offset() == 120);
    nav.jump(true);
    check(nav.selected() == "b" && nav.offset() == 160);
    nav.jump(true);
    check(nav.selected() == "c" && nav.offset() == 268);
    nav.scroll(-50);
    nav.jump(true);
    check(nav.selected() == "c" && nav.offset() == 500);
    auto appended = rows;
    appended.push_back({"d", 608, 650});
    nav.layout(appended, 100);
    check(nav.offset() == 500);
    nav.layout({{"s", 0, 20}, {"t", 28, 50}}, 100);
    nav.jump(false);
    check(nav.selected() == "s" && nav.offset() == 0);
    nav.jump(true);
    check(nav.selected() == "t" && nav.offset() == 0);
    nav.layout({}, 100);
    nav.jump(true);
    check(nav.selected().empty() && nav.offset() == 0);
}
