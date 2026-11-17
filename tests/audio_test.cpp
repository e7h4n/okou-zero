// SPDX-License-Identifier: MIT
#include "okou/core.hpp"
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
    // No start() call: these lifecycle checks must never open a capture device.
    okou::AudioCapture audio("not-opened-by-tests");
    check(!audio.recording(), "Constructor never records");
    check(audio.finish().empty(), "Finish before start is empty");
    audio.cancel();
    audio.cancel();
    check(!audio.recording() && audio.finish().empty(), "Cancel is idempotent");
    std::cout << "audio_test passed (synthetic WAV, no microphone opened)\n";
}
