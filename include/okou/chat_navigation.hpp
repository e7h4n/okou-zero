#pragma once

#include <algorithm>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace okou {
struct MessageBounds {
    std::string id;
    int top, bottom; // Content coordinates, excluding the gap after the message.
};

class ChatNavigation {
  public:
    void layout(std::vector<MessageBounds> rows, int height) {
        rows_ = std::move(rows);
        height_ = height;
        const auto selected = find_selection();
        if (selected != rows_.end())
            align(*selected, at_start_);
        else {
            selected_.clear();
            from_bottom_ = std::min(from_bottom_, limit());
        }
    }
    void scroll(int towards_top) {
        selected_.clear();
        from_bottom_ = std::clamp(from_bottom_ + towards_top, 0, limit());
    }
    void jump(bool right) {
        if (rows_.empty())
            return;
        const int top = offset();
        auto row = find_selection();
        if (row == rows_.end()) {
            // At the live end the latest message is current. Elsewhere use the
            // message at the top of the reading area (including after scrolling).
            row = from_bottom_ == 0 ? std::prev(rows_.end())
                                    : std::find_if(rows_.begin(), rows_.end(),
                                                   [top](auto &r) { return r.bottom > top; });
        }
        bool start = !right;
        if (right && row->bottom <= top + height_ && std::next(row) != rows_.end()) {
            ++row;
            start = true;
        } else if (!right && row->top >= top && row != rows_.begin()) {
            --row;
            start = false;
        }
        selected_ = row->id;
        at_start_ = start;
        align(*row, start);
    }
    int offset() const {
        return limit() - from_bottom_;
    }
    const std::string &selected() const {
        return selected_;
    }

  private:
    int limit() const {
        return rows_.empty() ? 0 : std::max(0, rows_.back().bottom - height_);
    }
    std::vector<MessageBounds>::iterator find_selection() {
        return std::find_if(rows_.begin(), rows_.end(),
                            [this](auto &r) { return !selected_.empty() && r.id == selected_; });
    }
    void align(const MessageBounds &row, bool start) {
        // A short neighbor may fit already. Keep it entirely visible without
        // moving backwards just to align it to the opposite viewport edge.
        int pos = row.bottom - row.top <= height_
                      ? std::clamp(offset(), row.bottom - height_, row.top)
                      : (start ? row.top : row.bottom - height_);
        from_bottom_ = limit() - std::clamp(pos, 0, limit());
    }
    std::vector<MessageBounds> rows_;
    std::string selected_;
    int from_bottom_ = 0, height_ = 0;
    bool at_start_ = false;
};
} // namespace okou
