// SPDX-License-Identifier: MIT
#pragma once
#include "okou/core.hpp"
#include <filesystem>
#include <fstream>
#include <utility>

namespace okou {
inline std::chrono::seconds launcher_dark_time(const std::filesystem::path &config) {
    std::ifstream file(config);
    if (file) {
        const auto settings = Json::parse(file, nullptr, false);
        if (settings.is_object() && settings.contains("dark_time")) {
            const auto &value = settings.at("dark_time");
            for (int seconds : {0, 10, 30, 60, 300})
                if (value == seconds)
                    return std::chrono::seconds(seconds);
        }
    }
    return std::chrono::seconds(30);
}

class IdleDisplay {
    using Clock = std::chrono::steady_clock;
    std::filesystem::path brightness_;
    std::chrono::seconds timeout_;
    Clock::time_point last_activity_ = Clock::now();
    int saved_ = -1;

    bool write(int value) const {
        std::ofstream file(brightness_);
        file << value;
        file.close();
        return bool(file);
    }

  public:
    IdleDisplay(std::filesystem::path brightness, std::chrono::seconds timeout)
        : brightness_(std::move(brightness)), timeout_(timeout) {}
    IdleDisplay(const IdleDisplay &) = delete;
    IdleDisplay &operator=(const IdleDisplay &) = delete;
    ~IdleDisplay() {
        wake();
    }
    bool asleep() const {
        return saved_ >= 0;
    }
    void wake() {
        last_activity_ = Clock::now();
        if (asleep() && write(saved_))
            saved_ = -1;
    }
    Clock::time_point deadline() const {
        return timeout_.count() && !asleep() ? last_activity_ + timeout_ : Clock::time_point::max();
    }
    void expire(Clock::time_point now) {
        if (now < deadline())
            return;
        int previous;
        if (std::ifstream(brightness_) >> previous && previous > 0 && write(0))
            saved_ = previous;
        else
            last_activity_ = now; // A missing driver must not turn the deadline into a busy loop.
    }
};
} // namespace okou
