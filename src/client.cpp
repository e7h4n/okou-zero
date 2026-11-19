// SPDX-License-Identifier: MIT
// Independent implementation of the documented public HTTP wire contracts.
#include "okou/core.hpp"
#include <algorithm>
#include <cmath>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <set>
#include <sqlite3.h>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace okou {
namespace {
constexpr std::size_t max_events = 8192, max_visible = 128, max_messages = 200;
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
bool flag(const Json &j, const char *key, bool fallback = false) {
    if (!j.contains(key))
        return fallback;
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
    auto first = content.find_first_not_of(" \t\r\n");
    auto plain = [&]() {
        for (const char *marker : {"/permissions", "/connectors/", "/mail/drafts/",
                                   "/authorizations/", "/ssh/", "/computer-use/"})
            if (content.find(marker) != std::string::npos)
                return clipped(content, 32768 - review.size() - 1) + "\n" + review;
        return clipped(content, 32768);
    };
    if (first == std::string::npos || (content[first] != '{' && content[first] != '['))
        return plain();
    auto doc = Json::parse(content, nullptr, false);
    if (doc.is_discarded())
        return plain(); // Markdown links/lists are ordinary text.
    auto blocks = [&](const Json &values) {
        std::string text;
        bool unsupported = false;
        for (const auto &block : values) {
            if (block.is_object() && block.value("type", Json()) == "text" &&
                block.contains("text") && block["text"].is_string()) {
                if (!text.empty())
                    text += '\n';
                text += clipped(str(block, "text"), 32768 - text.size());
            } else
                unsupported = true;
            if (text.size() >= 32768)
                break;
        }
        if (unsupported) {
            text = clipped(std::move(text), 32768 - review.size() - 1);
            if (!text.empty())
                text += '\n';
            text += review;
        }
        return clipped(text, 32768);
    };
    if (doc.is_array() && !doc.empty() && std::all_of(doc.begin(), doc.end(), [](const Json &p) {
            return p.is_object() && p.contains("type");
        }))
        return blocks(doc);
    if (doc.is_object()) {
        auto type = doc.contains("type") && doc["type"].is_string() ? doc["type"].get<std::string>()
                                                                    : std::string();
        if (type == "text" && doc.contains("text") && doc["text"].is_string())
            return clipped(str(doc, "text"), 32768);
        if ((type == "message" || doc.value("role", Json()) == "assistant") &&
            doc.contains("content") && doc["content"].is_array())
            return blocks(doc["content"]);
        if (!type.empty() || doc.contains("tool") || doc.contains("action") ||
            doc.contains("approval") || doc.contains("permission"))
            return review;
    }
    // Canonical output.message is a Markdown string, not a block envelope. Ordinary
    // JSON/code responses without tool/action discriminators remain ordinary text.
    return plain();
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
    }
    if (type.rfind("input.", 0) == 0 && p.contains("userMessage") && p["userMessage"].is_object()) {
        auto doc = p["userMessage"];
        Json parts = Json::array();
        std::size_t bytes = 0;
        if (doc.contains("parts") && doc["parts"].is_array())
            for (const auto &part : doc["parts"]) {
                if (parts.size() >= 128 || bytes >= 32768)
                    break;
                if (str(part, "type") == "text") {
                    auto text = clipped(str(part, "text"), 32768 - bytes);
                    bytes += text.size();
                    parts.push_back({{"type", "text"}, {"text", text}});
                } else
                    parts.push_back({{"type", "unsupported"}});
            }
        payload["userMessage"] = {{"parts", parts}};
    }
    safe["payload"] = std::move(payload);
    return safe;
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
// /tmp is a RAM-backed filesystem on Cardputer. Keep refresh work on disk.
class RefreshFile {
    char path_[sizeof("/var/tmp/okou-zero-refresh-XXXXXX")] = "/var/tmp/okou-zero-refresh-XXXXXX";
    int fd_ = -1;

