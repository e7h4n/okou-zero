// SPDX-License-Identifier: MIT
#include "okou/core.hpp"
#include "okou/recording_waveform.hpp"
#include "playback_gain.hpp"
#include <algorithm>
#include <alsa/asoundlib.h>
#include <array>
#include <cstring>
#include <limits>
#include <pulse/simple.h>
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

void play_wav(std::FILE *file, const std::atomic_bool &paused, const std::atomic_bool &stop,
              PlaybackMeter &meter) {
    meter.reset();
    std::array<unsigned char, 44> header{};
    auto matches = [&](std::size_t offset, const char *text) {
        return std::memcmp(header.data() + offset, text, 4) == 0;
    };
    auto number = [&](std::size_t offset, unsigned count) {
        std::uint32_t value = 0;
        for (unsigned i = 0; i < count; ++i)
            value |= static_cast<std::uint32_t>(header[offset + i]) << (8 * i);
        return value;
    };
    if (std::fread(header.data(), 1, header.size(), file) != header.size() || !matches(0, "RIFF") ||
        !matches(8, "WAVE") || !matches(12, "fmt ") || number(16, 4) != 16 || number(20, 2) != 1 ||
        number(22, 2) != 1 || number(24, 4) != 16000 || number(34, 2) != 16 || !matches(36, "data"))
        throw std::runtime_error("Unsupported recording format");
    const auto bytes = number(40, 4);
    if (!bytes || bytes % 2 || bytes > 25U * 1024U * 1024U - header.size() ||
        std::fseek(file, 0, SEEK_END) != 0 ||
        std::ftell(file) != static_cast<long>(header.size() + bytes))
        throw std::runtime_error("Invalid recording length");

    if (std::fseek(file, static_cast<long>(header.size()), SEEK_SET) != 0)
        throw std::runtime_error("Could not read recording");
    int peak = 0;
    std::array<std::uint8_t, 8192> samples{};
    for (std::size_t remaining = bytes / 2; remaining;) {
        if (stop)
            return;
        const auto frames = std::min(remaining, samples.size() / 2);
        if (std::fread(samples.data(), 2, frames, file) != frames)
            throw std::runtime_error("Could not read recording");
        peak = std::max(peak, detail::pcm_peak(samples.data(), frames));
        if (peak >= detail::playback_target_peak)
            break;
        remaining -= frames;
    }
    const float gain = detail::playback_gain(peak);
    constexpr unsigned rate = 16000;
    const pa_sample_spec format{PA_SAMPLE_S16LE, rate, 1};
    constexpr auto automatic = std::numeric_limits<std::uint32_t>::max();
    const pa_buffer_attr buffer_config{rate * 2 / 10, rate * 2 / 20, automatic, automatic,
                                       automatic};
    std::unique_ptr<pa_simple, decltype(&pa_simple_free)> audio(
        pa_simple_new(nullptr, "Okou Zero", PA_STREAM_PLAYBACK, nullptr, "Voice message", &format,
                      nullptr, &buffer_config, nullptr),
        pa_simple_free);
    if (!audio)
        throw std::runtime_error("Cannot open system audio output");

    std::array<unsigned char, 640> buffer{};
    const auto total = static_cast<std::size_t>(bytes / 2);
    meter.duration_ms = static_cast<unsigned>(total / (rate / 1000));
    std::size_t written = 0;
    bool suspended = false;
    while (!stop) {
        if (paused) {
            if (!suspended) {
                const auto latency = pa_simple_get_latency(audio.get(), nullptr);
                if (latency != static_cast<pa_usec_t>(-1))
                    written -= std::min(written, pa_usec_to_bytes(latency, &format) / 2);
                if (pa_simple_flush(audio.get(), nullptr) < 0)
                    throw std::runtime_error("Cannot pause audio playback");
                suspended = true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
            continue;
        }
        suspended = false;
        if (written == total) {
            if (pa_simple_drain(audio.get(), nullptr) < 0)
                throw std::runtime_error("Audio playback failed");
            break;
        }
        const auto frames = std::min(total - written, buffer.size() / 2);
        if (std::fseek(file, static_cast<long>(header.size() + 2 * written), SEEK_SET) != 0 ||
            std::fread(buffer.data(), 2, frames, file) != frames)
            throw std::runtime_error("Could not read recording");
        std::array<std::int16_t, 320> samples{};
        for (std::size_t i = 0; i < frames; ++i)
            samples[i] = static_cast<std::int16_t>(detail::pcm16le(buffer.data() + 2 * i));
        detail::apply_playback_gain(buffer.data(), frames, gain);
        if (pa_simple_write(audio.get(), buffer.data(), frames * 2, nullptr) < 0)
            throw std::runtime_error("Audio playback failed");
        written += frames;
        meter.waveform.append(samples.data(), frames);
        meter.elapsed_ms = static_cast<unsigned>(written / (rate / 1000));
    }
}

struct AudioCapture::Impl {
    explicit Impl(std::string name) : device(std::move(name)) {}
    std::string device;
    std::mutex lifecycle;
    std::atomic_bool stop{false};
    std::thread worker;
    snd_pcm_t *handle = nullptr;
    std::vector<std::int16_t> samples;
    bool failed = false; // Worker writes; owner reads only after join.

    void loop(RecordingWaveform &waveform) noexcept {
        // ALSA S16_LE describes device bytes; decode explicitly for big-endian hosts.
        std::array<unsigned char, 1024> buffer{};
        while (!stop.load() && samples.size() < max_samples) {
            const auto frames = snd_pcm_readi(handle, buffer.data(), buffer.size() / 2);
            if (frames > 0) {
                const auto count = std::min({static_cast<std::size_t>(frames), buffer.size() / 2,
                                             max_samples - samples.size()});
                try {
                    for (std::size_t i = 0; i < count; ++i) {
                        samples.push_back(
                            static_cast<std::int16_t>(detail::pcm16le(buffer.data() + 2 * i)));
                    }
                    waveform.append(samples.data() + samples.size() - count, count);
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
        waveform.clear();
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
    }
};
AudioCapture::AudioCapture(std::string device) : impl_(std::make_unique<Impl>(std::move(device))) {}
AudioCapture::~AudioCapture() {
    cancel();
}
void AudioCapture::start(RecordingWaveform &waveform) {
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
        waveform.clear();
        impl_->worker = std::thread([p = impl_.get(), &waveform] { p->loop(waveform); });
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
} // namespace okou
