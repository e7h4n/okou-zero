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
const std::string other_id = "22222222-2222-4222-8222-222222222222";
const std::string clip_id = "aaaaaaaa-1111-4000-8000-111111111111";
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
                    if (req.target == "/mcp") {
                        check(req.method == "POST" &&
                                  req.headers.at("accept") == "application/json, text/event-stream",
                              "streamable HTTP request");
                        auto rpc = Json::parse(req.body);
                        const auto &params = rpc.at("params");
                        check(
                            rpc.at("jsonrpc") == "2.0" && rpc.at("method") == "tools/call" &&
                                params.at("name") == req.headers.at("mcp-name") &&
                                params.at("_meta").at("io.modelcontextprotocol/protocolVersion") ==
                                    req.headers.at("mcp-protocol-version"),
                            "MCP envelope");
                        req.target = params.at("name");
                        req.body = params.at("arguments").dump();
                        r = steps_.at(done_)(req);
                        if (!r.drop && r.status < 500) {
                            const auto data = Json::parse(r.body);
                            r = json(
                                {{"jsonrpc", "2.0"},
                                 {"id", rpc["id"]},
                                 {"result",
                                  {{"structuredContent", data}, {"isError", r.status >= 400}}}});
                        }
                    } else {
                        check(req.target.rfind("/storage/", 0) == 0, "unexpected HTTP route");
                        r = steps_.at(done_)(req);
                    }
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
Step reply(const std::string &tool, const Json &args, const Json &body, int status = 200) {
    return [=](const Request &r) {
        api_request(r, "POST", tool);
        check(Json::parse(r.body) == args, "tool arguments for " + tool);
        return json(body, status);
    };
}
Json page_args(int n = 0, const std::string &eid = {}) {
    Json args = {{"threadId", thread_id}, {"limit", 50}};
    if (n) {
        args["sinceSeqId"] = n;
        args["sinceEventId"] = eid;
    }
    return args;
}
Step page(const Json &args, const Json &body, int status = 200) {
    return reply("get_chat_messages", args, body, status);
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
Json tail(Json rows, int last, const std::string &last_id, bool more = false) {
    for (auto &r : rows) {
        r["threadId"] = r.at("chatThreadId");
        r["eventId"] = r.at("id");
        r.erase("id");
        r.erase("chatThreadId");
    }
    return {
        {"snapshot", nullptr},
        {"rows", rows},
        {"cursor",
         {{"lastSeqId", last}, {"lastEventId", last_id.empty() ? Json(nullptr) : Json(last_id)}}},
        {"hasMore", more}};
}
Json input(const std::string &text) {
    return {{"userMessage",
             {{"version", 1}, {"parts", Json::array({{{"type", "text"}, {"text", text}}})}}}};
}
Json voice_part() {
    return {{"type", "voice"}, {"clips", Json::array({{{"fileId", clip_id}}})}};
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
    auto undated = row(7, "output.message", {{"content", "Undated reply"}});
    undated.erase("createdAt");
    auto invalid_date = row(8, "output.message", {{"content", "Undated reply"}});
    invalid_date["createdAt"] = "2026-02-30T00:00:00Z";
    d["revokesEventId"] = id(2);
    auto end = row(5, "run.completed");
    auto first = tail(Json::array({d, end}), 5, id(5), true);
    first["snapshot"] = {{"url", storage}, {"lastSeqId", 3}, {"lastEventId", id(3)}};
    f.server.start(
        {page(page_args(), first),
         [=](const Request &r) {
             check(!r.headers.count("authorization"), "history storage bearer absent");
             check(r.target == "/storage/history?sig=keep%2Bsignature", "signature preserved");
             return Response{200, a.dump() + "\n" + b.dump() + "\n" + c.dump() + "\n", false};
         },
         page(page_args(5, id(5)), tail(Json::array({summer, undated, invalid_date}), 8, id(8))),
         page(page_args(8, id(8)), tail(Json::array(), 8, id(8)))});
    const char *zone = getenv("TZ");
    const std::string original_zone = zone ? zone : "";
    const bool had_zone = zone != nullptr;
    setenv("TZ", "America/Los_Angeles", 1);
    tzset();
    auto client = f.client();
    auto messages = client.history(thread_id);
    check(client.history(thread_id).size() == messages.size(),
          "reattach resumes from paired cursor");
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
            reply["rows"][0]["threadId"] = other_id;
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
        f.server.start({page(page_args(0), reply)});
        throws([&] { f.client().history(thread_id); }, "malformed history rejected");
        f.server.finish();
    }
    {
        Fixture f;
        f.server.start({page(page_args(0), {{"error", {{"code", "CHAT_EVENTS_EXPIRED"}}}}, 410),
                        page(page_args(0), {{"error", {{"code", "CHAT_EVENTS_EXPIRED"}}}}, 410)});
        throws([&] { f.client().history(thread_id); }, "410 recovery bounded");
        f.server.finish();
    }
    {
        Fixture f;
        f.server.start({page(page_args(0), {{"error", {{"code", "CHAT_EVENTS_EXPIRED"}}}}, 410),
                        page(page_args(0), tail(Json::array(), 0, ""))});
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
        f.server.start({page(page_args(), {{"snapshot",
                                            {{"url", storage},
                                             {"lastSeqId", mode == 2 ? 1 : 2},
                                             {"lastEventId", mode == 2 ? id(1) : id(2)}}}}),
                        [=](const Request &) { return Response{200, payload, false}; }});
        throws([&] { f.client().history(thread_id); },
               "snapshot cursor, duplicate identity, or newline invalid");
        f.server.finish();
    }
    {
        Fixture f;
        auto a = row(1, "output.message", {{"content", "answer"}}), b = row(2, "run.failed");
        b["runId"] = other_id;
        f.server.start({page(page_args(0), tail(Json::array({a, b}), 2, id(2)))});
        auto m = f.client().history(thread_id);
        check(m.size() == 1 && !m[0].terminal, "unrelated terminal ignored");
        f.server.finish();
    }
}
void fixed_thread_test() {
    Fixture f;
    std::string original, replacement;
    const auto create = [&](std::string &saved, int status) {
        return [&, status](const Request &r) {
            api_request(r, "POST", "create_chat_thread");
            auto request = Json::parse(r.body);
            const auto candidate = request.at("clientThreadId").get<std::string>();
            if (saved.empty())
                saved = candidate;
            check(candidate == saved, "Retries reuse the reserved chat ID");
            return json({{"threadId", candidate}, {"title", "Cardputer Zero"}}, status);
        };
    };
    const auto open = [&](int status) {
        return [&, status](const Request &r) {
            api_request(r, "POST", "get_chat_messages");
            check(Json::parse(r.body).at("threadId") == original, "opens saved thread");
            if (status == 404)
                return json({{"error", {{"code", "NOT_FOUND"}}}}, 404);
            return json({{"id", original}, {"title", "Renamed"}}, status);
        };
    };
    f.server.start({create(original, 503), create(original, 201), open(200), open(503), open(404),
                    create(replacement, 201)});
    throws([&] { f.client().fixed_thread_id(); }, "Transient create failure surfaces");
    check(f.client().fixed_thread_id() == original, "Reserved ID persisted before create");
    check(f.client().fixed_thread_id() == original, "Launch opens the existing fixed chat");
    throws([&] { f.client().fixed_thread_id(); }, "A service outage does not create another chat");
    check(f.client().fixed_thread_id() == replacement && replacement != original,
          "An authoritative miss replaces the fixed chat");
    f.server.finish();
}
void lost_voice_acknowledgement_test() {
    Fixture f;
    std::string sent_id;
    f.server.start({
        [&](const Request &r) {
            api_request(r, "POST", "send_chat_message");
            check(r.headers.at("content-type") == "application/json", "JSON content type");
            auto j = Json::parse(r.body);
            sent_id = j.at("clientEventId");
            return Response{200, "", true};
        },
        [&](const Request &r) {
            api_request(r, "POST", "get_chat_messages");
            check(Json::parse(r.body) == page_args(0), "history cursor");
            auto a =
                row(1, "input.prompt",
                    {{"userMessage", {{"version", 1}, {"parts", Json::array({voice_part()})}}}});
            a["revokesEventId"] = sent_id;
            return json(tail(Json::array({a}), 1, id(1)));
        },
    });
    auto c = f.client();
    auto first = c.send_voice(thread_id, clip_id);
    check(first.uncertain && !first.client_event_id.empty(), "dropped write uncertain");
    std::size_t files = 0;
    for (const auto &e : std::filesystem::directory_iterator(f.config.data_dir)) {
        ++files;
        std::ifstream in(e.path());
        std::string contents((std::istreambuf_iterator<char>(in)), {});
        check(contents.find(clip_id) == std::string::npos &&
                  contents.find(bearer) == std::string::npos,
              "no audio reference or token persisted");
        check(contents.find(sent_id) != std::string::npos, "send identity persisted");
        struct stat st {};
        ::stat(e.path().c_str(), &st);
        check((st.st_mode & 077) == 0, "pending mode private");
    }
    check(files == 1, "one pending record");
    // Reopen the ledger and reconcile against durable history.
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
    f.server.start({[&](const Request &r) {
                        api_request(r, "POST", "send_chat_message");
                        pending = Json::parse(r.body).at("clientEventId");
                        return json({{"threadId", other_id}});
                    },
                    [&](const Request &r) {
                        api_request(r, "POST", "get_chat_messages");
                        check(Json::parse(r.body) == page_args(0), "history cursor");
                        auto a = row(1, "input.prompt",
                                     {{"userMessage",
                                       {{"version", 1}, {"parts", Json::array({voice_part()})}}}});
                        a["id"] = pending;
                        return json(tail(Json::array({a}), 2, id(2)));
                    }});
    auto c = f.client();
    auto result = c.send_voice(thread_id, clip_id);
    check(result.uncertain && c.has_pending_send(thread_id),
          "wrong success thread remains uncertain");
    throws([&] { c.history(thread_id); }, "matching input with bad final cursor rejected");
    check(c.has_pending_send(thread_id), "partial malformed read never clears pending");
    f.server.finish();
}
void assistant_content_test() {
    Fixture f;
    const std::string markdown = "[Documentation](https://example.com) and **plain Markdown**";
    const std::string json_answer = R"({"type":"text","text":"A JSON example"})";
    const std::string link = "[Review](https://app.okou.ai/connectors/example/permissions)";
    auto rows = Json::array({row(1, "output.message", {{"content", markdown}}),
                             row(2, "output.message", {{"content", json_answer}}),
                             row(3, "output.message", {{"content", link}})});
    f.server.start({page(page_args(0), tail(rows, 3, id(3)))});
    auto messages = f.client().history(thread_id);
    check(messages.size() == 3 && messages[0].text == markdown && messages[1].text == json_answer,
          "Markdown and literal JSON answers retain the assistant's words");
    check(messages[2].text.find(link) == 0 &&
              messages[2].text.find("Okou Web") != std::string::npos,
          "Action link directs the reader to Web");
    f.server.finish();
}
void send_rejection_test() {
    Fixture f;
    f.server.start({[](const Request &r) {
                        api_request(r, "POST", "send_chat_message");
                        return json({{"error", {{"code", "BAD_REQUEST"}}}}, 400);
                    },
                    [](const Request &r) {
                        api_request(r, "POST", "send_chat_message");
                        return json({{"error", {{"code", "CONFLICT"}}}}, 409);
                    }});
    auto c = f.client();
    throws([&] { c.send_voice(thread_id, clip_id); }, "known rejection surfaces");
    check(!c.has_pending_send(thread_id), "known rejection clears reservation");
    check(c.send_voice(thread_id, clip_id).uncertain && c.has_pending_send(thread_id),
          "conflict preserves uncertainty");
    f.server.finish();
}
void uncertain_absence_test() {
    Fixture f;
    std::string pending;
    f.server.start({[&](const Request &r) {
                        pending = Json::parse(r.body).at("clientEventId");
                        return json({}, 503);
                    },
                    page(page_args(0), tail(Json::array(), 0, ""))});
    auto first = f.client().send_voice(thread_id, clip_id);
    f.client().history(thread_id);
    auto again = f.client().send_voice(thread_id, clip_id);
    check(first.uncertain && again.uncertain && again.client_event_id == pending &&
              f.client().has_pending_send(thread_id),
          "absence does not authorize resend");
    f.server.finish();
}
void voice_history_test() {
    Fixture f;
    auto parts = Json::array({voice_part(),
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
    f.server.start({page(page_args(0), tail(rows, 3, id(3))),
                    reply("get_voice_playback_url", {{"fileId", clip_id}}, {{"url", url}})});
    auto client = f.client();
    auto messages = client.history(thread_id);
    check(messages.size() == 2 && messages[0].id == id(1) && messages[1].id == id(3) &&
              messages[0].text == "[Voice message]" &&
              messages[0].voice_clips == std::vector<std::string>{clip_id},
          "Promoted voice replaces its placeholder and retains playback and the original send ID");
    check(client.voice_playback_url(clip_id) == url, "Preserves the signed playback URL");
    f.server.finish();
}
void voice_message_test() {
    Fixture f;
    const auto upload_url = f.server.origin() + "/storage/voice?signature=a%2Bb";
    f.server.start({
        [&](const Request &r) {
            api_request(r, "POST", "prepare_voice_upload");
            check(Json::parse(r.body) == Json({{"filename", "recording.wav"}, {"size", 48}}),
                  "prepare private WAV");
            return json({{"id", clip_id},
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
            api_request(r, "POST", "complete_voice_upload");
            check(Json::parse(r.body).at("id") == clip_id, "complete uploaded file");
            return json({{"id", clip_id}});
        },
        [&](const Request &r) {
            api_request(r, "POST", "send_chat_message");
            auto body = Json::parse(r.body);
            const auto &message = body.at("userMessage");
            const auto &parts = message.at("parts");
            check(body.at("threadId") == thread_id && !body.contains("prompt") &&
                      message.at("version") == 1 && parts.size() == 2 &&
                      parts.at(0) == voice_part(),
                  "send the recording to the fixed chat");
            check(parts.at(1).at("type") == "additional_info" &&
                      parts.at(1).at("text").get<std::string>().find("Cardputer Zero") !=
                          std::string::npos,
                  "send the reader context as hidden message metadata");
            return json({{"threadId", thread_id}, {"eventId", body.at("clientEventId")}});
        },
    });
    auto c = f.client();
    auto uploaded = c.upload_voice({42, -42});
    check(uploaded == clip_id && !c.send_voice(thread_id, uploaded).uncertain,
          "voice upload accepted by chat");
    f.server.finish();
}
void token_test() {
    Fixture f;
    Json token = {{"keyName", "app.key"}, {"nonce", "nonce"},
                  {"mac", "signed-mac"},  {"capability", "{\"channel\":[\"subscribe\"]}"},
                  {"clientId", "user"},   {"timestamp", 123456},
                  {"ttl", 60000}};
    f.server.start({[=](const Request &r) {
        api_request(r, "POST", "get_chat_realtime_token");
        check(Json::parse(r.body) == Json::object(), "realtime empty object");
        return json(token);
    }});
    auto c = f.client();
    check(c.realtime_token() == token, "TokenRequest JSON retained, not bearer string");
    f.server.finish();
}
void security_test() {
    for (int mode = 0; mode < 3; ++mode) {
        Fixture f;
        std::string url = mode == 0   ? "http://example.com/secret?sig=x"
                          : mode == 1 ? "https://user:password@example.com/secret"
                                      : "https://example.com/secret#fragment";
        f.server.start(
            {page(page_args(),
                  {{"snapshot", {{"url", url}, {"lastSeqId", 0}, {"lastEventId", nullptr}}}})});
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
        history_test();
        bad_history_tests();
        fixed_thread_test();
        lost_voice_acknowledgement_test();
        send_identity_test();
        assistant_content_test();
        send_rejection_test();
        uncertain_absence_test();
        voice_message_test();
        voice_history_test();
        token_test();
        security_test();
        std::cout << "client protocol tests passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "client test failed: " << e.what() << "\n";
        return 1;
    }
}
