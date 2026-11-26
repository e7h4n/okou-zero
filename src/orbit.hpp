// SPDX-License-Identifier: MIT
#pragma once
#include "okou/platform.hpp"
#include "okou/recording_waveform.hpp"
#include <filesystem>

namespace okou {
enum class OrbitScene { welcome, pairing, ready, record, wait, bookmark, marker };

// A single persistent object carries the satellite between panels. Only its clipped
// drawing area is invalidated, and static states stop the timer after settling.
class PocketOrbit {
  public:
    PocketOrbit(lv_obj_t *root, const RecordingWaveform &microphone);
    ~PocketOrbit();
    PocketOrbit(const PocketOrbit &) = delete;
    PocketOrbit &operator=(const PocketOrbit &) = delete;
    void nudge();
    void sky(lv_obj_t *parent) const;
    void show(OrbitScene next, int marker_y = 80);
    const std::filesystem::path directory;

  private:
    struct Pose {
        double x, y, radius;
    };
    const RecordingWaveform &microphone_;
    std::string sky_path_, avatar_path_, blink_path_;
    lv_obj_t *object_;
    lv_timer_t *timer_;
    OrbitScene scene_ = OrbitScene::welcome;
    bool shown_ = false;
    int marker_y_ = 80;
    uint32_t entered_ = 0, nudged_ = 0;
    bool nudging_ = false;
    Pose from_{};
    Pose pose(uint32_t age) const;
    void draw(lv_event_t *event) const;
    bool continuous() const;
};
} // namespace okou
