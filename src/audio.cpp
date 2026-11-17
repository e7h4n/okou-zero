// SPDX-License-Identifier: MIT
#include "okou/core.hpp"
#include <algorithm>
#include <alsa/asoundlib.h>
#include <array>
#include <limits>
#include <thread>

namespace okou {
namespace {
constexpr std::size_t max_samples = 16000U * 60U * 10U;
void le16(std::vector<std::uint8_t> &out, std::uint16_t n) {
    out.push_back(static_cast<std::uint8_t>(n));
    out.push_back(static_cast<std::uint8_t>(n >> 8));
}
void le32(std::vector<std::uint8_t> &out, std::uint32_t n) {
    le16(out, static_cast<std::uint16_t>(n));
    le16(out, static_cast<std::uint16_t>(n >> 16));
}
void literal(std::vector<std::uint8_t> &out, const char *s) {
    for (int i = 0; i < 4; ++i)
        out.push_back(static_cast<std::uint8_t>(s[i]));
}
void require_alsa(int result) {
    // Device names and driver errors are deliberately not placed in diagnostics.
    if (result < 0)
        throw std::runtime_error("ALSA capture configuration failed");
}
} // namespace
std::vector<std::uint8_t> wav(const std::vector<std::int16_t> &pcm) {
    constexpr auto limit = (std::numeric_limits<std::uint32_t>::max() - 36ULL) / 2ULL;
    if (pcm.size() > limit)
        throw std::length_error("PCM exceeds RIFF size limit");
    const auto bytes = static_cast<std::uint32_t>(pcm.size() * 2ULL);
    std::vector<std::uint8_t> out;
    out.reserve(44ULL + bytes);
    literal(out, "RIFF");
    le32(out, 36U + bytes);
    literal(out, "WAVE");
    literal(out, "fmt ");
    le32(out, 16);
    le16(out, 1);
    le16(out, 1);
    le32(out, 16000);
    le32(out, 32000);
    le16(out, 2);
    le16(out, 16);
    literal(out, "data");
    le32(out, bytes);
    for (auto sample : pcm)
        le16(out, static_cast<std::uint16_t>(sample));
    return out;
}

struct AudioCapture::Impl {
    explicit Impl(std::string name) : device(std::move(name)) {}
    std::string device;
    std::mutex lifecycle;
    std::atomic_bool stop{false}, active{false};
    std::thread worker;
    snd_pcm_t *handle = nullptr;
    std::vector<std::int16_t> samples;
    bool failed = false; // Worker writes; owner reads only after join.

    void loop() noexcept {
        // ALSA S16_LE describes device bytes; decode explicitly for big-endian hosts.
        std::array<unsigned char, 1024> buffer{};
        while (!stop.load() && samples.size() < max_samples) {
            const auto frames = snd_pcm_readi(handle, buffer.data(), buffer.size() / 2);
            if (frames > 0) {
                const auto count = std::min({static_cast<std::size_t>(frames), buffer.size() / 2,
                                             max_samples - samples.size()});
                try {
                    for (std::size_t i = 0; i < count; ++i) {
                        const auto u = static_cast<unsigned>(buffer[2 * i]) |
                                       (static_cast<unsigned>(buffer[2 * i + 1]) << 8);
                        const auto signed_value =
                            u < 32768U ? static_cast<int>(u) : static_cast<int>(u) - 65536;
                        samples.push_back(static_cast<std::int16_t>(signed_value));
                    }
                } catch (...) {
                    failed = true;
                    break;
                }
            } else if (frames == -EPIPE) {
                // Recover an overrun, but never use snd_pcm_recover's potentially
                // unbounded suspend/resume loop.
                if (snd_pcm_prepare(handle) < 0) {
                    failed = true;
                    break;
                }
            } else if (frames == -EAGAIN || frames == 0) {
                const auto ready = snd_pcm_wait(handle, 50);
                if (ready < 0 && ready != -EINTR && ready != -EPIPE) {
                    failed = true;
                    break;
                }
            } else if (frames != -EINTR) {
                failed = true;
                break;
            }
        }
        active.store(false);
    }
    void join() noexcept {
        stop.store(true);
        if (worker.joinable())
            worker.join();
        if (handle) {
            snd_pcm_drop(handle);
            snd_pcm_close(handle);
            handle = nullptr;
        }
        active.store(false);
    }
};
AudioCapture::AudioCapture(std::string device) : impl_(std::make_unique<Impl>(std::move(device))) {}
AudioCapture::~AudioCapture() {
    cancel();
}
void AudioCapture::start() {
    std::lock_guard<std::mutex> lock(impl_->lifecycle);
    if (impl_->worker.joinable())
        throw std::logic_error("Finish or cancel the previous recording first");
    if (impl_->device.empty())
        throw std::invalid_argument("Capture device is empty");
    impl_->samples.clear();
    impl_->failed = false;
    impl_->stop.store(false);
    // Reserve the exact cap, avoiding geometric growth beyond ten minutes of PCM.
    impl_->samples.reserve(max_samples);
    require_alsa(snd_pcm_open(&impl_->handle, impl_->device.c_str(), SND_PCM_STREAM_CAPTURE,
                              SND_PCM_NONBLOCK));
    try {
        snd_pcm_hw_params_t *raw = nullptr;
        require_alsa(snd_pcm_hw_params_malloc(&raw));
        std::unique_ptr<snd_pcm_hw_params_t, decltype(&snd_pcm_hw_params_free)> params(
            raw, snd_pcm_hw_params_free);
        require_alsa(snd_pcm_hw_params_any(impl_->handle, raw));
        require_alsa(
            snd_pcm_hw_params_set_access(impl_->handle, raw, SND_PCM_ACCESS_RW_INTERLEAVED));
        require_alsa(snd_pcm_hw_params_set_format(impl_->handle, raw, SND_PCM_FORMAT_S16_LE));
        require_alsa(snd_pcm_hw_params_set_channels(impl_->handle, raw, 1));
        require_alsa(snd_pcm_hw_params_set_rate(impl_->handle, raw, 16000, 0));
        snd_pcm_uframes_t period = 512, buffer = 4096;
        int direction = 0;
        require_alsa(
            snd_pcm_hw_params_set_period_size_near(impl_->handle, raw, &period, &direction));
        require_alsa(snd_pcm_hw_params_set_buffer_size_near(impl_->handle, raw, &buffer));
        require_alsa(snd_pcm_hw_params(impl_->handle, raw));
        require_alsa(snd_pcm_prepare(impl_->handle));
        impl_->active.store(true);
        impl_->worker = std::thread([p = impl_.get()] { p->loop(); });
    } catch (...) {
        impl_->join();
        throw;
    }
}
std::vector<std::int16_t> AudioCapture::finish() {
    std::lock_guard<std::mutex> lock(impl_->lifecycle);
    impl_->join();
    if (impl_->failed) {
        std::vector<std::int16_t>().swap(impl_->samples);
        throw std::runtime_error("ALSA recording failed; recording discarded");
    }
    auto result = std::move(impl_->samples);
    impl_->samples.clear();
    return result;
}
void AudioCapture::cancel() {
    std::lock_guard<std::mutex> lock(impl_->lifecycle);
    impl_->join();
    std::vector<std::int16_t>().swap(impl_->samples);
    impl_->failed = false;
}
bool AudioCapture::recording() const {
    return impl_->active.load();
}
} // namespace okou