  public:
    RefreshFile() {
        fd_ = ::mkostemp(path_, O_CLOEXEC);
        if (fd_ < 0)
            throw std::runtime_error("Cannot create temporary chat refresh file");
    }
    RefreshFile(const RefreshFile &) = delete;
    RefreshFile &operator=(const RefreshFile &) = delete;
    ~RefreshFile() {
        if (fd_ >= 0)
            ::close(fd_);
        if (path_[0])
            ::unlink(path_);
    }
    const char *path() const {
        return path_;
    }
    std::FILE *stream() {
        auto *file = ::fdopen(fd_, "w+b");
        if (!file)
            throw std::runtime_error("Cannot open temporary chat snapshot");
        fd_ = -1;
        if (::unlink(path_) == 0)
            path_[0] = '\0';
        return file;
    }
};

class ThreadIndex {
    RefreshFile file_;
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db_{nullptr, sqlite3_close};
    using Statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;

    static void check(int code) {
        if (code != SQLITE_OK)
            throw std::runtime_error("Cannot use temporary chat index");
    }
    Statement statement(const char *sql, std::initializer_list<std::string_view> args = {}) {
        sqlite3_stmt *raw = nullptr;
        const auto code = sqlite3_prepare_v2(db_.get(), sql, -1, &raw, nullptr);
        Statement stmt(raw, sqlite3_finalize);
        check(code);
        int i = 1;
        for (const auto arg : args)
            check(sqlite3_bind_text(stmt.get(), i++, arg.data(), static_cast<int>(arg.size()),
                                    SQLITE_TRANSIENT));
        return stmt;
    }
    static bool row(Statement &stmt) {
        const auto code = sqlite3_step(stmt.get());
        if (code == SQLITE_ROW)
            return true;
        if (code != SQLITE_DONE)
            check(code);
        return false;
    }
    static Json body(Statement &stmt) {
        const auto *value = reinterpret_cast<const char *>(sqlite3_column_text(stmt.get(), 0));
        if (!value)
            throw std::runtime_error("Cannot read temporary chat index");
        return parsed(value);
    }

  public:
    ThreadIndex() {
        sqlite3 *raw = nullptr;
        const auto code = sqlite3_open_v2(file_.path(), &raw, SQLITE_OPEN_READWRITE, nullptr);
        db_.reset(raw);
        check(code);
        check(sqlite3_exec(db_.get(),
                           "PRAGMA cache_size=-512; PRAGMA journal_mode=OFF;"
                           "CREATE TABLE threads(id TEXT PRIMARY KEY, body TEXT NOT NULL,"
                           " sort_at TEXT NOT NULL, visible INTEGER NOT NULL,"
                           " project_id TEXT NOT NULL) WITHOUT ROWID;"
                           "CREATE INDEX thread_order ON threads(visible,sort_at DESC,id);"
                           "CREATE INDEX project_order"
                           " ON threads(project_id,visible,sort_at DESC,id);"
                           "BEGIN",
                           nullptr, nullptr, nullptr));
    }
    void clear() {
        auto stmt = statement("DELETE FROM threads");
        row(stmt);
    }
    void erase(const std::string &id) {
        auto stmt = statement("DELETE FROM threads WHERE id=?1", {id});
        row(stmt);
    }
    Json find(const std::string &id) {
        auto stmt = statement("SELECT body FROM threads WHERE id=?1", {id});
        return row(stmt) ? body(stmt) : Json(nullptr);
    }
    void save(const std::string &id, const Json &value, bool replace) {
        const auto json = value.dump();
        const auto sort_at = str(value, "sortAt");
        const auto project_id = str(value, "projectId");
        const bool visible = !flag(value, "archived") && !flag(value, "hidden");
        auto stmt = statement(
            replace ? "INSERT OR REPLACE INTO threads VALUES(?1,?2,?3,CAST(?4 AS INTEGER),?5)"
                    : "INSERT INTO threads VALUES(?1,?2,?3,CAST(?4 AS INTEGER),?5)",
            {id, json, sort_at, visible ? "1" : "0", project_id});
        row(stmt);
    }
    template <typename Consume> void visible(const std::string &project, Consume consume) {
        const auto limit = std::to_string(max_visible);
        auto stmt = project.empty()
                        ? statement("SELECT body FROM threads WHERE visible=1"
                                    " ORDER BY sort_at DESC,id LIMIT ?1",
                                    {limit})
                        : statement("SELECT body FROM threads WHERE visible=1 AND project_id=?1"
                                    " ORDER BY sort_at DESC,id LIMIT ?2",
                                    {project == "~" ? "" : project, limit});
        while (row(stmt))
            consume(body(stmt));
    }
};

class SnapshotSpool {
    RefreshFile temp_;
    std::unique_ptr<std::FILE, decltype(&std::fclose)> file_{temp_.stream(), std::fclose};

