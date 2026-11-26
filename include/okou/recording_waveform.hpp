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

    struct Overview {
        Envelope peaks{};
        std::size_t samples = 0;
    };

    void append(const std::int16_t *samples, std::size_t count) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (std::size_t i = 0; i < count; ++i) {
            partial_.low = std::min(partial_.low, samples[i]);
            partial_.high = std::max(partial_.high, samples[i]);
            if (++frames_ == 256) {
                std::move(history_.begin() + 1, history_.end(), history_.begin());
                history_.back() = partial_;
                // Merge neighboring time bins when the recording outgrows the buffer.
                if (blocks_ == bins_.size() * blocks_per_bin_) {
                    for (std::size_t j = 0; j < bins_.size() / 2; ++j)
                        bins_[j] = combine(bins_[2 * j], bins_[2 * j + 1]);
                    blocks_per_bin_ *= 2;
                }
                auto &bin = bins_[blocks_ / blocks_per_bin_];
                bin = blocks_ % blocks_per_bin_ ? combine(bin, partial_) : partial_;
                ++blocks_;
                partial_ = {};
                frames_ = 0;
            }
        }
    }

    Envelope snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return history_;
    }

    Overview overview() const {
        std::lock_guard<std::mutex> lock(mutex_);
        Overview result;
        const auto complete = blocks_ * 256;
        result.samples = complete + frames_;
        if (!result.samples)
            return result;
        const auto width = blocks_per_bin_ * 256;
        for (std::size_t i = 0; i < result.peaks.size(); ++i) {
            const auto first = i * result.samples / result.peaks.size();
            const auto end = std::max(first + 1, (i + 1) * result.samples / result.peaks.size());
            for (auto j = first / width; j * width < std::min(end, complete); ++j)
                result.peaks[i] = combine(result.peaks[i], bins_[j]);
            if (end > complete)
                result.peaks[i] = combine(result.peaks[i], partial_);
        }
        return result;
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        blocks_ = 0;
        blocks_per_bin_ = 1;
        history_ = {};
        partial_ = {};
        frames_ = 0;
    }

  private:
    static Peaks combine(Peaks a, Peaks b) {
        return {std::min(a.low, b.low), std::max(a.high, b.high)};
    }
    std::array<Peaks, 96> bins_{};
    std::size_t blocks_ = 0, blocks_per_bin_ = 1;
    mutable std::mutex mutex_;
    Envelope history_{};
    Peaks partial_{};
    unsigned frames_ = 0;
};
} // namespace okou
