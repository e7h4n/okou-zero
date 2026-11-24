#pragma once
#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace okou {
// Only replies have a reading position. A blank anchor follows the live reply.
class ChatNavigation {
  public:
    void layout(std::vector<std::string> ids) {
        ids_ = std::move(ids);
        auto it = std::find(ids_.begin(), ids_.end(), anchor_);
        if (it == ids_.end())
            anchor_.clear();
        position_ = anchor_.empty() ? static_cast<int>(ids_.size()) - 1
                                    : static_cast<int>(it - ids_.begin());
        std::string current = position_ < 0 ? std::string{} : ids_[position_];
        if (current != measured_) {
            offset_ = limit_ = 0;
            measured_ = std::move(current);
        }
    }
    void measure(int content, int viewport) {
        limit_ = std::max(0, content - viewport);
        offset_ = std::min(offset_, limit_);
    }
    void scroll(int delta) {
        if (position_ < 0)
            return;
        offset_ = std::clamp(offset_ + delta, 0, limit_);
        if (offset_ > 0)
            anchor_ = ids_[position_];
    }
    void jump(bool newer) {
        if (position_ < 0)
            return;
        if (newer && offset_ < limit_) {
            scroll(limit_);
            return;
        }
        if (!newer && offset_) {
            scroll(-offset_);
            return;
        }
        int next = std::clamp(position_ + (newer ? 1 : -1), 0, static_cast<int>(ids_.size()) - 1);
        if (next != position_) {
            position_ = next;
            measured_ = ids_[next];
            offset_ = limit_ = 0;
        }
        anchor_ = position_ + 1 == static_cast<int>(ids_.size()) ? std::string{} : ids_[position_];
    }
    int position() const {
        return position_;
    }
    int offset() const {
        return offset_;
    }

  private:
    std::vector<std::string> ids_;
    std::string anchor_, measured_;
    int position_ = -1, offset_ = 0, limit_ = 0;
};
} // namespace okou
