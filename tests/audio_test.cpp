// SPDX-License-Identifier: MIT
#include "../src/playback_gain.hpp"
#include "okou/core.hpp"
#include "okou/recording_waveform.hpp"
#include <array>
#include <cstdlib>
#include <iostream>
#include <limits>

namespace {
void check(bool value, const char *message) {
    if (!value) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}
std::uint32_t read32(const std::vector<std::uint8_t> &b, std::size_t p) {
    return static_cast<std::uint32_t>(b.at(p)) | (static_cast<std::uint32_t>(b.at(p + 1)) << 8) |
           (static_cast<std::uint32_t>(b.at(p + 2)) << 16) |
           (static_cast<std::uint32_t>(b.at(p + 3)) << 24);
}
std::string tag(const std::vector<std::uint8_t> &b, std::size_t p) {
    return std::string(b.begin() + static_cast<std::ptrdiff_t>(p),
                       b.begin() + static_cast<std::ptrdiff_t>(p + 4));
}
} // namespace
int main() {
    const auto empty = okou::wav({});
    check(empty.size() == 44, "Empty WAV has a header");
    check(tag(empty, 0) == "RIFF" && tag(empty, 8) == "WAVE", "RIFF tags");
    check(read32(empty, 4) == 36 && read32(empty, 40) == 0, "Empty RIFF sizes");
    check(tag(empty, 12) == "fmt " && read32(empty, 16) == 16, "PCM fmt chunk");
    check(empty[20] == 1 && empty[21] == 0 && empty[22] == 1 && empty[23] == 0, "PCM mono");
    check(read32(empty, 24) == 16000 && read32(empty, 28) == 32000, "16k sample rate");
    check(empty[32] == 2 && empty[33] == 0 && empty[34] == 16 && empty[35] == 0,
          "16-bit alignment");
    check(tag(empty, 36) == "data", "Data chunk");
    const auto bytes = okou::wav({0, 1, -1, std::numeric_limits<std::int16_t>::min(),
                                  std::numeric_limits<std::int16_t>::max(), 0x1234});
    check(bytes.size() == 56 && read32(bytes, 4) == 48 && read32(bytes, 40) == 12,
          "WAV data sizes");
    const std::vector<std::uint8_t> expected{0, 0, 1, 0, 255, 255, 0, 128, 255, 127, 0x34, 0x12};
    check(std::vector<std::uint8_t>(bytes.begin() + 44, bytes.end()) == expected,
          "Signed little-endian samples");
    auto quiet = okou::wav({-383, 0, 200});
    auto *pcm = quiet.data() + 44;
    const auto gain = okou::detail::playback_gain(okou::detail::pcm_peak(pcm, 3));
    okou::detail::apply_playback_gain(pcm, 3, gain);
    check(okou::detail::pcm16le(pcm) == -12256 && okou::detail::pcm16le(pcm + 2) == 0 &&
              okou::detail::pcm16le(pcm + 4) == 6400,
          "Quiet speech amplified with one bounded gain");
    check(okou::detail::playback_gain(okou::detail::pcm_peak(expected.data(), 6)) == 1.0f &&
              okou::detail::playback_gain(0) == 1.0f,
          "Full-range PCM and silence stay unchanged");
    okou::RecordingWaveform wave;
    std::array<std::int16_t, 256> block{};
    block.front() = -32768;
    block.back() = 32767;
    wave.append(block.data(), 1);
    check(wave.snapshot().back().low == 0, "Incomplete wave column is held until ready");
    wave.append(block.data() + 1, 255);
    auto peaks = wave.snapshot();
    check(peaks.back().low == -32768 && peaks.back().high == 32767,
          "Both signed PCM extremes survive a split read");
    block.fill(0);
    for (std::size_t i = 1; i < peaks.size(); ++i)
        wave.append(block.data(), block.size());
    peaks = wave.snapshot();
    check(peaks.front().low == -32768 && peaks.back().low == 0 && peaks.back().high == 0,
          "Silence moves the previous peak across the display");
    wave.append(block.data(), block.size());
    check(wave.snapshot().front().high == 0, "Old PCM scrolls out of the bounded window");
    block.front() = -200;
    wave.append(block.data(), 1);
    wave.clear();
    block.front() = 0;
    wave.append(block.data(), block.size());
    check(wave.snapshot().back().low == 0, "A new capture discards the previous partial column");
    // No start() call: these lifecycle checks must never open a capture device.
    okou::AudioCapture audio("not-opened-by-tests");
    check(audio.finish().empty(), "Finish before start is empty");
    audio.cancel();
    audio.cancel();
    check(audio.finish().empty(), "Cancelled capture has no audio");
    std::cout << "audio_test passed (synthetic WAV, no microphone opened)\n";
}
