#include "update_process.hpp"
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <linux/input.h>
#include <regex>
#include <sys/wait.h>
#include <unistd.h>

namespace okou {
bool UpdateShortcut::event(unsigned code, int value) {
    if (value != 0 && value != 1 && value != 2) return false;
    const bool down = value != 0;
    if (code == KEY_LEFTCTRL) left_ctrl_ = down;
    if (code == KEY_RIGHTCTRL) right_ctrl_ = down;
    if (code == KEY_LEFTALT) left_alt_ = down;
    if (code == KEY_RIGHTALT) right_alt_ = down;
    return code == KEY_U && value == 1 && (left_ctrl_ || right_ctrl_) && (left_alt_ || right_alt_);
}
UpdateProcess::~UpdateProcess() {
    // Checks are cancellable. Even during exception cleanup, do not interrupt
    // an explicitly confirmed package installation; the helper bounds its work.
    if (pid_ > 0) {
        if (!applying_) kill(-pid_, SIGKILL);
        while (waitpid(pid_, nullptr, 0) < 0 && errno == EINTR) {}
    }
    if (pipe_ >= 0) close(pipe_);
}
bool UpdateProcess::start(bool install, const std::string &version) {
    if (pid_ > 0 || (install && view_.status != UpdateStatus::ready)) return false;
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) < 0) { view_.status = UpdateStatus::failed; return false; }
    const auto child = fork();
    if (child == 0) {
        setpgid(0, 0);
        dup2(fds[1], STDOUT_FILENO);
        const int nullfd = open("/dev/null", O_RDWR);
        if (nullfd >= 0) { dup2(nullfd, STDIN_FILENO); dup2(nullfd, STDERR_FILENO); }
        close(fds[0]); close(fds[1]);
        execl("/usr/bin/python3", "python3", "-I", "/usr/lib/okou-zero/updater.py", install ? "apply" : "check", "--current", version.c_str(), static_cast<char *>(nullptr));
        _exit(127);
    }
    close(fds[1]);
    if (child < 0) { close(fds[0]); view_.status = UpdateStatus::failed; return false; }
    setpgid(child, child);
    pipe_ = fds[0];
    fcntl(pipe_, F_SETFL, fcntl(pipe_, F_GETFL) | O_NONBLOCK);
    pid_ = child;
    applying_ = install;
    started_ = std::chrono::steady_clock::now();
    buffer_.clear();
    result_ = {UpdateStatus::failed, {}};
    view_.status = install ? UpdateStatus::installing : UpdateStatus::checking;
    return true;
}
bool UpdateProcess::poll() {
    if (pid_ < 0) return false;
    const auto old = view_.status;
    const auto drain = [&]() {
    char bytes[256];
    ssize_t size;
    while ((size = read(pipe_, bytes, sizeof(bytes))) > 0) {
        buffer_.append(bytes, static_cast<std::size_t>(size));
        if (buffer_.size() > 4096) { kill(-pid_, SIGKILL); buffer_.clear(); break; }
        for (auto newline = buffer_.find('\n'); newline != std::string::npos; newline = buffer_.find('\n')) {
            const auto line = buffer_.substr(0, newline);
            buffer_.erase(0, newline + 1);
            if (!applying_ && line == "DOWNLOADING") view_.status = UpdateStatus::downloading;
            else if (!applying_ && line == "CURRENT") result_ = {UpdateStatus::current, {}};
            else if (!applying_ && line.rfind("READY ", 0) == 0 && line.size() < 48 &&
                     std::regex_match(line.substr(6), std::regex("[0-9]+\\.[0-9]+\\.[0-9]+")))
                result_ = {UpdateStatus::ready, line.substr(6)};
            else if (applying_ && line == "INSTALLED") result_ = {UpdateStatus::installed, {}};
            else if (line == "ERROR") result_ = {UpdateStatus::failed, {}};
        }
    }
    };
    drain();
    // Checks have a hard lifecycle bound. Confirmed dpkg work is owned by the
    // helper and not interrupted by a UI timeout or Escape key.
    if (!applying_ && std::chrono::steady_clock::now() - started_ > std::chrono::seconds(210)) kill(-pid_, SIGKILL);
    int status = 0;
    const auto done = waitpid(pid_, &status, WNOHANG);
    if (done == pid_ || (done < 0 && errno != EINTR)) {
        drain(); // Collect final output written between the first read and exit.
        view_ = (done == pid_ && WIFEXITED(status) && WEXITSTATUS(status) == 0) ? result_ : UpdateView{UpdateStatus::failed, {}};
        close(pipe_); pipe_ = -1; pid_ = -1;
    }
    return old != view_.status;
}
} // namespace okou
