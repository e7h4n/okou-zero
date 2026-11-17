// SPDX-License-Identifier: MIT
#include "okou/core.hpp"
#include <csignal>
#include <filesystem>
#include <iostream>
#include <netinet/in.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x))                                                                                  \
            throw std::runtime_error("check failed: " #x);                                         \
    } while (false)
namespace {
class Server {
    int fd_ = -1;
    std::thread thread_;

  public:
    std::string received;
    unsigned short port = 0;
    Server(std::string response, bool stall = false, SSL_CTX *tls = nullptr) {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        CHECK(fd_ >= 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        CHECK(::bind(fd_, reinterpret_cast<sockaddr *>(&addr), sizeof addr) == 0);
        CHECK(::listen(fd_, 4) == 0);
        socklen_t len = sizeof addr;
        CHECK(::getsockname(fd_, reinterpret_cast<sockaddr *>(&addr), &len) == 0);
        port = ntohs(addr.sin_port);
        thread_ = std::thread([this, response = std::move(response), stall, tls] {
            int client = ::accept(fd_, nullptr, nullptr);
            if (client < 0)
                return;
            timeval timeout{4, 0};
            ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
            SSL *ssl = nullptr;
            if (tls) {
                ssl = SSL_new(tls);
                SSL_set_fd(ssl, client);
                if (SSL_accept(ssl) != 1) {
                    SSL_free(ssl);
                    ::close(client);
                    return;
                }
            }
            std::string data;
            char block[4096];
            while (data.find("\r\n\r\n") == std::string::npos) {
                int n = ssl ? SSL_read(ssl, block, sizeof block)
                            : static_cast<int>(::recv(client, block, sizeof block, 0));
                if (n <= 0)
                    break;
                data.append(block, n);
            }
            auto end = data.find("\r\n\r\n");
            std::size_t length = 0;
            auto pos = data.find("Content-Length:");
            if (pos != std::string::npos) {
                try {
                    length = std::stoul(data.substr(pos + 15));
                } catch (...) {
                }
            }
            while (end != std::string::npos && data.size() < end + 4 + length) {
                int n = ssl ? SSL_read(ssl, block, sizeof block)
                            : static_cast<int>(::recv(client, block, sizeof block, 0));
                if (n <= 0)
                    break;
                data.append(block, n);
            }
            received = data;
            if (stall)
                std::this_thread::sleep_for(std::chrono::milliseconds(1500));
            for (std::size_t p = 0; p < response.size();) {
                auto n = ssl ? SSL_write(ssl, response.data() + p,
                                         static_cast<int>(
                                             std::min<std::size_t>(65536, response.size() - p)))
                             : static_cast<int>(::send(client, response.data() + p,
                                                       response.size() - p, MSG_NOSIGNAL));
                if (n <= 0)
                    break;
                p += static_cast<std::size_t>(n);
            }
            if (ssl) {
                SSL_shutdown(ssl);
                SSL_free(ssl);
            }
            ::close(client);
        });
    }
    std::string url(bool tls = false) const {
        return std::string(tls ? "https" : "http") + "://127.0.0.1:" + std::to_string(port) +
               "/fixture";
    }
    void join() {
        if (thread_.joinable())
            thread_.join();
    }
    ~Server() {
        ::shutdown(fd_, SHUT_RDWR);
        ::close(fd_);
        join();
    }
};
std::string reply(int status, const std::string &body, const std::string &extra = {}) {
    return "HTTP/1.1 " + std::to_string(status) +
           " Fixture\r\nConnection: close\r\nContent-Length: " + std::to_string(body.size()) +
           "\r\n" + extra + "\r\n" + body;
}
template <class F> okou::HttpError error(F f) {
    try {
        f();
    } catch (const okou::HttpError &e) {
        return e;
    }
    throw std::runtime_error("expected HttpError");
}
void tests() {
    std::signal(SIGPIPE, SIG_IGN);
    ::setenv("OKOU_ZERO_TEST_ALLOW_LOOPBACK_HTTP", "1", 1);
    okou::Http h;
    CHECK(okou::Http::encode("a +/&\n") == "a%20%2B%2F%26%0A");
    {
        Server s(reply(200, "{\"ok\":true}", "X-Case: value\r\n"));
        auto r = h.request("GET", s.url(), {}, "fixture-secret");
        CHECK(r.status == 200);
        CHECK(r.json()["ok"] == true);
        CHECK(r.headers.at("x-case") == "value");
        s.join();
        CHECK(s.received.find("X-Client-Type: OkouZero") != std::string::npos);
        CHECK(s.received.find("X-Client-Request-Id:") != std::string::npos);
        CHECK(s.received.find("iOS") == std::string::npos);
    }
    {
        Server s(reply(200, "{}"));
        h.request("GET", s.url());
        s.join();
        CHECK(s.received.find("Authorization:") == std::string::npos);
        CHECK(s.received.find("X-Client-Type:") == std::string::npos);
    }
    {
        Server s(reply(302, "", "Location: http://127.0.0.1:1/leak\r\n"));
        CHECK(h.request("GET", s.url(), {}, "secret").status == 302);
    }
    {
        Server s(reply(400, "{\"error\":\"authorization_pending\"}"));
        CHECK(h.request("POST", s.url()).json()["error"] == "authorization_pending");
    }
    {
        Server s(reply(503, "PRIVATE RESPONSE"));
        auto e = error([&] { h.request("POST", s.url(), "PRIVATE REQUEST", "SECRET"); });
        CHECK(e.uncertain);
        CHECK(e.status == 503);
        CHECK(std::string(e.what()).find("PRIVATE") == std::string::npos);
    }
    {
        Server s(reply(503, ""));
        CHECK(!error([&] { h.request("GET", s.url()); }).uncertain);
    }
    {
        Server s("");
        CHECK(error([&] { h.request("POST", s.url()); }).uncertain);
    }
    {
        Server s(reply(200, "not-json PRIVATE"));
        auto r = h.request("GET", s.url());
        auto e = error([&] { (void)r.json(); });
        CHECK(std::string(e.what()).find("PRIVATE") == std::string::npos);
    }
    {
        // A deterministic gzip stream containing {"gzip":true}, decoded by curl.
        const unsigned char raw[] = {0x1f, 0x8b, 0x08, 0x00, 0,    0,    0,    0,    0x02,
                                     0x03, 0xab, 0x56, 0x4a, 0xaf, 0xca, 0x2c, 0x50, 0xb2,
                                     0x2a, 0x29, 0x2a, 0x4d, 0xad, 0x05, 0x00, 0xf9, 0xea,
                                     0xf4, 0x36, 0x0d, 0,    0,    0};
        std::string body(reinterpret_cast<const char *>(raw), sizeof raw);
        Server s(reply(200, body, "Content-Encoding: gzip\r\n"));
        CHECK(h.request("GET", s.url()).json()["gzip"] == true);
    }
    {
        Server s(reply(200, std::string(16 * 1024 * 1024 + 1, 'a')));
        CHECK(!error([&] { h.request("GET", s.url()); }).uncertain);
    }
    {
        Server s(reply(200, "{}"));
        std::string binary("a\0b", 3);
        h.request("POST", s.url(), {}, {}, {},
                  {{"file", binary, "segment.wav", "audio/wav"},
                   {"options", "{}", {}, "application/json"}});
        s.join();
        CHECK(s.received.find(binary) != std::string::npos);
        CHECK(s.received.find("filename=\"segment.wav\"") != std::string::npos);
    }
    {
        std::atomic_bool cancelled{true};
        okou::Http hc(&cancelled);
        CHECK(!error([&] { hc.request("POST", "https://example.invalid"); }).uncertain);
        cancelled = false;
        Server s(reply(200, "{}"), true);
        std::thread cancel([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            cancelled = true;
        });
        auto e = error([&] { hc.request("POST", s.url()); });
        cancel.join();
        CHECK(e.uncertain);
        CHECK(std::string(e.what()) == "Request cancelled");
    }
    CHECK(
        !error([&] { h.request("GET", "http://example.invalid/private?secret=value"); }).uncertain);
    CHECK(
        std::string(error([&] { h.request("GET", "https://user:secret@example.invalid"); }).what())
            .find("secret") == std::string::npos);
    CHECK(!error([&] { h.request("GET", "file:///etc/passwd"); }).uncertain);
    CHECK(!error([&] {
               h.request("GET", "https://example.invalid", {}, "x\r\nX-Evil: 1");
           }).uncertain);
    ::unsetenv("OKOU_ZERO_TEST_ALLOW_LOOPBACK_HTTP");
    CHECK(!error([&] { h.request("GET", "http://127.0.0.1:1"); }).uncertain);
    ::setenv("OKOU_ZERO_TEST_ALLOW_LOOPBACK_HTTP", "1", 1);
    // Local TLS: untrusted self-signed cert rejected, explicit loopback CA accepted.
    char temp[] = "/tmp/okou-http-test-XXXXXX";
    CHECK(::mkdtemp(temp) != nullptr);
    std::string dir = temp;
    const std::string command =
        "openssl req -x509 -newkey rsa:2048 -nodes -keyout " + dir + "/key.pem -out " + dir +
        "/cert.pem -days 1 -subj /CN=127.0.0.1 -addext subjectAltName=IP:127.0.0.1 >/dev/null 2>&1";
    CHECK(std::system(command.c_str()) == 0);
    std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)> context(SSL_CTX_new(TLS_server_method()),
                                                              SSL_CTX_free);
    CHECK(context);
    CHECK(SSL_CTX_use_certificate_file(context.get(), (dir + "/cert.pem").c_str(),
                                       SSL_FILETYPE_PEM) == 1);
    CHECK(SSL_CTX_use_PrivateKey_file(context.get(), (dir + "/key.pem").c_str(),
                                      SSL_FILETYPE_PEM) == 1);
    {
        Server s(reply(200, "{}"), false, context.get());
        CHECK(!error([&] { h.request("GET", s.url(true)); }).uncertain);
    }
    ::setenv("OKOU_ZERO_TEST_CA_FILE", (dir + "/cert.pem").c_str(), 1);
    {
        Server s(reply(200, "{}"), false, context.get());
        CHECK(h.request("GET", s.url(true)).status == 200);
    }
    ::unsetenv("OKOU_ZERO_TEST_CA_FILE");
    std::filesystem::remove_all(dir);
}
} // namespace
int main() {
    try {
        tests();
        std::cout << "http_test passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
