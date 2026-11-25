#pragma once

#include "okou/platform.hpp"
#include <chrono>
#include <deque>
#include <optional>
#include <pulse/pulseaudio.h>

namespace okou {
// The UI thread pumps libpulse without blocking keyboard or recording updates.
class VolumeControl {
  public:
    ~VolumeControl();
    void press(Key key);
    bool pending() const {
        return !keys_.empty();
    }
    std::optional<VolumeStatus> poll();

  private:
    pa_mainloop *loop_ = nullptr;
    pa_context *context_ = nullptr;
    std::deque<Key> keys_;
    bool busy_ = false, failed_ = false, reading_back_ = false, unmute_ = false;
    uint32_t sink_ = PA_INVALID_INDEX;
    std::chrono::steady_clock::time_point deadline_;
    std::optional<VolumeStatus> result_;
    void close();
    void track(pa_operation *op);
    static void info(pa_context *, const pa_sink_info *, int, void *);
    static void applied(pa_context *, int, void *);
};
} // namespace okou
