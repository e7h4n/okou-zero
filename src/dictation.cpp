// SPDX-License-Identifier: MIT
#include "okou/dictation.hpp"
#include "okou/core.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace okou {
namespace {
constexpr std::size_t rate = 16000;
constexpr std::size_t window = 15 * rate;
constexpr std::size_t overlap = 2 * rate;
constexpr std::size_t voice_window = 8 * rate;
constexpr std::size_t voice_fallback = 5 * rate;
constexpr std::size_t max_samples = 10 * 60 * rate;

// Best-effort erasure of our owned buffers, not copies owned by callers/callbacks.
template <typename Buffer> void erase(Buffer &buffer) noexcept {
    if (!buffer.empty()) {
        volatile typename Buffer::value_type *p = buffer.data();
        for (std::size_t i = 0; i < buffer.size(); ++i)
            p[i] = 0;
    }
    Buffer{}.swap(buffer);
}
struct Slice {
    std::vector<std::int16_t> pcm;
    ~Slice() {
        erase(pcm);
    }
};
struct Processing {
    bool &flag;
    explicit Processing(bool &value) : flag(value) {
        flag = true;
    }
    ~Processing() {
        flag = false;
    }
};
} // namespace

Dictation::Dictation(std::string thread_id, std::vector<std::int16_t> pcm, Mode mode)
    : thread_id_(std::move(thread_id)), pcm_(std::move(pcm)),
      window_samples_(mode == Mode::push_to_talk ? voice_window : window), pending_(!pcm_.empty()) {
    if (pcm_.size() > max_samples) {
        erase(pcm_);
        throw std::runtime_error("Dictation exceeds the ten-minute limit");
    }
}

Dictation::~Dictation() {
    discard();
}

void Dictation::discard() noexcept {
    erase(pcm_);
    for (auto &segment : segments_)
        erase(segment);
    std::vector<std::string>{}.swap(segments_);
    next_offset_ = 0;
    pending_ = false;
}

std::string Dictation::partial_text() const {
    std::string text;
    for (const auto &segment : segments_) {
        if (segment.empty())
            continue;
        if (!text.empty())
            text += ' ';
        text += segment;
    }
    return text;
}

void Dictation::check_cancelled(const Cancelled &cancelled) {
    bool stop = !pending_;
    if (cancelled) {
        try {
            stop = cancelled() || stop;
        } catch (...) {
            stop = true;
        } // Fail closed if cancellation cannot be checked.
    }
    if (stop) {
        discard();
        throw std::runtime_error("Dictation cancelled");
    }
}

std::string Dictation::process(Transcribe transcribe, Polish polish, Cancelled cancelled) {
    if (processing_)
        throw std::runtime_error("Dictation is already processing");
    if (!pending_)
        return {}; // Empty capture, completed, or discarded: no callbacks.
    Processing processing(processing_);
    check_cancelled(cancelled);
    // Allocate bookkeeping before uploads, so committing a successful callback
    // cannot fail allocation and accidentally cause that window to be retried.
    const auto minimum_window = window_samples_ == window ? window : voice_fallback;
    const auto minimum_step = minimum_window - overlap;
    segments_.reserve(1 + (pcm_.size() + minimum_step - 1) / minimum_step);
    while (next_offset_ < pcm_.size()) {
        check_cancelled(cancelled);
        const auto start = next_offset_;
        const auto end = std::min(start + window_samples_, pcm_.size());
        // All of this suffix has already appeared in the preceding window.
        if (start != 0 && end - start <= overlap) {
            next_offset_ = pcm_.size();
            break;
        }
        Slice slice{{pcm_.begin() + static_cast<std::ptrdiff_t>(start),
                     pcm_.begin() + static_cast<std::ptrdiff_t>(end)}};
        const std::string empty;
        const std::string &previous = segments_.empty() ? empty : segments_.back();
        std::string transcript;
        try {
            check_cancelled(cancelled);
            transcript = transcribe(slice.pcm, previous, start == 0 ? 0.0 : 2.0);
        } catch (const HttpError &error) {
            check_cancelled(cancelled);
            if (error.status == 502 && window_samples_ == voice_window &&
                slice.pcm.size() > voice_fallback) {
                // No transcript was committed. Reprocess from the same offset
                // with less audio, retaining the previous successful context.
                window_samples_ = voice_fallback;
                continue;
            }
            throw std::runtime_error("Dictation transcription failed; retry explicitly");
        } catch (...) {
            check_cancelled(cancelled);
            throw std::runtime_error("Dictation transcription failed; retry explicitly");
        }
        try {
            check_cancelled(cancelled);
        } catch (...) {
            erase(transcript);
            throw;
        }
        segments_.push_back(std::move(transcript));
        // Commit progress only after a callback has returned successfully.
        next_offset_ = end == pcm_.size() ? end : end - overlap;
    }
    check_cancelled(cancelled);
    std::string result;
    try {
        result = polish(segments_);
    } catch (...) {
        check_cancelled(cancelled);
        throw std::runtime_error("Dictation polishing failed; retry explicitly");
    }
    try {
        check_cancelled(cancelled);
    } catch (...) {
        erase(result);
        throw;
    }
    discard();
    return result;
}

} // namespace okou
