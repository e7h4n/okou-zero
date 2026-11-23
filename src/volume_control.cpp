#include "volume_control.hpp"
#include <algorithm>

namespace okou {
void VolumeControl::close() {
    if (context_) {
        pa_context_disconnect(context_);
        pa_context_unref(context_);
        context_ = nullptr;
    }
    if (loop_) {
        pa_mainloop_free(loop_);
        loop_ = nullptr;
    }
}
VolumeControl::~VolumeControl() {
    close();
}
void VolumeControl::track(pa_operation *operation) {
    if (operation)
        pa_operation_unref(operation);
    else
        failed_ = true;
}
void VolumeControl::press(Key key) {
    if (keys_.size() < 32)
        keys_.push_back(key);
    if (loop_)
        return;
    deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    loop_ = pa_mainloop_new();
    if (loop_)
        context_ = pa_context_new(pa_mainloop_get_api(loop_), "Okou Zero volume");
    failed_ =
        !context_ || pa_context_connect(context_, nullptr, PA_CONTEXT_NOAUTOSPAWN, nullptr) < 0;
}
void VolumeControl::info(pa_context *ctx, const pa_sink_info *sink, int end, void *data) {
    auto &self = *static_cast<VolumeControl *>(data);
    if (end < 0)
        self.failed_ = true;
    if (end || !sink)
        return;
    const auto raw = pa_cvolume_max(&sink->volume);
    int percent = static_cast<int>((uint64_t(raw) * 100 + PA_VOLUME_NORM / 2) / PA_VOLUME_NORM);
    if (self.reading_back_) {
        self.result_ = VolumeStatus{percent, sink->mute != 0};
        self.keys_.pop_front();
        self.busy_ = false;
        return;
    }
    self.sink_ = sink->index;
    const Key key = self.keys_.front();
    self.unmute_ = key != Key::mute && sink->mute;
    if (key == Key::mute) {
        self.track(pa_context_set_sink_mute_by_index(ctx, sink->index, !sink->mute, applied, data));
    } else {
        const int step = key == Key::volume_up ? 10 : -10;
        const int target = std::clamp(((percent + 5) / 10) * 10 + step, 0, 100);
        auto levels = sink->volume;
        pa_cvolume_scale(&levels, (target * PA_VOLUME_NORM + 50) / 100);
        self.track(pa_context_set_sink_volume_by_index(ctx, sink->index, &levels, applied, data));
    }
}
void VolumeControl::applied(pa_context *ctx, int success, void *data) {
    auto &self = *static_cast<VolumeControl *>(data);
    if (!success) {
        self.failed_ = true;
        return;
    }
    if (self.unmute_) {
        self.unmute_ = false;
        self.track(pa_context_set_sink_mute_by_index(ctx, self.sink_, false, applied, data));
        return;
    }
    self.reading_back_ = true;
    self.track(pa_context_get_sink_info_by_index(ctx, self.sink_, info, data));
}
std::optional<VolumeStatus> VolumeControl::poll() {
    if (keys_.empty())
        return std::nullopt;
    auto now = std::chrono::steady_clock::now();
    if (!failed_) {
        // Dispatch buffered responses in order, without waiting on the sound server.
        for (int i = 0; i < 8; ++i) {
            int n = pa_mainloop_iterate(loop_, 0, nullptr);
            if (n < 0)
                failed_ = true;
            if (n <= 0 || failed_)
                break;
        }
        auto state = pa_context_get_state(context_);
        if (!PA_CONTEXT_IS_GOOD(state))
            failed_ = true;
        if (!failed_ && state == PA_CONTEXT_READY && !busy_ && !keys_.empty()) {
            busy_ = true;
            reading_back_ = false;
            deadline_ = now + std::chrono::seconds(2);
            track(pa_context_get_sink_info_by_name(context_, "@DEFAULT_SINK@", info, this));
        }
    }
    if (failed_ || (!keys_.empty() && now >= deadline_)) {
        close();
        failed_ = busy_ = false;
        keys_.clear();
        result_ = VolumeStatus{};
    }
    auto result = result_;
    result_.reset();
    return result;
}
} // namespace okou
