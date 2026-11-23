// SPDX-License-Identifier: MIT
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>

namespace okou::detail {
inline int pcm16le(const std::uint8_t *bytes) {
    const int bits = bytes[0] | (static_cast<int>(bytes[1]) << 8);
    return bits < 32768 ? bits : bits - 65536;
}
inline int pcm_peak(const std::uint8_t *data, std::size_t frames) {
    int peak = 0;
    for (std::size_t i = 0; i < frames; ++i)
        peak = std::max(peak, std::abs(pcm16le(data + 2 * i)));
    return peak;
}
constexpr int playback_target_peak = 16384; // -6 dBFS
inline float playback_gain(int peak) {
    // Leave normal-level clips alone. Cap gain on very quiet recordings.
    return peak ? std::clamp(static_cast<float>(playback_target_peak) / peak, 1.0f, 32.0f) : 1.0f;
}
inline void apply_playback_gain(std::uint8_t *data, std::size_t frames, float gain) {
    if (gain == 1.0f)
        return;
    for (std::size_t i = 0; i < frames; ++i) {
        auto *sample = data + 2 * i;
        const int value = static_cast<int>(pcm16le(sample) * gain);
        const auto bits = static_cast<std::uint16_t>(value);
        sample[0] = static_cast<std::uint8_t>(bits);
        sample[1] = static_cast<std::uint8_t>(bits >> 8);
    }
}
} // namespace okou::detail
