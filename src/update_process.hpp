#pragma once
#include <chrono>
#include <string>
#include <sys/types.h>

namespace okou {
enum class UpdateStatus { idle, checking, downloading, ready, installing, current, failed, installed };
struct UpdateView {
    UpdateStatus status = UpdateStatus::idle;
    std::string version;
};
class UpdateShortcut {
  public:
    bool event(unsigned code, int value);
  private:
    bool left_ctrl_ = false, right_ctrl_ = false;
    bool left_alt_ = false, right_alt_ = false;
};
class UpdateProcess {
  public:
    ~UpdateProcess();
    bool start(bool install, const std::string &version);
    bool poll();
    const UpdateView &view() const { return view_; }
    bool installing() const { return pid_ > 0 && applying_; }
  private:
    pid_t pid_ = -1;
    int pipe_ = -1;
    bool applying_ = false;
    std::string buffer_;
    UpdateView view_;
    UpdateView result_;
    std::chrono::steady_clock::time_point started_;
};
} // namespace okou
