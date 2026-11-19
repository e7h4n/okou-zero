#pragma once

#include "okou/platform.hpp"
#include <memory>
#include <string>
#include <utility>

namespace okou {
std::pair<std::string, std::string> split_emoji_title(const std::string &title);

class EmojiRenderer {
  public:
    EmojiRenderer();
    ~EmojiRenderer();
    bool draw(lv_obj_t *parent, const std::string &emoji, int x, int y, int size);

  private:
    struct Font;
    std::unique_ptr<Font> font_;
};
} // namespace okou