  public:
    std::FILE *file() {
        return file_.get();
    }
    template <typename Consume> Json read(Consume consume) {
        if (std::fflush(file()) != 0 || std::fseek(file(), 0, SEEK_SET) != 0)
            throw std::runtime_error("Cannot read temporary chat snapshot");

        using Event = Json::parse_event_t;
        bool next_is_threads = false, in_threads = false, have_threads = false;
        auto root = Json::parse(file(), [&](int depth, Event event, Json &value) {
            if (depth == 1 && event == Event::key) {
                next_is_threads = value == "chatThreads";
                return next_is_threads || value == "url" || value == "latestSeqId" ||
                       value == "latestEventId";
            }
            if (depth == 1 && event == Event::array_start && next_is_threads) {
                if (have_threads)
                    invalid();
                have_threads = in_threads = true;
            }
            if (depth == 1 && event == Event::array_end)
                in_threads = false;
            if (depth == 2 && in_threads) {
                if (event == Event::value || event == Event::array_start)
                    invalid();
                if (event == Event::object_end) {
                    consume(value);
                    return false;
                }
            }
            return true;
        });
        if (!root.is_object() ||
            (root.contains("chatThreads") &&
             (!have_threads || !root["chatThreads"].is_array() || !root["chatThreads"].empty())))
            invalid();
        return root;
    }
};

void success(const HttpResponse &r) {
    if (r.status < 200 || r.status >= 300)
        throw HttpError(r.status, "HTTP request failed (" + std::to_string(r.status) + ")",
                        r.status >= 500);
}
// Never persist prompts or tokens. Identity records are account/origin scoped.
std::string pending_path(const Config &c, const Tokens &t, const std::string &thread) {
    if (c.data_dir.empty() || t.user_id.empty() || t.org_id.empty())
        throw std::runtime_error(
            "Private data directory and scoped identity required for reliable send");
    std::string scope = c.api_origin + "\n" + t.user_id + "\n" + t.org_id + "\n" + thread;
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char *>(scope.data()), scope.size(), digest);
    static const char *hex = "0123456789abcdef";
    std::string key;
    for (auto b : digest) {
        key += hex[b >> 4];
        key += hex[b & 15];
    }
    return c.data_dir + "/pending-" + key + ".json";
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
// UTF-8 suffix bounded in UTF-16 units, without splitting multibyte characters.
std::string suffix1000(const std::string &s) {
    std::vector<std::pair<std::size_t, int>> chars;
    for (std::size_t i = 0; i < s.size();) {
        unsigned char b = s[i];
        std::size_t n =
            b < 128 ? 1
                    : ((b & 224) == 192 ? 2 : ((b & 240) == 224 ? 3 : ((b & 248) == 240 ? 4 : 0)));
        if (!n || i + n > s.size())
            throw std::runtime_error("Invalid UTF-8 text");
        for (std::size_t k = 1; k < n; ++k)
            if ((static_cast<unsigned char>(s[i + k]) & 192) != 128)
                throw std::runtime_error("Invalid UTF-8 text");
        chars.emplace_back(i, n == 4 ? 2 : 1);
        i += n;
    }
    int units = 0;
    std::size_t start = s.size();
    for (auto it = chars.rbegin(); it != chars.rend() && units + it->second <= 1000; ++it) {
        units += it->second;
        start = it->first;
    }
    return s.substr(start);
}
using Fetch = std::function<Json(const std::string &)>;
struct History {
    std::vector<Json> rows;
    bool found = false;
};
History read_history(const std::string &thread, const Fetch &api, const Fetch &archive,
                     const std::string &looking = {}) {
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
            if (rid == looking && str(row, "eventType") == "input.prompt")
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
    for (const auto &r : h.rows)
        if (str(r, "eventType") == "control.revoke")
            revoked.insert(str(r, "revokesEventId"));
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
        if (type.rfind("input.", 0) == 0) {
            m.role = "user";
            auto doc = p.value("userMessage", Json::object());
            if (doc.is_object() && doc.contains("parts") && doc["parts"].is_array())
                for (const auto &part : doc["parts"]) {
                    if (str(part, "type") == "text")
                        m.text += str(part, "text");
                    else
                        m.text += "\n[Attachment or action: review in Okou Web]";
                }
        } else if (type == "output.message") {
            m.role = "assistant";
            m.text = str(p, "content");
        } else if (type == "output.error") {
            m.role = "system";
            m.text = "Response error. Review in Okou Web.";
        } else if (type == "run.completed" || type == "run.failed" || type == "run.cancelled") {
            // Lifecycle conclusions require the SAME run identity, never indicators.
            if (!run.empty() && runs.count(run)) {
                for (auto index : runs[run])
                    out[index].terminal = true;
                out.push_back({id, "system",
                               type == "run.completed"
                                   ? "Run completed"
                                   : (type == "run.failed" ? "Run failed" : "Run cancelled"),
                               true});
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
HttpResponse Client::snapshot_response(const std::string &url, std::FILE *output) {
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
    auto r = http_.request("GET", url, {}, {}, {}, {}, output);
    success(r);
    return r;
}
Json Client::snapshot(const std::string &url, bool ndjson) {
    auto r = snapshot_response(url);
    if (!ndjson)
        return parsed(r.body);
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
Json Client::indicators() {
    auto result = api("GET", "/api/indicators");
    if (!result.contains("threads") || !result["threads"].is_object() ||
        !result.contains("unreadAt") || !result["unreadAt"].is_object())
        invalid();
    for (auto it = result["threads"].begin(); it != result["threads"].end(); ++it) {
        if (!is_uuid(it.key()) || !it.value().is_string() ||
            (it.value() != "active" && it.value() != "unread"))
            invalid();
    }
    return result;
}
ListState Client::list(bool include_indicators, const std::string &project_filter) {
    ListState out;
    ThreadIndex threads;
    auto index = [&](const Json &item) {
        auto id = str(item, "id"), agent_id = str(item, "agentId"),
             project_id = str(item, "projectId");
        if (!is_uuid(id) || !is_uuid(agent_id) || (!project_id.empty() && !is_uuid(project_id)))
            invalid();
        threads.save(id,
                     {{"id", id},
                      {"agentId", agent_id},
                      {"projectId", project_id},
                      {"title", clipped(str(item, "title"), 512)},
                      {"sortAt", clipped(str(item, "sortAt"), 64)},
                      {"archived", flag(item, "archived")},
                      {"hidden", flag(item, "hidden")},
                      {"muted", flag(item, "muted")},
                      {"pinned", !str(item, "pinnedAt").empty()}},
                     false);
    };
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (attempt)
            threads.clear();
        Json s;
        {
            SnapshotSpool spool;
            success(http_.request("GET", config_.api_origin + "/api/chat-threads/snapshot", {},
                                  tokens_.access_token, {}, {}, spool.file()));
            s = spool.read(index);
        }
        if (s.contains("url") == s.contains("chatThreads"))
            invalid();
        auto n = seq(s, "latestSeqId");
        auto eid = str(s, "latestEventId");
        cursor(n, eid);
        if (s.contains("url")) {
            SnapshotSpool spool;
            snapshot_response(str(s, "url"), spool.file());
            if (!spool.read(index).contains("chatThreads"))
                invalid();
        }
        std::set<std::string> seen;
        if (!eid.empty())
            seen.insert(eid);
        bool done = false;
        try {
            for (std::size_t page = 0; page < max_events; ++page) {
                auto p = api("GET", "/api/chat-threads/events" +
                                        (n ? "?sinceSeqId=" + std::to_string(n) : ""));
                auto events = p.at("events");
                if (!events.is_array())
                    invalid();
                bool more = flag(p, "hasMore");
                if (more && events.empty())
                    invalid();
                for (const auto &e : events) {
                    auto next = seq(e, "seqId");
                    auto id = str(e, "id"), t = str(e, "chatThreadId"), kind = str(e, "kind");
                    if (next <= n || !is_uuid(id) || !is_uuid(t) || !seen.insert(id).second ||
                        seen.size() > max_events)
                        invalid();
                    n = next;
                    if (kind == "deleted") {
                        threads.erase(t);
                        continue;
                    }
                    auto v = threads.find(t);
                    if (v.is_null()) {
                        if (kind != "created")
                            continue;
                        v = {{"id", t}, {"archived", false}};
                    }
                    if (e.contains("title"))
                        v["title"] = clipped(str(e, "title"), 512);
                    for (const char *k : {"agentId", "projectId", "reassignedAgentId"})
                        if (e.contains(k)) {
                            auto value = str(e, k);
                            if (!value.empty() && !is_uuid(value))
                                invalid();
                            if (std::string(k) == "reassignedAgentId")
                                v["agentId"] = value;
                            else
                                v[k] = value;
                        }
                    for (const char *k : {"hidden", "muted"})
                        if (e.contains(k))
                            v[k] = flag(e, k);
                    if (kind == "archived")
                        v["archived"] = true;
                    if (kind == "unarchived")
                        v["archived"] = false;
                    if (kind == "pinned" || kind == "unpinned")
                        v["pinned"] = kind == "pinned";
                    if (kind == "created" || kind == "sort_touched")
                        v["sortAt"] = clipped(str(e, "createdAt"), 64);
                    threads.save(t, v, true);
                }
                if (!more) {
                    done = true;
                    break;
                }
            }
        } catch (const HttpError &e) {
            if (e.status == 410 && attempt == 0)
                continue;
            throw;
        }
        if (!done)
            invalid();
        break;
    }
    auto values = include_indicators ? indicators().at("threads") : Json::object();
    threads.visible(project_filter, [&](const Json &v) {
        Thread t;
        t.id = str(v, "id");
        t.title = str(v, "title");
        t.agent_id = str(v, "agentId");
        t.project_id = str(v, "projectId");
        t.muted = flag(v, "muted");
        t.pinned = flag(v, "pinned");
        auto state = str(values, t.id.c_str());
        t.active = state == "active";
        t.unread = state == "unread" && !t.muted;
        out.threads.push_back(std::move(t));
    });
    auto projects = api("GET", "/api/projects").at("projects");
    if (!projects.is_array() || projects.size() > 4096)
        invalid();
    for (const auto &p : projects) {
        if (out.projects.size() == max_visible)
            break;
        if (!is_uuid(str(p, "id")))
            invalid();
        out.projects.push_back({str(p, "id"), clipped(str(p, "name"), 512)});
    }
    return out;
}
std::vector<Message> Client::history(const std::string &thread) {
    auto path = pending_path(config_, tokens_, thread);
    auto pending = load_pending(path);
    if (!pending.is_null() && str(pending, "threadId") != thread)
        invalid();
    auto looking = pending.is_null() ? std::string() : str(pending, "clientEventId");
    auto result = read_history(
        thread, [&](const std::string &p) { return api("GET", p); },
        [&](const std::string &u) { return snapshot(u, true); }, looking);
    // Reconciliation commits only after the entire cursor chain validates.
    if (result.found)
        std::filesystem::remove(path);
    return present(result);
}
void Client::mark_read(const std::string &thread) {
    auto response = api("POST", tid(thread) + "/mark-read");
    if (!response.contains("lastReadAt") ||
        (!response["lastReadAt"].is_null() && !response["lastReadAt"].is_string()))
        invalid();
    // No optimistic unread clearing. The server watermark and next indicators own read state.
}
bool Client::has_pending_send(const std::string &thread_id) const {
    tid(thread_id);
    auto pending = load_pending(pending_path(config_, tokens_, thread_id));
    if (!pending.is_null() && str(pending, "threadId") != thread_id)
        invalid();
    return !pending.is_null();
}
SendResult Client::send(const Thread &thread, const std::string &text,
                        const std::string &requested_id) {
    auto base = tid(thread.id);
    auto path = pending_path(config_, tokens_, thread.id);
    auto pending = load_pending(path);
    // Reconcile through history(), never submit a second operation while uncertain.
    if (!pending.is_null())
        return {str(pending, "clientEventId"), true};
    if (text.find_first_not_of(" \t\r\n") == std::string::npos || text.size() > 128 * 1024)
        throw std::runtime_error("Enter a bounded nonempty message");
    auto id = requested_id.empty() ? uuid() : requested_id;
    if (!is_uuid(id))
        throw std::runtime_error("Invalid message identity");
    auto metadata = api("GET", base + "/metadata");
    if (str(metadata, "id") != thread.id ||
        (!thread.agent_id.empty() && str(metadata, "agentId") != thread.agent_id) ||
        !is_uuid(str(metadata, "agentId")))
        invalid();
    auto sort = uuid();
    Json body = {{"agentId", str(metadata, "agentId")},
                 {"threadId", thread.id},
                 {"prompt", text},
                 {"clientEventId", id},
                 {"chatThreadSortEventId", sort},
                 {"userMessage",
                  {{"version", 1}, {"parts", Json::array({{{"type", "text"}, {"text", text}}})}}},
                 {"hasTextContent", true},
                 {"computerUseHostId", metadata.value("computerUseHostId", Json(nullptr))},
                 {"cloudBrowserEnabled", flag(metadata, "cloudBrowserEnabled")}};
    save_pending(path,
                 {{"clientEventId", id}, {"chatThreadSortEventId", sort}, {"threadId", thread.id}});
    try {
        auto result = api("POST", "/api/chat/events", body);
        if (str(result, "threadId") != thread.id)
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
std::string Client::transcribe(const std::vector<std::int16_t> &pcm, const std::string &previous,
                               double overlap) {
    if (pcm.empty())
        return {};
    if (pcm.size() > 16000 * 60 || !std::isfinite(overlap) || overlap < 0 || overlap > 2 ||
        overlap > static_cast<double>(pcm.size()) / 16000)
        throw std::runtime_error("Invalid audio segment");
    auto quota = api("GET", "/api/voice-io/quota");
    if (!flag(quota, "allowed"))
        throw std::runtime_error("Voice quota unavailable");
    auto audio = wav(pcm);
    std::string bytes(reinterpret_cast<const char *>(audio.data()), audio.size());
    Json options = {{"previousTranscript", suffix1000(previous)},
                    {"overlapDurationSeconds", overlap},
                    {"totalDurationSeconds", static_cast<double>(pcm.size()) / 16000}};
    Json editor = {{"before", ""}, {"selected", ""}, {"after", ""}};
    std::vector<UploadPart> parts = {{"file", bytes, "segment.wav", "audio/wav"},
                                     {"options", options.dump(), "", "application/json"},
                                     {"editorContext", editor.dump(), "", "application/json"}};
    auto r = http_.request("POST", config_.api_origin + "/api/voice-io/transcribe/segment", "",
                           tokens_.access_token, {{"Accept", "application/json"}}, parts);
    success(r);
    if (r.status == 204)
        return {};
    return str(parsed(r.body), "transcript");
}
std::string Client::polish(const std::vector<std::string> &segments) {
    Json clean = Json::array();
    std::size_t bytes = 0;
    for (const auto &value : segments) {
        auto first = value.find_first_not_of(" \t\r\n");
        if (first == std::string::npos)
            continue;
        auto last = value.find_last_not_of(" \t\r\n");
        auto s = value.substr(first, last - first + 1);
        bytes += s.size() + 1;
        if (bytes > 128 * 1024)
            throw std::runtime_error("Voice draft too large");
        if (clean.size() < 128)
            clean.push_back(s);
        else
            clean[127] = clean[127].get<std::string>() + "\n" + s;
    }
    if (clean.empty())
        return {};
    return str(api("POST", "/api/voice-io/polish/segments", {{"segments", clean}}), "text");
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
