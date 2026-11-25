// SPDX-License-Identifier: MIT
// Independent implementation of the documented public HTTP wire contracts.
#include "okou/core.hpp"
#include <algorithm>
#include <ctime>
#include <fcntl.h>
#include <filesystem>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <regex>
#include <set>
#include <sstream>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace okou {
namespace detail {
Json read_private(const std::string &);
void write_private(const std::string &, const Json &);
} // namespace detail
namespace {
constexpr std::size_t max_events = 8192, max_messages = 200;
constexpr char cardputer_context[] =
    "The user sent this message from an M5Stack Cardputer Zero. Its 320x170 screen displays basic "
    "Markdown and fits about 3-4 lines of body text (roughly 45-60 Chinese characters) at a time. "
    "Scrolling makes long replies hard to read. Lead with the answer and keep it brief. "
    "Markdown links show only their labels and cannot be opened on the handheld. When a link is "
    "needed, include it in the reply and tell the user to open this chat on their phone or "
    "computer to complete the action.";
[[noreturn]] void invalid() {
    throw std::runtime_error("Invalid or oversized server protocol response");
}
std::string str(const Json &j, const char *key) {
    auto i = j.find(key);
    if (i == j.end() || i->is_null())
        return {};
    if (!i->is_string())
        invalid();
    return i->get<std::string>();
}
std::int64_t seq(const Json &j, const char *key) {
    if (!j.contains(key) || j[key].is_null())
        return 0;
    if (!j[key].is_number_integer())
        invalid();
    auto v = j[key].get<std::int64_t>();
    if (v < 0)
        invalid();
    return v;
}
bool flag(const Json &j, const char *key) {
    if (!j.contains(key))
        return false;
    if (!j[key].is_boolean())
        invalid();
    return j[key].get<bool>();
}
std::string clipped(std::string s, std::size_t maximum) {
    if (s.size() <= maximum)
        return s;
    auto n = maximum;
    while (n && (static_cast<unsigned char>(s[n]) & 192) == 128)
        --n;
    s.resize(n);
    return s;
}
std::string assistant_text(const std::string &content) {
    static const std::string review =
        "Action or unsupported content: review in Okou Web. This client cannot approve actions.";
    for (const char *marker : {"/permissions", "/connectors/", "/mail/drafts/", "/authorizations/",
                               "/ssh/", "/computer-use/"})
        if (content.find(marker) != std::string::npos)
            return clipped(content, 32768 - review.size() - 1) + "\n" + review;
    return clipped(content, 32768);
}
bool is_uuid(const std::string &s) {
    if (s.size() != 36)
        return false;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (s[i] != '-')
                return false;
        } else if (!std::isxdigit(static_cast<unsigned char>(s[i])))
            return false;
    }
    return true;
}
// Convert the event instant, including its UTC offset, using the device's zone rules.
std::string local_sent_time(const std::string &stamp) {
    static const std::regex iso(
        R"(^([0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2})(?:\.[0-9]+)?(Z|([+-])([0-9]{2}):([0-9]{2}))$)");
    std::smatch fields;
    if (stamp.size() < 20 || stamp.size() > 40 || !std::regex_match(stamp, fields, iso))
        return {};
    std::tm utc{};
    const std::string civil = fields[1].str();
    const char *end = strptime(civil.c_str(), "%Y-%m-%dT%H:%M:%S", &utc);
    if (!end || *end)
        return {};
    const int day = utc.tm_mday;
    auto instant = timegm(&utc);
    if (utc.tm_mday != day)
        return {};
    if (fields[3].matched) {
        const int hours = std::stoi(fields[4].str()), minutes = std::stoi(fields[5].str());
        if (hours > 23 || minutes > 59)
            return {};
        instant -= (fields[3] == "+" ? 1 : -1) * (hours * 3600 + minutes * 60);
    }
    std::tm local{};
    char text[24]{};
    if (!localtime_r(&instant, &local) ||
        !std::strftime(text, sizeof(text), "%Y-%m-%d %H:%M", &local))
        return {};
    return text;
}
Json visible_row(const Json &row) {
    Json safe = Json::object();
    for (const char *k : {"id", "chatThreadId", "seqId", "eventType", "runId", "revokesEventId"})
        if (row.contains(k))
            safe[k] = row[k];
    auto p = row.value("payload", Json::object());
    if (!p.is_object())
        return safe;
    auto type = str(row, "eventType");
    Json payload = Json::object();
    if (type == "output.message") {
        payload["content"] = assistant_text(str(p, "content"));
        const auto stamp = row.find("createdAt");
        if (stamp != row.end() && stamp->is_string() &&
            stamp->get_ref<const std::string &>().size() <= 40)
            safe["createdAt"] = *stamp;
    }
    if (type.rfind("input.", 0) == 0 && p.contains("userMessage") && p["userMessage"].is_object()) {
        auto doc = p["userMessage"];
        Json parts = Json::array();
        std::size_t bytes = 0;
        if (doc.contains("parts") && doc["parts"].is_array())
            for (const auto &part : doc["parts"]) {
                if (parts.size() >= 128 || bytes >= 32768)
                    break;
                const auto kind = str(part, "type");
                if (kind == "source" || kind == "additional_info" || kind == "automation" ||
                    kind == "model")
                    continue;
                if (kind == "text") {
                    auto text = clipped(str(part, "text"), 32768 - bytes);
                    bytes += text.size();
                    parts.push_back({{"type", "text"}, {"text", text}});
                } else if (kind == "voice") {
                    Json clips = Json::array();
                    if (part.contains("clips") && part["clips"].is_array())
                        for (const auto &clip : part["clips"]) {
                            if (clips.size() == 32)
                                break;
                            auto file_id = str(clip, "fileId");
                            if (is_uuid(file_id))
                                clips.push_back(file_id);
                        }
                    parts.push_back({{"type", "voice"}, {"clips", std::move(clips)}});
                } else {
                    parts.push_back({{"type", "unsupported"}});
                }
            }
        payload["userMessage"] = {{"parts", parts}};
    }
    safe["payload"] = std::move(payload);
    return safe;
}
std::string tid(const std::string &id) {
    if (!is_uuid(id))
        throw std::runtime_error("Invalid thread identity");
    return "/api/chat-threads/" + id;
}
void cursor(std::int64_t n, const std::string &id) {
    if ((n == 0 && !id.empty()) || (n > 0 && !is_uuid(id)))
        invalid();
}
Json parsed(const std::string &s) {
    try {
        return Json::parse(s);
    } catch (...) {
        invalid();
    }
}
void success(const HttpResponse &r) {
    if (r.status < 200 || r.status >= 300)
        throw HttpError(r.status, "HTTP request failed (" + std::to_string(r.status) + ")",
                        r.status >= 500);
}
// Never persist prompts or tokens. Identity records are account/origin scoped.
std::string scoped_path(const Config &c, const Tokens &t, const std::string &prefix,
                        const std::string &key_suffix) {
    if (c.data_dir.empty() || t.user_id.empty() || t.org_id.empty())
        throw std::runtime_error(
            "Private data directory and scoped identity required for reliable send");
    std::string scope = c.api_origin + "\n" + t.user_id + "\n" + t.org_id + "\n" + key_suffix;
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char *>(scope.data()), scope.size(), digest);
    static const char *hex = "0123456789abcdef";
    std::string key;
    for (auto b : digest) {
        key += hex[b >> 4];
        key += hex[b & 15];
    }
    return c.data_dir + "/" + prefix + key + ".json";
}
std::string pending_path(const Config &c, const Tokens &t, const std::string &thread) {
    return scoped_path(c, t, "pending-", thread);
}
void save_pending(const std::string &path, const Json &j) {
    std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    // O_EXCL prevents concurrent clients from replacing another uncertain send.
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (fd < 0)
        throw std::runtime_error("Cannot reserve pending send identity");
    std::string data = j.dump();
    std::size_t pos = 0;
    while (pos < data.size()) {
        auto n = ::write(fd, data.data() + pos, data.size() - pos);
        if (n <= 0) {
            ::close(fd);
            throw std::runtime_error("Cannot persist pending send identity");
        }
        pos += static_cast<std::size_t>(n);
    }
    bool ok = ::fsync(fd) == 0;
    ::close(fd);
    int dir = ::open(std::filesystem::path(path).parent_path().c_str(), O_RDONLY | O_DIRECTORY);
    if (dir < 0)
        ok = false;
    else {
        if (::fsync(dir) != 0)
            ok = false;
        ::close(dir);
    }
    if (!ok)
        throw std::runtime_error("Cannot durably persist pending send identity");
}
Json load_pending(const std::string &path) {
    struct stat st {};
    if (::lstat(path.c_str(), &st) != 0) {
        if (errno == ENOENT)
            return nullptr;
        throw std::runtime_error("Cannot inspect pending send");
    }
    if (!S_ISREG(st.st_mode) || st.st_uid != ::geteuid() || (st.st_mode & 077) != 0 ||
        st.st_size > 2048)
        throw std::runtime_error("Unsafe pending send record");
    int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW);
    if (fd < 0)
        throw std::runtime_error("Cannot read pending send");
    char b[2049];
    auto n = ::read(fd, b, sizeof(b));
    ::close(fd);
    if (n < 0 || n > 2048)
        invalid();
    auto j = parsed(std::string(b, static_cast<std::size_t>(n)));
    if (!is_uuid(str(j, "clientEventId")) || !is_uuid(str(j, "chatThreadSortEventId")))
        invalid();
    return j;
}
using Fetch = std::function<Json(const std::string &)>;
struct History {
    std::vector<Json> rows;
    bool found = false;
};
History read_history(const std::string &thread, const Fetch &api, const Fetch &archive,
                     const std::string &looking) {
    const auto base = tid(thread);
    for (int attempt = 0; attempt < 2; ++attempt) {
        History out;
        std::int64_t n = 0;
        std::string id;
        std::set<std::string> seen;
        std::size_t count = 0;
        auto consume = [&](const Json &row) {
            auto next = seq(row, "seqId");
            auto rid = str(row, "id");
            auto run = str(row, "runId"), revoke = str(row, "revokesEventId");
            if (str(row, "eventType").size() > 128 || (!run.empty() && !is_uuid(run)) ||
                (!revoke.empty() && !is_uuid(revoke)))
                invalid();
            if (str(row, "chatThreadId") != thread || next <= n || !is_uuid(rid) ||
                !seen.insert(rid).second || ++count > max_events)
                invalid();
            n = next;
            id = rid;
            if (!looking.empty() && (rid == looking || revoke == looking) &&
                str(row, "eventType") == "input.prompt")
                out.found = true;
            // The public endpoint excludes tool events. Retain only the visible window.
            out.rows.push_back(visible_row(row));
            if (out.rows.size() > max_messages)
                out.rows.erase(out.rows.begin());
        };
        try {
            Json baseline;
            try {
                baseline = api(base + "/event-snapshot");
            } catch (const HttpError &e) {
                // api() preserves the one documented absence code through its message.
                if (e.status != 404 || std::string(e.what()) != "CHAT_EVENT_SNAPSHOT_NOT_FOUND")
                    throw;
            }
            if (!baseline.is_null()) {
                auto end = seq(baseline, "lastSeqId");
                auto eid = str(baseline, "lastEventId");
                cursor(end, eid);
                auto rows = archive(str(baseline, "url"));
                if (!rows.is_array())
                    invalid();
                for (const auto &row : rows)
                    consume(row);
                if (n != end || id != eid)
                    invalid();
            }
            bool confirm = true;
            for (std::size_t page = 0; page < max_events; ++page) {
                auto path = base + "/event-rows?sinceSeqId=" + std::to_string(n) + "&limit=50";
                if (n)
                    path += "&sinceEventId=" + Http::encode(id);
                auto p = api(path);
                auto rows = p.at("rows");
                if (!rows.is_array() || rows.size() > 50)
                    invalid();
                bool more = flag(p, "hasMore");
                if (more && rows.empty())
                    invalid();
                for (const auto &row : rows)
                    consume(row);
                auto c = p.at("cursor");
                cursor(seq(c, "lastSeqId"), str(c, "lastEventId"));
                if (seq(c, "lastSeqId") != n || str(c, "lastEventId") != id)
                    invalid();
                if (!more) {
                    if (confirm) {
                        confirm = false;
                        continue;
                    }
                    return out;
                }
            }
            invalid();
        } catch (const HttpError &e) {
            if (e.status == 410 && attempt == 0)
                continue;
            throw;
        }
    }
    invalid();
}
std::vector<Message> present(const History &h) {
    std::set<std::string> revoked;
    // A queued input is promoted to a run-backed input with a new row ID.
    // Keep the original send identity for both acknowledgement and navigation.
    std::map<std::string, std::string> input_origins;
    for (const auto &r : h.rows) {
        auto target = str(r, "revokesEventId");
        if (target.empty())
            continue;
        revoked.insert(target);
        if (str(r, "eventType").rfind("input.", 0) == 0) {
            auto prior = input_origins.find(target);
            input_origins.emplace(str(r, "id"),
                                  prior == input_origins.end() ? target : prior->second);
        }
    }
    std::vector<Message> out;
    std::map<std::string, std::vector<std::size_t>> runs;
    for (const auto &r : h.rows) {
        auto id = str(r, "id"), type = str(r, "eventType"), run = str(r, "runId");
        if (revoked.count(id))
            continue;
        auto p = r.value("payload", Json::object());
        if (!p.is_object())
            p = Json::object();
        Message m{id, "", "", false};
        m.run_id = run;
        if (type.rfind("input.", 0) == 0) {
            m.role = "user";
            if (auto origin = input_origins.find(id); origin != input_origins.end())
                m.id = origin->second;
            auto doc = p.value("userMessage", Json::object());
            bool unsupported = false;
            if (doc.is_object() && doc.contains("parts") && doc["parts"].is_array())
                for (const auto &part : doc["parts"]) {
                    if (str(part, "type") == "text")
                        m.text += str(part, "text");
                    else if (str(part, "type") == "voice") {
                        m.text += m.text.empty() ? "[Voice message]" : "\n[Voice message]";
                        for (const auto &clip : part.at("clips"))
                            if (m.voice_clips.size() < 32)
                                m.voice_clips.push_back(clip.get<std::string>());
                    } else
                        unsupported = true;
                }
            if (unsupported)
                m.text += m.text.empty() ? "[Attachment or action: review in Okou Web]"
                                         : "\n[Attachment or action: review in Okou Web]";
        } else if (type == "output.message") {
            m.role = "assistant";
            m.local_sent_at = local_sent_time(str(r, "createdAt"));
            m.text = str(p, "content");
        } else if (type == "output.error") {
            m.role = "system";
            m.text = "Response error. Review in Okou Web.";
        } else if (type == "run.completed" || type == "run.failed" || type == "run.cancelled") {
            // Lifecycle conclusions require the SAME run identity, never indicators.
            if (!run.empty() && runs.count(run)) {
                for (auto index : runs[run])
                    out[index].terminal = true;
                if (type != "run.completed")
                    out.push_back({id, "system",
                                   type == "run.failed" ? "Run failed" : "Run cancelled", true});
            }
            continue;
        } else if (type.find("approval") != std::string::npos ||
                   type.find("action") != std::string::npos ||
                   type.find("permission") != std::string::npos) {
            m.role = "system";
            m.text = "Action requires review in Okou Web. This client cannot approve it.";
        } else
            continue;
        if (m.text.empty())
            continue;
        m.text = clipped(std::move(m.text), 32768);
        if (!run.empty())
            runs[run].push_back(out.size());
        out.push_back(std::move(m));
    }
    return out;
}
} // namespace
Client::Client(Config config, Http &http, Tokens tokens)
    : config_(std::move(config)), http_(http), tokens_(std::move(tokens)) {}
