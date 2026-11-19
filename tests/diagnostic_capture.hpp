// SPDX-License-Identifier: MIT
#pragma once
#include <cstdio>
#include <stdexcept>
#include <string>
#include <unistd.h>

// Capture the real stderr descriptor, not just a C++ stream replacement.
class DiagnosticCapture {
    FILE *file_ = nullptr;
    int saved_ = -1;

  public:
    DiagnosticCapture() {
        std::fflush(stderr);
        file_ = std::tmpfile();
        saved_ = ::dup(STDERR_FILENO);
        if (!file_ || saved_ < 0 || ::dup2(::fileno(file_), STDERR_FILENO) < 0) {
            if (file_)
                std::fclose(file_);
            if (saved_ >= 0)
                ::close(saved_);
            throw std::runtime_error("stderr capture failed");
        }
    }
    ~DiagnosticCapture() {
        std::fflush(stderr);
        ::dup2(saved_, STDERR_FILENO);
        ::close(saved_);
        std::fclose(file_);
    }
    DiagnosticCapture(const DiagnosticCapture &) = delete;
    DiagnosticCapture &operator=(const DiagnosticCapture &) = delete;
    std::string take() {
        std::fflush(stderr);
        std::rewind(file_);
        std::string out;
        char bytes[1024];
        while (auto n = std::fread(bytes, 1, sizeof bytes, file_))
            out.append(bytes, n);
        if (std::ferror(file_) || ::ftruncate(::fileno(file_), 0) != 0)
            throw std::runtime_error("stderr capture read failed");
        std::rewind(file_);
        return out;
    }
};
