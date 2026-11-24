// SPDX-License-Identifier: MIT
#include "okou/core.hpp"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <netinet/in.h>
#include <sstream>
#include <sys/socket.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

using okou::Json;
namespace {
void check(bool condition, const std::string &what) {
    if (!condition)
        throw std::runtime_error(what);
}
const std::string thread_id = "11111111-1111-4111-8111-111111111111";
const std::string agent = "22222222-2222-4222-8222-222222222222";
const std::string project = "33333333-3333-4333-8333-333333333333";
const std::string run = "44444444-4444-4444-8444-444444444444";
const std::string bearer = "secret-API-access-token";
std::string id(int n) {
    std::ostringstream s;
    s << "55555555-5555-4555-8555-";
    s.width(12);
    s.fill('0');
    s << n;
    return s.str();
}
struct Request {
    std::string method, target, body;
    std::map<std::string, std::string> headers;
};
struct Response {
    int status = 200;
    std::string body = "{}";
    bool drop = false;
};
using Step = std::function<Response(const Request &)>;
Response json(const Json &j, int status = 200) {
    return {status, j.dump(), false};
}
class Server {
    int socket_ = -1;
    std::thread worker_;
    std::vector<Step> steps_;
    std::exception_ptr error_;
    std::atomic<std::size_t> done_{0};
    std::atomic_bool stop_{false};
    unsigned short port_ = 0;
    static Request read_request(int fd) {
        std::string raw;
        char buf[4096];
        std::size_t split = std::string::npos;
        while ((split = raw.find("\r\n\r\n")) == std::string::npos) {
            auto n = ::recv(fd, buf, sizeof(buf), 0);
            check(n > 0, "request header read");
            raw.append(buf, static_cast<std::size_t>(n));
            check(raw.size() < 65536, "request headers bounded");
        }
        Request out;
        std::istringstream in(raw.substr(0, split));
        std::string version, line;
        in >> out.method >> out.target >> version;
        std::getline(in, line);
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            auto colon = line.find(':');
            if (colon == std::string::npos)
                continue;
            auto k = line.substr(0, colon), v = line.substr(colon + 1);
            std::transform(k.begin(), k.end(), k.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            while (!v.empty() && v.front() == ' ')
                v.erase(v.begin());
            out.headers[k] = v;
        }
        std::size_t size =
            out.headers.count("content-length") ? std::stoull(out.headers.at("content-length")) : 0;
        check(size < 2 * 1024 * 1024, "request body bounded");
        if (out.headers.count("expect")) {
            const std::string c = "HTTP/1.1 100 Continue\r\n\r\n";
            ::send(fd, c.data(), c.size(), MSG_NOSIGNAL);
        }
        out.body = raw.substr(split + 4);
        while (out.body.size() < size) {
            auto n = ::recv(fd, buf, sizeof(buf), 0);
            check(n > 0, "request body read");
            out.body.append(buf, static_cast<std::size_t>(n));
        }
        return out;
    }

