// SPDX-License-Identifier: MIT
#include "okou/core.hpp"
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <limits>
#include <sys/stat.h>
#include <unistd.h>

namespace okou {
namespace detail {
namespace fs = std::filesystem;
std::string env(const char *key) {
    const char *v = std::getenv(key);
    return v ? v : "";
}
std::string home() {
    auto v = env("HOME");
    if (v.empty() || v[0] != '/')
        throw std::runtime_error("Absolute HOME directory required");
    return v;
}
std::string data_home() {
    auto d = env("XDG_DATA_HOME");
    if (d.empty() || d[0] != '/')
        d = home() + "/.local/share";
    return d + "/" + build::storage_name;
}
std::string config_path() {
    auto override = env("OKOU_ZERO_CONFIG");
    if (!override.empty())
        return override;
    auto d = env("XDG_CONFIG_HOME");
    if (d.empty() || d[0] != '/')
        d = home() + "/.config";
    return d + "/" + build::storage_name + "/config.json";
}
struct Fd {
    int fd = -1;
    explicit Fd(int f) : fd(f) {}
    ~Fd() {
        if (fd >= 0)
            ::close(fd);
    }
    Fd(const Fd &) = delete;
};
// Resolve every directory with O_NOFOLLOW. No symlink traversal, including when
// a supplied parent is replaced concurrently; all final operations use dirfds.
int directory(const fs::path &path, bool create) {
    const auto absolute = fs::absolute(path).lexically_normal();
    Fd current(::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (current.fd < 0)
        throw std::runtime_error("Cannot open private storage directory");
    for (const auto &component : absolute.relative_path()) {
        const std::string name = component.string();
        int next =
            ::openat(current.fd, name.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (next < 0 && errno == ENOENT && create) {
            if (::mkdirat(current.fd, name.c_str(), 0700) < 0 && errno != EEXIST)
                throw std::runtime_error("Cannot create private storage directory");
            next =
                ::openat(current.fd, name.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        }
        if (next < 0) {
            if (errno == ENOENT && !create)
                return -1;
            throw std::runtime_error("Unsafe or inaccessible storage directory");
        }
        ::close(current.fd);
        current.fd = next;
    }
    struct stat st {};
    if (::fstat(current.fd, &st) < 0 || st.st_uid != ::geteuid() || (st.st_mode & 0077))
        throw std::runtime_error(
            "Private storage directory must be owned by the user with mode 0700");
    int result = current.fd;
    current.fd = -1;
    return result;
}
Json read_private(const std::string &path) {
    try {
        auto p = fs::path(path);
        Fd dir(directory(p.parent_path().empty() ? "." : p.parent_path(), false));
        if (dir.fd < 0)
            return Json::object();
        Fd file(
            ::openat(dir.fd, p.filename().c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
        if (file.fd < 0) {
            if (errno == ENOENT)
                return Json::object();
            throw std::runtime_error("Cannot read private file");
        }
        struct stat st {};
        if (::fstat(file.fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_uid != ::geteuid() ||
            (st.st_mode & 0077) || st.st_nlink != 1 || st.st_size > 1024 * 1024)
            throw std::runtime_error("Unsafe private file ownership, permissions or size");
        std::string data;
        char block[4096];
        while (true) {
            auto n = ::read(file.fd, block, sizeof block);
            if (n < 0) {
                if (errno == EINTR)
                    continue;
                throw std::runtime_error("Cannot read private file");
            }
            if (!n)
                break;
            if (data.size() + static_cast<std::size_t>(n) > 1024 * 1024)
                throw std::runtime_error("Private file exceeds size limit");
            data.append(block, static_cast<std::size_t>(n));
        }
        auto j = Json::parse(data);
        if (!j.is_object())
            throw std::runtime_error("Invalid private configuration");
        return j;
    } catch (const Json::exception &) {
        throw std::runtime_error("Invalid private configuration JSON");
    } catch (const fs::filesystem_error &) {
        throw std::runtime_error("Cannot access private storage");
    }
}
void write_private(const std::string &path, const Json &value) {
    try {
        auto p = fs::path(path);
        Fd dir(directory(p.parent_path().empty() ? "." : p.parent_path(), true));
        std::string data = value.dump(2) + "\n";
        if (data.size() > 1024 * 1024)
            throw std::runtime_error("Private file exceeds size limit");
        // O_EXCL and mode 0600 are effective before any sensitive byte is written.
        std::string tmp;
        int raw = -1;
        for (int i = 0; i < 100; ++i) {
            tmp = ".okou-zero-" + std::to_string(::getpid()) + "-" + std::to_string(i);
            raw = ::openat(dir.fd, tmp.c_str(),
                           O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
            if (raw >= 0)
                break;
            if (errno != EEXIST)
                throw std::runtime_error("Cannot create private temporary file");
        }
        if (raw < 0)
            throw std::runtime_error("Cannot create private temporary file");
        Fd file(raw);
        try {
            std::size_t offset = 0;
            while (offset < data.size()) {
                auto n = ::write(file.fd, data.data() + offset, data.size() - offset);
                if (n < 0 && errno == EINTR)
                    continue;
                if (n <= 0)
                    throw std::runtime_error("Cannot save private file");
                offset += static_cast<std::size_t>(n);
            }
            if (::fsync(file.fd) < 0)
                throw std::runtime_error("Cannot synchronize private file");
            if (::renameat(dir.fd, tmp.c_str(), dir.fd, p.filename().c_str()) < 0)
                throw std::runtime_error("Cannot replace private file");
            if (::fsync(dir.fd) < 0)
                throw std::runtime_error("Cannot synchronize private directory");
        } catch (...) {
            ::unlinkat(dir.fd, tmp.c_str(), 0);
            throw;
        }
    } catch (const Json::exception &) {
        throw std::runtime_error("Cannot serialize private configuration");
    } catch (const fs::filesystem_error &) {
        throw std::runtime_error("Cannot access private storage");
    }
}
void remove_private(const std::string &path) {
    auto p = fs::path(path);
    Fd dir(directory(p.parent_path().empty() ? "." : p.parent_path(), false));
    if (dir.fd < 0)
        return;
    if (::unlinkat(dir.fd, p.filename().c_str(), 0) < 0 && errno != ENOENT)
        throw std::runtime_error("Cannot clear local credentials");
    if (::fsync(dir.fd) < 0)
        throw std::runtime_error("Cannot synchronize credential removal");
}
} // namespace detail
Config load_config(const std::string &path) {
    Config c;
    const auto j = detail::read_private(path.empty() ? detail::config_path() : path);
    auto set = [&](std::string &field, const char *key, const char *environment) {
        if (j.contains(key)) {
            if (!j[key].is_string())
                throw std::runtime_error("Configuration field must be a string");
            field = j[key].get<std::string>();
        }
        if (const char *value = std::getenv(environment))
            field = value;
        if (field.find('\0') != std::string::npos)
            throw std::runtime_error("Invalid configuration field");
    };
    set(c.api_origin, "api_origin", "OKOU_ZERO_API_ORIGIN");
    set(c.oauth_issuer, "oauth_issuer", "OKOU_ZERO_OAUTH_ISSUER");
    set(c.oauth_client_id, "oauth_client_id", "OKOU_ZERO_OAUTH_CLIENT_ID");
    set(c.oauth_resource, "oauth_resource", "OKOU_ZERO_OAUTH_RESOURCE");
    set(c.scopes, "scopes", "OKOU_ZERO_SCOPES");
    set(c.keyboard, "keyboard", "OKOU_ZERO_KEYBOARD");
    set(c.framebuffer, "framebuffer", "OKOU_ZERO_FRAMEBUFFER");
    set(c.capture_device, "capture_device", "OKOU_ZERO_CAPTURE_DEVICE");
    set(c.data_dir, "data_dir", "OKOU_ZERO_DATA_DIR");
    if (c.data_dir.empty())
        c.data_dir = detail::data_home();
    if (c.data_dir.empty() || c.data_dir[0] != '/')
        throw std::runtime_error("Absolute data directory required");
    return c;
}
void save_config(const Config &c) {
    detail::write_private(detail::config_path(), {{"api_origin", c.api_origin},
                                                  {"oauth_issuer", c.oauth_issuer},
                                                  {"oauth_client_id", c.oauth_client_id},
                                                  {"oauth_resource", c.oauth_resource},
                                                  {"scopes", c.scopes},
                                                  {"keyboard", c.keyboard},
                                                  {"framebuffer", c.framebuffer},
                                                  {"capture_device", c.capture_device},
                                                  {"data_dir", c.data_dir}});
}
} // namespace okou
