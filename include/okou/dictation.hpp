// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace okou {

// In-memory recovery only. Serialize access, including discard(); cancellation
// from another thread should be exposed through the Cancelled callback instead.
// Processing starts on finish or explicit retry. Push-to-talk may shorten a rejected segment.
class Dictation {
  public:
    using Transcribe =
        std::function<std::string(const std::vector<std::int16_t> &, const std::string &, double)>;
    using Polish = std::function<std::string(const std::vector<std::string> &)>;
    using Cancelled = std::function<bool()>;

    enum class Mode { draft, push_to_talk };
    Dictation(std::string thread_id, std::vector<std::int16_t> pcm, Mode mode = Mode::draft);
    ~Dictation();
    Dictation(const Dictation &) = delete;
    Dictation &operator=(const Dictation &) = delete;
    Dictation(Dictation &&) = delete;
    Dictation &operator=(Dictation &&) = delete;

    const std::string &thread_id() const noexcept {
        return thread_id_;
    }
    bool pending() const noexcept {
        return pending_;
    }
    std::string partial_text() const;

    // Previous context is the immediately preceding successful segment (possibly
    // empty). Empty recognition is a success and is still passed to Polish.
    // Errors are sanitized; recoverable failures retain audio and segment progress.
    // Cancellation discards progress and throws. No clients, sends, or persistence.
    std::string process(Transcribe transcribe, Polish polish, Cancelled cancelled = {});
    void discard() noexcept;

  private:
    void check_cancelled(const Cancelled &cancelled);
    std::string thread_id_;
    std::vector<std::int16_t> pcm_;
    std::vector<std::string> segments_;
    std::size_t window_samples_;
    std::size_t next_offset_ = 0;
    bool pending_ = false;
    bool processing_ = false;
};

} // namespace okou