  public:
    Server() {
        socket_ = ::socket(AF_INET, SOCK_STREAM, 0);
        check(socket_ >= 0, "socket");
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        check(::bind(socket_, reinterpret_cast<sockaddr *>(&a), sizeof(a)) == 0, "bind");
        socklen_t len = sizeof(a);
        check(::getsockname(socket_, reinterpret_cast<sockaddr *>(&a), &len) == 0, "getsockname");
        port_ = ntohs(a.sin_port);
        check(::listen(socket_, 16) == 0, "listen");
    }
    std::string origin() const {
        return "http://127.0.0.1:" + std::to_string(port_);
    }
    void start(std::vector<Step> steps) {
        steps_ = std::move(steps);
        worker_ = std::thread([this] {
            while (!stop_ && done_ < steps_.size()) {
                int fd = ::accept(socket_, nullptr, nullptr);
                if (fd < 0)
                    break;
                timeval timeout{3, 0};
                ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
                Response r;
                try {
                    auto req = read_request(fd);
                    r = steps_.at(done_)(req);
                } catch (...) {
                    error_ = std::current_exception();
                    r = {500, "{}", false};
                }
                if (!r.drop) {
                    std::string data =
                        "HTTP/1.1 " + std::to_string(r.status) +
                        " Test\r\nContent-Type: application/json\r\nContent-Length: " +
                        std::to_string(r.body.size()) + "\r\nConnection: close\r\n\r\n" + r.body;
                    std::size_t pos = 0;
                    while (pos < data.size()) {
                        auto n = ::send(fd, data.data() + pos, data.size() - pos, MSG_NOSIGNAL);
                        if (n <= 0)
                            break;
                        pos += static_cast<std::size_t>(n);
                    }
                }
                ::shutdown(fd, SHUT_RDWR);
                ::close(fd);
                ++done_;
            }
        });
    }
    void finish() {
        if (worker_.joinable())
            worker_.join();
        if (error_)
            std::rethrow_exception(error_);
        check(done_ == steps_.size(), "all expected requests consumed");
    }
    ~Server() {
        stop_ = true;
        ::shutdown(socket_, SHUT_RDWR);
        ::close(socket_);
        if (worker_.joinable())
            worker_.join();
    }
};
void api_request(const Request &r, const std::string &method, const std::string &target) {
    check(r.method == method, "method: " + target);
    check(r.target == target, "target expected " + target + ", got " + r.target);
    check(r.headers.count("authorization") && r.headers.at("authorization") == "Bearer " + bearer,
          "API bearer");
}
Step get(const std::string &path, const Json &body, int status = 200) {
    return [=](const Request &r) {
        api_request(r, "GET", path);
        return json(body, status);
    };
}
std::string base() {
    return "/api/chat-threads/" + thread_id;
}
Json row(int n, const std::string &type, Json payload = Json::object()) {
    return {{"id", id(n)},
            {"seqId", n},
            {"chatThreadId", thread_id},
            {"eventType", type},
            {"runId", run},
            {"revokesEventId", nullptr},
            {"contextType", type.rfind("input.", 0) == 0 ? Json("web") : Json(nullptr)},
            {"contextId", nullptr},
            {"runEventSequenceNumber", nullptr},
            {"runEventId", nullptr},
            {"createdAt", "2026-11-17T18:00:00Z"},
            {"payload", payload}};
}
Json tail(const Json &rows, int last, const std::string &last_id, bool more = false) {
    return {
        {"rows", rows},
        {"cursor",
         {{"lastSeqId", last}, {"lastEventId", last_id.empty() ? Json(nullptr) : Json(last_id)}}},
        {"hasMore", more}};
}
Json input(const std::string &text) {
    return {{"userMessage",
             {{"version", 1}, {"parts", Json::array({{{"type", "text"}, {"text", text}}})}}}};
}
std::string rows_path(int n, const std::string &eid = {}) {
    auto s = base() + "/event-rows?sinceSeqId=" + std::to_string(n) + "&limit=50";
    return n ? s + "&sinceEventId=" + eid : s;
}
Step missing_snapshot() {
    return get(
        base() + "/event-snapshot",
        {{"error", {{"code", "CHAT_EVENT_SNAPSHOT_NOT_FOUND"}, {"message", "Not yet archived"}}}},
        404);
}
struct Fixture {
    Server server;
    okou::Http http;
    okou::Config config;
    okou::Tokens tokens;
    Fixture() {
        config.api_origin = server.origin();
        char pattern[] = "/tmp/okou-client-test-XXXXXX";
        auto p = ::mkdtemp(pattern);
        check(p, "mkdtemp");
        config.data_dir = p;
        tokens.access_token = bearer;
        tokens.user_id = "user";
        tokens.org_id = "org";
    }
    okou::Client client() {
        return okou::Client(config, http, tokens);
    }
    ~Fixture() {
        std::filesystem::remove_all(config.data_dir);
    }
};
void list_test() {
    Fixture f;
    auto storage = f.server.origin() + "/storage/list?X-Signature=a%2Bb%2F%3D&part=1";
    Json metadata = {{"id", thread_id},      {"title", "Before"},      {"agentId", agent},
                     {"projectId", project}, {"sortAt", "2026-11-01"}, {"archived", false}};
    Json e = {{"id", id(11)},
              {"seqId", 11},
              {"kind", "renamed"},
              {"chatThreadId", thread_id},
              {"agentId", agent},
              {"title", "After"},
              {"createdAt", "2026-11-17"}};
    f.server.start({get("/api/chat-threads/snapshot",
                        {{"url", storage}, {"latestSeqId", 10}, {"latestEventId", id(10)}}),
                    [=](const Request &r) {
                        check(r.target == "/storage/list?X-Signature=a%2Bb%2F%3D&part=1",
                              "signed URL preserved");
                        check(!r.headers.count("authorization"), "no bearer to storage");
                        check(!r.headers.count("x-client-type"), "no client headers to storage");
                        return json({{"chatThreads", Json::array({metadata})}});
                    },
                    get("/api/chat-threads/events?sinceSeqId=10",
                        {{"events", Json::array({e})}, {"hasMore", true}}),
                    get("/api/chat-threads/events?sinceSeqId=11",
                        {{"events", Json::array({{{"id", id(12)},
                                                  {"seqId", 12},
                                                  {"kind", "sort_touched"},
                                                  {"chatThreadId", thread_id},
                                                  {"agentId", agent},
                                                  {"title", nullptr},
                                                  {"createdAt", "2026-11-18"}}})},
                         {"hasMore", false}}),
                    get("/api/indicators",
                        {{"threads", {{thread_id, "active"}}}, {"unreadAt", Json::object()}}),
                    get("/api/projects",
                        {{"projects", Json::array({{{"id", project}, {"name", "Project"}}})}})});
    auto result = f.client().list();
    check(result.threads.size() == 1 && result.threads[0].title == "After" &&
              result.threads[0].active,
          "ordering events preserve the title established by a rename");
    check(result.threads[0].project_id == project && result.projects.at(0).title == "Project",
          "projects");
    f.server.finish();
}
void metadata_only_test() {
    Fixture f;
    Json e = {
        {"id", id(1)},      {"seqId", 1},       {"kind", "created"}, {"chatThreadId", thread_id},
        {"agentId", agent}, {"title", "Fresh"}, {"muted", true},     {"createdAt", "2026-11-17"}};
    f.server.start(
        {get("/api/chat-threads/snapshot", {{"chatThreads", Json::array()},
                                            {"latestSeqId", nullptr},
                                            {"latestEventId", nullptr}}),
         get("/api/chat-threads/events", {{"events", Json::array({e})}, {"hasMore", false}}),
         get("/api/projects", {{"projects", Json::array()}}),
         get("/api/indicators", {{"threads", {{thread_id, "unread"}}},
                                 {"unreadAt", {{thread_id, "2026-11-17T18:00:00Z"}}}})});
    auto c = f.client();
    auto state = c.list(false);
    check(state.threads.size() == 1 && !state.threads[0].active && !state.threads[0].unread &&
              state.threads[0].muted,
          "metadata invalidation preserves mute without fetching indicators");
    check(c.indicators()["threads"][thread_id] == "unread", "independent indicator read");
    f.server.finish();
}
void large_list_test(bool project_only) {
    Fixture f;
    auto storage = f.server.origin() + "/storage/large-list";
    auto rows = Json::array();
    for (int i = 1000; i < 9000; ++i)
        rows.push_back({{"id", id(i)},
                        {"agentId", agent},
                        {"sortAt", std::to_string(i)},
                        {"title", "Thread " + std::to_string(i)},
                        {"projectId", i == 1001 ? Json(project) : Json(nullptr)},
                        {"pinnedAt", i == 1001 ? Json("2026-11-17") : Json(nullptr)},
                        {"hidden", i == 8999}});
    Json event = {{"id", id(11)},
                  {"seqId", 11},
                  {"kind", "sort_touched"},
                  {"chatThreadId", id(1000)},
                  {"createdAt", "9999"}};
    f.server.start({
        get("/api/chat-threads/snapshot",
            {{"url", storage}, {"latestSeqId", 10}, {"latestEventId", id(10)}}),
        [rows = std::move(rows)](const Request &r) {
            check(r.target == "/storage/large-list", "large snapshot URL");
            return json({{"chatThreads", rows}});
        },
        get("/api/chat-threads/events?sinceSeqId=10",
            {{"events", Json::array({event})}, {"hasMore", false}}),
        get("/api/projects", {{"projects", Json::array()}}),
    });
    auto result = f.client().list(false, project_only ? project : "");
    if (project_only) {
        check(result.threads.size() == 1 && result.threads[0].id == id(1001) &&
                  result.threads[0].pinned,
              "project window includes older pinned chats outside the global window");
    } else {
        check(result.threads.size() == 128, "large workspace only loads the visible window");
        check(result.threads[0].id == id(1000), "tail can promote a thread outside the window");
        check(result.threads[1].id == id(8998), "hidden thread excluded before applying window");
    }
    f.server.finish();
}
void history_test() {
    Fixture f;
    auto storage = f.server.origin() + "/storage/history?sig=keep%2Bsignature";
    auto a = row(1, "input.prompt", input("Hello")),
         b = row(2, "output.message", {{"content", "revoked text"}}),
         c = row(3, "output.message", {{"content", "Visible answer"}}),
         d = row(4, "control.revoke");
    c["createdAt"] = "2026-11-24T01:12:34.567Z";
    auto summer = row(6, "output.message", {{"content", "Summer reply"}});
    summer["createdAt"] = "2026-07-01T10:07:15+02:00";
    auto legacy = row(7, "output.message", {{"content", "Legacy reply"}});
    legacy.erase("createdAt");
    auto invalid_date = row(8, "output.message", {{"content", "Undated reply"}});
    invalid_date["createdAt"] = "2026-02-30T00:00:00Z";
    d["revokesEventId"] = id(2);
    auto end = row(5, "run.completed");
    f.server.start({get(base() + "/event-snapshot",
                        {{"url", storage}, {"lastSeqId", 3}, {"lastEventId", id(3)}}),
                    [=](const Request &r) {
                        check(!r.headers.count("authorization"), "history storage bearer absent");
                        check(r.target == "/storage/history?sig=keep%2Bsignature",
                              "history signature preserved");
                        return Response{200, a.dump() + "\n" + b.dump() + "\n" + c.dump() + "\n",
                                        false};
                    },
                    get(rows_path(3, id(3)),
                        tail(Json::array({d, end, summer, legacy, invalid_date}), 8, id(8))),
                    get(rows_path(8, id(8)), tail(Json::array(), 8, id(8)))});
    const char *zone = getenv("TZ");
    const std::string original_zone = zone ? zone : "";
    const bool had_zone = zone != nullptr;
    setenv("TZ", "America/Los_Angeles", 1);
    tzset();
    auto messages = f.client().history(thread_id);
    if (had_zone)
        setenv("TZ", original_zone.c_str(), 1);
    else
        unsetenv("TZ");
    tzset();
    check(messages.size() == 5, "Revoked and successful completion rows hidden");
    check(messages[0].role == "user" && messages[0].text == "Hello", "user presentation");
    check(messages[1].text == "Visible answer" && messages[1].terminal,
          "assistant terminal evidence");
    check(messages[0].local_sent_at.empty() && messages[1].local_sent_at == "2026-11-23 17:12",
          "archive timestamp in Pacific standard time");
    check(messages[2].local_sent_at == "2026-07-01 01:07",
          "tail timestamp with offset in Pacific daylight time");
    check(messages[3].local_sent_at.empty() && messages[4].local_sent_at.empty(),
          "missing and invalid timestamps preserve replies");
    f.server.finish();
}
template <class Fn> void throws(Fn fn, const std::string &what) {
    bool threw = false;
    try {
        fn();
    } catch (const std::exception &) {
        threw = true;
    }
    check(threw, what);
}
void bad_history_tests() {
    for (int mode = 0; mode < 6; ++mode) {
        Fixture f;
        auto a = row(1, "input.prompt", input("x"));
        Json rows = Json::array({a});
        auto reply = tail(rows, 1, id(1));
        if (mode == 0)
            reply["rows"][0]["chatThreadId"] = agent;
        if (mode == 1)
            reply["cursor"]["lastSeqId"] = 2;
        if (mode == 2)
            reply["rows"].push_back(a);
        if (mode == 3) {
            reply["rows"] = Json::array();
            reply["hasMore"] = true;
            reply["cursor"] = {{"lastSeqId", 0}, {"lastEventId", nullptr}};
        }
        if (mode == 4)
            reply["cursor"]["lastEventId"] = nullptr;
        if (mode == 5)
            reply["rows"][0]["seqId"] = -1;
        f.server.start({missing_snapshot(), get(rows_path(0), reply)});
        throws([&] { f.client().history(thread_id); }, "malformed history rejected");
        f.server.finish();
    }
    {
        Fixture f;
        f.server.start({missing_snapshot(), get(rows_path(0), {{"code", "EXPIRED"}}, 410),
                        missing_snapshot(), get(rows_path(0), {{"code", "EXPIRED"}}, 410)});
        throws([&] { f.client().history(thread_id); }, "410 recovery bounded");
        f.server.finish();
    }
    {
        Fixture f;
        f.server.start({missing_snapshot(), get(rows_path(0), {}, 410), missing_snapshot(),
                        get(rows_path(0), tail(Json::array(), 0, "")),
                        get(rows_path(0), tail(Json::array(), 0, ""))});
        check(f.client().history(thread_id).empty(), "410 replaced baseline succeeds");
        f.server.finish();
    }
    for (int mode = 0; mode < 3; ++mode) {
        Fixture f;
        auto storage = f.server.origin() + "/storage/bad";
        auto a = row(1, "input.prompt", input("x"));
        auto b = row(2, "output.message", {{"content", "y"}});
        if (mode == 1)
            b["id"] = a["id"];
        auto payload = a.dump() + "\n" + (mode == 1 ? b.dump() + "\n" : "");
        if (mode == 2)
            payload.pop_back();
        f.server.start(
            {get(base() + "/event-snapshot", {{"url", storage},
                                              {"lastSeqId", mode == 2 ? 1 : 2},
                                              {"lastEventId", mode == 2 ? id(1) : id(2)}}),
             [=](const Request &) { return Response{200, payload, false}; }});
        throws([&] { f.client().history(thread_id); },
               "snapshot cursor, duplicate identity, or newline invalid");
        f.server.finish();
    }
    {
        Fixture f;
        auto a = row(1, "output.message", {{"content", "answer"}}), b = row(2, "run.failed");
        b["runId"] = agent;
        f.server.start({missing_snapshot(), get(rows_path(0), tail(Json::array({a, b}), 2, id(2))),
                        get(rows_path(2, id(2)), tail(Json::array(), 2, id(2)))});
        auto m = f.client().history(thread_id);
        check(m.size() == 1 && !m[0].terminal, "unrelated terminal ignored");
        f.server.finish();
    }
    {
        Fixture f;
        auto a = row(1, "output.message",
                     {{"content", "{\"tool\":\"approval\",\"secret\":\"do-not-show\"}"}});
        f.server.start({missing_snapshot(), get(rows_path(0), tail(Json::array({a}), 1, id(1))),
                        get(rows_path(1, id(1)), tail(Json::array(), 1, id(1)))});
        auto m = f.client().history(thread_id);
        check(m.at(0).text.find("Okou Web") != std::string::npos &&
                  m[0].text.find("do-not-show") == std::string::npos,
              "structured action safe review");
        f.server.finish();
    }
}
Json metadata() {
    return {{"id", thread_id}, {"computerUseHostId", nullptr}, {"cloudBrowserEnabled", true}};
}
okou::Thread thread() {
    okou::Thread t;
    t.id = thread_id;
    t.agent_id = agent;
    return t;
}
void fixed_thread_test() {
    Fixture f;
    std::string original, replacement;
    const auto create = [&](std::string &saved, int status) {
        return [&, status](const Request &r) {
            api_request(r, "POST", "/api/chat-threads");
            auto request = Json::parse(r.body);
            const auto candidate = request.at("clientThreadId").get<std::string>();
            if (saved.empty())
                saved = candidate;
            check(candidate == saved, "Retries reuse the reserved chat ID");
            return json({{"id", candidate}, {"title", "Cardputer Zero"}}, status);
        };
    };
    const auto open = [&](int status) {
        return [&, status](const Request &r) {
            api_request(r, "GET", "/api/chat-threads/" + original + "/metadata");
            return json({{"id", original}, {"title", "Renamed"}}, status);
        };
    };
    f.server.start({create(original, 503), create(original, 201), open(200), open(503), open(404),
                    create(replacement, 201)});
    throws([&] { f.client().fixed_thread(); }, "Transient create failure surfaces");
    check(f.client().fixed_thread().id == original, "Reserved ID persisted before create");
    check(f.client().fixed_thread().title == "Renamed", "Launch opens the existing fixed chat");
    throws([&] { f.client().fixed_thread(); }, "A service outage does not create another chat");
    check(f.client().fixed_thread().id == replacement && replacement != original,
          "An authoritative miss replaces the fixed chat");
    f.server.finish();
}
void send_test() {
    Fixture f;
    std::string sent_id, sort;
    const auto prompt = "private prompt never persisted";
    f.server.start({get(base() + "/metadata", metadata()),
                    [&](const Request &r) {
                        api_request(r, "POST", "/api/chat/events");
                        check(r.headers.at("content-type") == "application/json",
                              "JSON content type");
                        auto j = Json::parse(r.body);
                        sent_id = j.at("clientEventId");
                        sort = j.at("chatThreadSortEventId");
                        check(j.at("prompt") == prompt && j.at("threadId") == thread_id &&
                                  !j.contains("agentId"),
                              "send canonical body");
                        check(j["userMessage"]["version"] == 1 &&
                                  j["userMessage"]["parts"][0]["type"] == "text" &&
                                  j["hasTextContent"] == true,
                              "versioned user message schema");
                        check(j["computerUseHostId"].is_null() && j["cloudBrowserEnabled"] == true,
                              "metadata settings preserved");
                        return Response{200, "", true};
                    },
                    missing_snapshot(),
                    [&](const Request &r) {
                        api_request(r, "GET", rows_path(0));
                        auto a = row(1, "input.prompt", input("accepted"));
                        a["id"] = sent_id;
                        return json(tail(Json::array({a}), 1, sent_id));
                    },
                    [&](const Request &r) {
                        api_request(r, "GET", rows_path(1, sent_id));
                        return json(tail(Json::array(), 1, sent_id));
                    }});
    auto c = f.client();
    auto first = c.send(thread(), prompt);
    check(first.uncertain && !first.client_event_id.empty(), "dropped write uncertain");
    std::size_t files = 0;
    for (const auto &e : std::filesystem::directory_iterator(f.config.data_dir)) {
        ++files;
        std::ifstream in(e.path());
        std::string contents((std::istreambuf_iterator<char>(in)), {});
        check(contents.find(prompt) == std::string::npos &&
                  contents.find(bearer) == std::string::npos,
              "no prompt/token persisted");
        check(contents.find(sent_id) != std::string::npos &&
                  contents.find(sort) != std::string::npos,
              "both identities persisted");
        struct stat st {};
        ::stat(e.path().c_str(), &st);
        check((st.st_mode & 077) == 0, "pending mode private");
    }
    check(files == 1, "one pending record");
    // Reconstruct client (process-restart equivalent); another send only reconciles.
    auto reopened = f.client();
    check(reopened.has_pending_send(thread_id), "pending survives restart");
    reopened.history(thread_id);
    check(!reopened.has_pending_send(thread_id), "durable identity reconciliation");
    check(std::filesystem::is_empty(f.config.data_dir), "confirmed pending removed");
    f.server.finish();
}
void send_identity_test() {
    Fixture f;
    std::string pending;
    f.server.start({get(base() + "/metadata", metadata()),
                    [&](const Request &r) {
                        api_request(r, "POST", "/api/chat/events");
                        pending = Json::parse(r.body).at("clientEventId");
                        return json({{"threadId", agent}});
                    },
                    missing_snapshot(),
                    [&](const Request &r) {
                        api_request(r, "GET", rows_path(0));
                        auto a = row(1, "input.prompt", input("accepted"));
                        a["id"] = pending;
                        return json(tail(Json::array({a}), 2, id(2)));
                    }});
    auto c = f.client();
    auto result = c.send(thread(), "identity check");
    check(result.uncertain && c.has_pending_send(thread_id),
          "wrong success thread remains uncertain");
    throws([&] { c.history(thread_id); }, "matching input with bad final cursor rejected");
    check(c.has_pending_send(thread_id), "partial malformed read never clears pending");
    f.server.finish();
}
void assistant_content_test() {
    Fixture f;
    const std::string markdown = "[Documentation](https://example.com) and **plain Markdown**";
    const std::string json_answer = "{\"answer\":42,\"type\":1}";
    auto blocks = Json::array({{{"type", "text"}, {"text", "Public text block"}},
                               {{"type", "tool_use"},
                                {"name", "permission"},
                                {"input", {{"secret", "must-not-display"}}}}})
                      .dump();
    auto message =
        Json({{"role", "assistant"},
              {"content",
               Json::array({{{"type", "text"}, {"text", "Envelope public text"}},
                            {{"type", "tool_result"}, {"content", "must-not-display"}}})}})
            .dump();
    auto rows = Json::array(
        {row(1, "output.message", {{"content", markdown}}),
         row(2, "output.message", {{"content", json_answer}}),
         row(3, "output.message", {{"content", blocks}}),
         row(4, "output.message", {{"content", message}}),
         row(5, "output.message",
             {{"content",
               "Approve "
               "[access](https://app.okou.ai/connectors/example/permissions?request=id)"}})});
    f.server.start({missing_snapshot(), get(rows_path(0), tail(rows, 5, id(5))),
                    get(rows_path(5, id(5)), tail(Json::array(), 5, id(5)))});
    auto messages = f.client().history(thread_id);
    check(messages.size() == 5, "all public assistant replies retained");
    check(messages[0].text == markdown && messages[1].text == json_answer,
          "canonical Markdown and ordinary JSON retained");
    check(messages[2].text.find("Public text block") != std::string::npos &&
              messages[2].text.find("Okou Web") != std::string::npos,
          "allowlisted text blocks plus unsupported guidance");
    check(messages[3].text.find("Envelope public text") != std::string::npos,
          "allowlisted envelope text");
    for (const auto &m : messages)
        check(m.text.find("must-not-display") == std::string::npos,
              "tool arguments never displayed");
    check(messages[4].text.find("cannot approve") != std::string::npos,
          "plain action link web guidance");
    f.server.finish();
}
void send_rejection_test() {
    Fixture f;
    f.server.start({get(base() + "/metadata", metadata()),
                    [](const Request &r) {
                        api_request(r, "POST", "/api/chat/events");
                        return json({{"error", {{"code", "BAD_INPUT"}}}}, 400);
                    },
                    get(base() + "/metadata", metadata()),
                    [](const Request &r) {
                        api_request(r, "POST", "/api/chat/events");
                        return json({{"error", {{"code", "CONFLICT"}}}}, 409);
                    }});
    auto c = f.client();
    throws([&] { c.send(thread(), "rejected"); }, "known rejection surfaces");
    check(!c.has_pending_send(thread_id), "known rejection clears reservation");
    check(c.send(thread(), "conflict").uncertain && c.has_pending_send(thread_id),
          "conflict preserves uncertainty");
    f.server.finish();
}
void uncertain_absence_test() {
    Fixture f;
    std::string pending;
    f.server.start({get(base() + "/metadata", metadata()),
                    [&](const Request &r) {
                        pending = Json::parse(r.body).at("clientEventId");
                        return json({}, 503);
                    },
                    missing_snapshot(), get(rows_path(0), tail(Json::array(), 0, "")),
                    get(rows_path(0), tail(Json::array(), 0, ""))});
    auto first = f.client().send(thread(), "hello");
    f.client().history(thread_id);
    auto again = f.client().send(thread(), "hello");
    check(first.uncertain && again.uncertain && again.client_event_id == pending &&
              f.client().has_pending_send(thread_id),
          "absence does not authorize resend");
    f.server.finish();
}
std::map<std::string, std::string> multipart(const Request &r) {
    auto type = r.headers.at("content-type");
    auto pos = type.find("boundary=");
    check(pos != std::string::npos, "multipart boundary");
    auto boundary = "--" + type.substr(pos + 9);
    std::map<std::string, std::string> parts;
    std::size_t p = 0;
    while ((p = r.body.find(boundary, p)) != std::string::npos) {
        p += boundary.size();
        if (r.body.substr(p, 2) == "--")
            break;
        auto split = r.body.find("\r\n\r\n", p);
        check(split != std::string::npos, "multipart headers");
        auto header = r.body.substr(p, split - p);
        auto namepos = header.find("name=\"");
        check(namepos != std::string::npos, "part name");
        auto end = header.find('"', namepos + 6);
        auto name = header.substr(namepos + 6, end - namepos - 6);
        auto next = r.body.find("\r\n" + boundary, split + 4);
        check(next != std::string::npos, "multipart end");
        parts[name] = r.body.substr(split + 4, next - split - 4);
        p = next + 2;
    }
    return parts;
}
void voice_history_test() {
    Fixture f;
    const std::string clip = "aaaaaaaa-1111-4000-8000-111111111111";
    auto parts = Json::array({{{"type", "voice"}, {"clips", Json::array({{{"fileId", clip}}})}},
                              {{"type", "source"}, {"hidden", "source metadata"}},
                              {{"type", "additional_info"}, {"text", "hidden context"}},
                              {{"type", "model"}, {"selectedModel", "hidden model"}}});
    const Json payload = {{"userMessage", {{"parts", parts}}}};
    auto queued = row(1, "input.prompt", payload);
    queued["runId"] = nullptr;
    auto promoted = row(2, "input.prompt", payload);
    promoted["revokesEventId"] = id(1);
    // An independent send of the same clip remains a separate user message.
    auto rows = Json::array({queued, promoted, row(3, "input.prompt", payload)});
    const auto url = f.server.origin() + "/storage/voice?signature=private";
    f.server.start({missing_snapshot(), get(rows_path(0), tail(rows, 3, id(3))),
                    get(rows_path(3, id(3)), tail(Json::array(), 3, id(3))),
                    get("/api/voice/clips/" + clip + "/playback", {{"url", url}})});
    auto client = f.client();
    auto messages = client.history(thread_id);
    check(messages.size() == 2 && messages[0].id == id(1) && messages[1].id == id(3) &&
              messages[0].text == "[Voice message]" &&
              messages[0].voice_clips == std::vector<std::string>{clip},
          "Promoted voice replaces its placeholder and retains playback and the original send ID");
    check(client.voice_playback_url(clip) == url, "Preserves the signed playback URL");
    f.server.finish();
}
void voice_message_test() {
    Fixture f;
    const std::string file_id = "aaaaaaaa-1111-4000-8000-111111111111";
    const auto upload_url = f.server.origin() + "/storage/voice?signature=a%2Bb";
    f.server.start({
        [&](const Request &r) {
            api_request(r, "POST", "/api/uploads/prepare/private");
            check(Json::parse(r.body) == Json({{"filename", "recording.wav"},
                                               {"contentType", "audio/wav"},
                                               {"size", 48}}),
                  "prepare private WAV");
            return json({{"id", file_id},
                         {"uploadUrl", upload_url},
                         {"uploadHeaders", {{"x-amz-meta-test", "signed"}}}});
        },
        [](const Request &r) {
            check(r.method == "PUT" && r.target == "/storage/voice?signature=a%2Bb",
                  "PUT preserves signed URL");
            check(!r.headers.count("authorization") && !r.headers.count("x-client-type"),
                  "OAuth credentials stay on the API origin");
            check(r.headers.at("content-type") == "audio/wav" &&
                      r.headers.at("x-amz-meta-test") == "signed" && r.body.size() == 48 &&
                      r.body.substr(0, 4) == "RIFF",
                  "signed headers and WAV body");
            return json({});
        },
        [&](const Request &r) {
            api_request(r, "POST", "/api/uploads/complete");
            check(Json::parse(r.body).at("id") == file_id, "complete uploaded file");
            return json({{"id", file_id}});
        },
        get(base() + "/metadata", metadata()),
        [&](const Request &r) {
            api_request(r, "POST", "/api/chat/events");
            auto body = Json::parse(r.body);
            const Json expected = {{"type", "voice"},
                                   {"clips", Json::array({{{"fileId", file_id}}})}};
            check(body.at("threadId") == thread_id && body.at("hasTextContent") == false &&
                      body.at("prompt") == "(see message parts)" &&
                      body.at("userMessage") ==
                          Json({{"version", 1}, {"parts", Json::array({expected})}}),
                  "send a voice-only message to the fixed chat");
            return json({{"threadId", thread_id}});
        },
    });
    auto c = f.client();
    auto uploaded = c.upload_voice({42, -42});
    check(uploaded == file_id && !c.send_voice(thread(), uploaded).uncertain,
          "voice upload accepted by chat");
    f.server.finish();
}
void voice_test() {
    Fixture f;
    std::vector<std::int16_t> pcm(16000, 42);
    std::string previous(1100, 'a');
    f.server.start({get("/api/voice-io/quota", {{"allowed", true}, {"count", 1}}),
                    [&](const Request &r) {
                        api_request(r, "POST", "/api/voice-io/transcribe/segment");
                        auto parts = multipart(r);
                        check(parts.size() == 3, "required voice parts");
                        auto options = Json::parse(parts["options"]);
                        check(options["previousTranscript"].get<std::string>().size() == 1000,
                              "previous suffix capped");
                        check(options["overlapDurationSeconds"] == 0.5 &&
                                  options["totalDurationSeconds"] == 1.0,
                              "voice durations");
                        check(Json::parse(parts["editorContext"]) ==
                                  Json({{"before", ""}, {"selected", ""}, {"after", ""}}),
                              "required editorContext JSON");
                        auto &bytes = parts["file"];
                        check(bytes.size() == 32044 && bytes.substr(0, 4) == "RIFF" &&
                                  bytes.substr(8, 4) == "WAVE",
                              "WAV multipart binary");
                        check(static_cast<unsigned char>(bytes[22]) == 1 &&
                                  static_cast<unsigned char>(bytes[24]) == 0x80 &&
                                  static_cast<unsigned char>(bytes[25]) == 0x3e &&
                                  static_cast<unsigned char>(bytes[34]) == 16,
                              "16k mono PCM16");
                        check(r.body.find("filename=\"segment.wav\"") != std::string::npos &&
                                  r.body.find("audio/wav") != std::string::npos,
                              "voice filename MIME");
                        return Response{204, "", false};
                    },
                    [](const Request &r) {
                        api_request(r, "POST", "/api/voice-io/polish/segments");
                        auto j = Json::parse(r.body);
                        check(j == Json({{"segments", Json::array({"hello", "world"})}}),
                              "exact polish body");
                        return json({{"text", "Hello world"}});
                    },
                    get("/api/voice-io/quota", {{"allowed", false}, {"count", 100}})});
    auto c = f.client();
    check(c.transcribe(pcm, previous, .5).empty(), "204 empty transcript");
    check(c.polish({"", "hello", "world"}) == "Hello world", "polish result");
    check(c.polish({"", ""}).empty(), "empty polish no request");
    throws([&] { c.transcribe(pcm, "", 0); }, "quota denies upload");
    throws([&] { c.transcribe(pcm, "", 2); }, "invalid overlap local validation");
    f.server.finish();
}
void read_and_token_test() {
    Fixture f;
    Json token = {{"keyName", "app.key"}, {"nonce", "nonce"},
                  {"mac", "signed-mac"},  {"capability", "{\"channel\":[\"subscribe\"]}"},
                  {"clientId", "user"},   {"timestamp", 123456},
                  {"ttl", 60000}};
    f.server.start({[&](const Request &r) {
                        api_request(r, "POST", base() + "/mark-read");
                        check(r.body.empty(), "mark read has no body");
                        return json({{"lastReadAt", "2026-11-17T18:00:00Z"}});
                    },
                    [=](const Request &r) {
                        api_request(r, "POST", "/api/realtime/token");
                        check(Json::parse(r.body) == Json::object(), "realtime empty object");
                        return json(token);
                    }});
    auto c = f.client();
    c.mark_read(thread_id);
    check(c.realtime_token() == token, "TokenRequest JSON retained, not bearer string");
    f.server.finish();
}
void security_test() {
    for (int mode = 0; mode < 3; ++mode) {
        Fixture f;
        std::string url = mode == 0   ? "http://example.com/secret?sig=x"
                          : mode == 1 ? "https://user:password@example.com/secret"
                                      : "https://example.com/secret#fragment";
        f.server.start({get(base() + "/event-snapshot",
                            {{"url", url}, {"lastSeqId", 0}, {"lastEventId", nullptr}})});
        throws([&] { f.client().history(thread_id); }, "unsafe snapshot URL rejected");
        f.server.finish();
    }
    auto a = okou::Client::uuid(), b = okou::Client::uuid();
    check(a.size() == 36 && a != b && a[14] == '4' &&
              std::string("89ab").find(a[19]) != std::string::npos,
          "secure UUID v4");
}
} // namespace
int main() {
    try {
        ::setenv("OKOU_ZERO_TEST_ALLOW_LOOPBACK_HTTP", "1", 1);
        list_test();
        metadata_only_test();
        large_list_test(false);
        large_list_test(true);
        history_test();
        bad_history_tests();
        fixed_thread_test();
        send_test();
        send_identity_test();
        assistant_content_test();
        send_rejection_test();
        uncertain_absence_test();
        voice_test();
        voice_message_test();
        voice_history_test();
        read_and_token_test();
        security_test();
        std::cout << "client protocol tests passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "client test failed: " << e.what() << "\n";
        return 1;
    }
}