void Client::set_tokens(Tokens tokens) {
    tokens_ = std::move(tokens);
}
Json Client::api(const std::string &method, const std::string &path, const Json &body) {
    if (path.rfind("/api/", 0) != 0)
        throw std::runtime_error("Invalid API path");
    std::map<std::string, std::string> headers = {{"Accept", "application/json"}};
    if (!body.is_null())
        headers["Content-Type"] = "application/json";
    auto r = http_.request(method, config_.api_origin + path, body.is_null() ? "" : body.dump(),
                           tokens_.access_token, headers);
    if (r.status == 404) {
        auto j = Json::parse(r.body, nullptr, false);
        if (j.is_object() && j.contains("error") && j["error"].is_object() &&
            str(j["error"], "code") == "CHAT_EVENT_SNAPSHOT_NOT_FOUND")
            throw HttpError(404, "CHAT_EVENT_SNAPSHOT_NOT_FOUND");
    }
    success(r);
    if (r.status == 204)
        return nullptr;
    return parsed(r.body);
}
Json Client::history_snapshot(const std::string &url) {
    // Signed URL is passed byte-for-byte. No bearer or client headers, no redirects (Http).
    const char *fixture = std::getenv("OKOU_ZERO_TEST_ALLOW_LOOPBACK_HTTP");
    bool test_loopback =
        fixture && std::string(fixture) == "1" && url.rfind("http://127.0.0.1:", 0) == 0;
    if (url.rfind("https://", 0) != 0 && !test_loopback)
        throw std::runtime_error("Snapshot storage URL must use HTTPS");
    auto offset = test_loopback ? 7u : 8u;
    auto authority_end = url.find('/', offset);
    auto authority = url.substr(
        offset, authority_end == std::string::npos ? std::string::npos : authority_end - offset);
    if (authority.empty() || authority.find('@') != std::string::npos ||
        url.find('#') != std::string::npos || url.find_first_of("\r\n\\") != std::string::npos)
        throw std::runtime_error("Unsafe snapshot storage URL");
    auto r = http_.request("GET", url);
    success(r);
    if (!r.body.empty() && r.body.back() != '\n')
        invalid();
    Json rows = Json::array();
    std::istringstream lines(r.body);
    std::string line;
    while (std::getline(lines, line)) {
        if (line.empty() || line == "\r")
            invalid();
        if (rows.size() >= max_events)
            invalid();
        rows.push_back(parsed(line));
    }
    return rows;
}
std::string Client::fixed_thread_id() {
    const auto path = scoped_path(config_, tokens_, "fixed-chat-", "cardputer");
    // One reservation across concurrent launches for the same private data directory.
    struct DirectoryLock {
        int fd;
        ~DirectoryLock() {
            if (fd >= 0)
                ::close(fd);
        }
    } lock{::open(config_.data_dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)};
    if (lock.fd < 0 || ::flock(lock.fd, LOCK_EX) != 0)
        throw std::runtime_error("Cannot lock fixed chat storage");

    const auto record = detail::read_private(path);
    auto id = str(record, "threadId");
    const bool ready = flag(record, "ready");
    const auto check_id = [&](const Json &body) {
        if (str(body, "id") != id)
            invalid();
    };
    if (is_uuid(id) && ready) {
        try {
            check_id(api("GET", tid(id) + "/metadata"));
            return id;
        } catch (const HttpError &error) {
            // Only an authoritative miss permits replacing the selected chat.
            if (error.status != 404 && error.status != 410)
                throw;
        }
        id.clear();
    }
    if (!is_uuid(id)) {
        id = uuid();
        // The server treats clientThreadId as idempotent. Persist it before a
        // request whose response may be lost, including across restarts.
        detail::write_private(path, {{"threadId", id}, {"ready", false}});
    }
    check_id(
        api("POST", "/api/chat-threads", {{"clientThreadId", id}, {"title", "Cardputer Zero"}}));
    detail::write_private(path, {{"threadId", id}, {"ready", true}});
    return id;
}
std::vector<Message> Client::history(const std::string &thread) {
    auto path = pending_path(config_, tokens_, thread);
    auto pending = load_pending(path);
    if (!pending.is_null() && str(pending, "threadId") != thread)
        invalid();
    auto looking = pending.is_null() ? std::string() : str(pending, "clientEventId");
    auto result = read_history(
        thread, [&](const std::string &p) { return api("GET", p); },
        [&](const std::string &u) { return history_snapshot(u); }, looking);
    // Reconciliation commits only after the entire cursor chain validates.
    if (result.found)
        std::filesystem::remove(path);
    return present(result);
}
bool Client::has_pending_send(const std::string &thread_id) const {
    tid(thread_id);
    auto pending = load_pending(pending_path(config_, tokens_, thread_id));
    if (!pending.is_null() && str(pending, "threadId") != thread_id)
        invalid();
    return !pending.is_null();
}
std::string Client::voice_playback_url(const std::string &file_id) {
    if (!is_uuid(file_id))
        throw std::runtime_error("Invalid voice clip identity");
    auto url = str(api("GET", "/api/voice/clips/" + file_id + "/playback"), "url");
    if (url.empty())
        invalid();
    return url;
}

