#pragma once
#include <lvgl.h>

namespace okou {
enum class Key { enter, escape, other };
struct State {
    unsigned greetings = 0;
    bool running = true;
    void apply(Key key);
};
class Ui {
  public:
    Ui();
    void render(const State &state);
  private:
    lv_obj_t *message_;
    lv_obj_t *counter_;
};
} // namespace okou
