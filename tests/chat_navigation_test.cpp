#include "okou/chat_navigation.hpp"
#include <cstdlib>
static void check(bool value) {
    if (!value)
        std::abort();
}
int main() {
    okou::ChatNavigation n;
    n.layout({"a", "b"});
    check(n.position() == 1 && n.offset() == 0);
    n.measure(300, 66);
    n.scroll(22);
    check(n.offset() == 22);
    n.layout({"a", "b", "c"});
    check(n.position() == 1 && n.offset() == 22);
    n.jump(true);
    check(n.offset() == 234 && n.position() == 1);
    n.jump(true);
    check(n.position() == 2 && n.offset() == 0);
    n.layout({"a", "b", "c", "d"});
    check(n.position() == 3);
    n.jump(false);
    check(n.position() == 2);
    n.measure(300, 66);
    n.scroll(220);
    n.jump(false);
    check(n.position() == 2 && !n.offset());
    n.jump(false);
    check(n.position() == 1);
    n.layout({"c", "d"});
    check(n.position() == 1 && !n.offset());
    n.layout({});
    n.jump(true);
    n.scroll(22);
    check(n.position() == -1 && !n.offset());
}