std::string Client::upload_voice(const std::vector<std::int16_t> &pcm) {
    constexpr auto limit = 25U * 1024U * 1024U;
    constexpr auto mime = "audio/wav";
    if (pcm.empty() || pcm.size() > (limit - 44) / sizeof(std::int16_t))
        throw std::runtime_error("Recording must contain audio and fit within 25 MiB");
    auto audio = wav(pcm);
    auto prepared =
        api("POST", "/api/uploads/prepare/private",
            {{"filename", "recording.wav"}, {"contentType", mime}, {"size", audio.size()}});
    auto id = str(prepared, "id");
    auto url = str(prepared, "uploadUrl");
    if (!is_uuid(id) || url.empty())
        invalid();
    auto headers =
        prepared.value("uploadHeaders", Json::object()).get<std::map<std::string, std::string>>();
    headers["Content-Type"] = mime;
    std::string_view bytes(reinterpret_cast<const char *>(audio.data()), audio.size());
    success(http_.request("PUT", url, bytes, "", headers));
    if (str(api("POST", "/api/uploads/complete", {{"id", id}}), "id") != id)
        invalid();
    return id;
}

SendResult Client::send_voice(const std::string &thread_id, const std::string &file_id) {
    if (!is_uuid(file_id))
        throw std::runtime_error("Invalid voice clip identity");
    auto base = tid(thread_id);
    auto path = pending_path(config_, tokens_, thread_id);
    auto pending = load_pending(path);
    // Reconcile through history(), never submit a second operation while uncertain.
    if (!pending.is_null())
        return {str(pending, "clientEventId"), true};
    auto id = uuid();
    auto metadata = api("GET", base + "/metadata");
    if (str(metadata, "id") != thread_id)
        invalid();
    auto sort = uuid();
    const Json parts = Json::array({
        {{"type", "voice"}, {"clips", Json::array({{{"fileId", file_id}}})}},
        {{"type", "additional_info"}, {"text", cardputer_context}},
    });
    Json body = {{"threadId", thread_id},
                 {"prompt", "(see message parts)"},
                 {"clientEventId", id},
                 {"chatThreadSortEventId", sort},
                 {"userMessage", {{"version", 1}, {"parts", parts}}},
                 {"hasTextContent", false},
                 {"computerUseHostId", metadata.value("computerUseHostId", Json(nullptr))},
                 {"cloudBrowserEnabled", flag(metadata, "cloudBrowserEnabled")}};
    save_pending(path,
                 {{"clientEventId", id}, {"chatThreadSortEventId", sort}, {"threadId", thread_id}});
    try {
        auto result = api("POST", "/api/chat/events", body);
        if (str(result, "threadId") != thread_id)
            return {id, true};
        std::filesystem::remove(path);
        return {id, false};
    } catch (const HttpError &e) {
        if (e.status >= 400 && e.status < 500 && e.status != 408 && e.status != 409 &&
            !e.uncertain) {
            std::filesystem::remove(path);
            throw;
        }
        return {id, true};
    } catch (...) {
        return {id, true};
    }
}
Json Client::realtime_token() {
    auto t = api("POST", "/api/realtime/token", Json::object());
    for (const char *k : {"keyName", "nonce", "mac", "capability", "clientId"})
        if (str(t, k).empty())
            invalid();
    if (!t.contains("timestamp") || !t["timestamp"].is_number_integer() ||
        seq(t, "timestamp") == 0 || str(t, "clientId") != tokens_.user_id)
        invalid();
    if (t.contains("ttl") && (!t["ttl"].is_number_integer() || seq(t, "ttl") == 0))
        invalid();
    return t;
}
std::string Client::uuid() {
    unsigned char bytes[16];
    if (RAND_bytes(bytes, sizeof(bytes)) != 1)
        throw std::runtime_error("Secure random source unavailable");
    bytes[6] = (bytes[6] & 15) | 64;
    bytes[8] = (bytes[8] & 63) | 128;
    const char *h = "0123456789abcdef";
    std::string out;
    for (int i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10)
            out += '-';
        out += h[bytes[i] >> 4];
        out += h[bytes[i] & 15];
    }
    return out;
}
} // namespace okou
