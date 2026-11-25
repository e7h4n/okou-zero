// SPDX-License-Identifier: MIT
#pragma once
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <stdexcept>
#include <unistd.h>

namespace okou {
// /tmp is a RAM-backed filesystem on Cardputer. Keep temporary files on disk.
class DiskFile {
    char path_[sizeof("/var/tmp/okou-zero-XXXXXX")] = "/var/tmp/okou-zero-XXXXXX";
    int fd_ = -1;

  public:
    DiskFile() {
        fd_ = ::mkostemp(path_, O_CLOEXEC);
        if (fd_ < 0)
            throw std::runtime_error("Cannot create temporary file");
    }
    DiskFile(const DiskFile &) = delete;
    DiskFile &operator=(const DiskFile &) = delete;
    ~DiskFile() {
        if (fd_ >= 0)
            ::close(fd_);
        if (path_[0])
            ::unlink(path_);
    }
    std::FILE *stream() {
        auto *file = ::fdopen(fd_, "w+b");
        if (!file)
            throw std::runtime_error("Cannot open temporary file");
        fd_ = -1;
        if (::unlink(path_) == 0)
            path_[0] = '\0';
        return file;
    }
};

} // namespace okou
