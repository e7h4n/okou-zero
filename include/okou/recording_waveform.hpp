// SPDX-License-Identifier: MIT
#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace okou {
// One min/max pair per 16 ms of the same mono 16 kHz PCM that goes into the message.
class RecordingWaveform {
  public:
    struct Peaks {
        std::int16_t low = 0, high = 0;
    };
    using Envelope = std::array<Peaks, 48>;

    void append(const std::int16_t *samples, std::size_t count) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (std::size_t i = 0; i < count; ++i) {
            partial_.low = std::min(partial_.low, samples[i]);
            partial_.high = std::max(partial_.high, samples[i]);
            if (++frames_ == 256) {
                std::move(history_.begin() + 1, history_.end(), history_.begin());
                history_.back() = partial_;
                partial_ = {};
                frames_ = 0;
            }
        }
    }

    Envelope snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return history_;
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        history_ = {};
        partial_ = {};
        frames_ = 0;
    }

  private:
    mutable std::mutex mutex_;
    Envelope history_{};
    Peaks partial_{};
    unsigned frames_ = 0;
};
} // namespace okou
