#include "backend.hpp"
#include <curl/curl.h>
#include <glib.h>
#include <libsecret/secret.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <memory>
#include <regex>
#include <sstream>
#include <stdexcept>
#include "mime.hpp"
#include "oauth.hpp"
#include "outgoing.hpp"
#include "storage.hpp"

namespace forma::mail {
namespace {
constexpr size_t limit = 16 * 1024 * 1024;
std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
std::string trim(std::string s) {
    auto a = s.find_first_not_of(" \r\n\t");
    if (a == s.npos) return {};
    return s.substr(a, s.find_last_not_of(" \r\n\t") - a + 1);
}
bool clean(const std::string& s) { return s.find_first_of("\r\n\0", 0, 3) == s.npos; }
std::string unb64(const std::string& s) {
    gsize n = 0;
    auto* p = g_base64_decode(s.c_str(), &n);
    std::string out(reinterpret_cast<char*>(p), n);
    g_free(p);
    return out;
}
std::string utf8(const std::string& s, const std::string& charset) {
    GError* e = nullptr;
    gsize n = 0;
    auto* p = g_convert(s.data(), s.size(), "UTF-8", charset.empty() ? "UTF-8" : charset.c_str(), nullptr, &n, &e);
    if (e) {
        g_error_free(e);
        auto* valid = g_utf8_make_valid(s.data(), s.size());
        std::string o = valid;
        g_free(valid);
        return o;
    }
    std::string o(p, n);
    g_free(p);
    return o;
}
std::string modifiedUTF7(const std::string& raw) {
    std::string out;
    for (size_t i = 0; i < raw.size();) {
        if (raw[i] != '&') {
            out += raw[i++];
            continue;
        }
        auto end = raw.find('-', i);
        if (end == raw.npos) {
            out += raw.substr(i);
            break;
        }
        auto chunk = raw.substr(i + 1, end - i - 1);
        if (chunk.empty())
            out += '&';
        else {
            std::replace(chunk.begin(), chunk.end(), ',', '/');
            while (chunk.size() % 4) chunk += '=';
            out += utf8(unb64(chunk), "UTF-16BE");
        }
        i = end + 1;
    }
    return out;
}
std::string quoted(const std::string& s) {
    if (!clean(s)) throw std::runtime_error("邮箱名称无效");
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out + '"';
}
void validate(const Account& a) {
    auto host = [](const std::string& h) {
        return !h.empty() && h.size() < 254 &&
               std::all_of(
                   h.begin(), h.end(),
                   [](unsigned char c) {
                       return std::isalnum(c) || c == '.' || c == '-' || c == ':' || c == '[' || c == ']';
                   });
    };
    if (!host(a.imapHost) || !host(a.smtpHost) || a.imapPort < 1 || a.imapPort > 65535 || a.smtpPort < 1 ||
        a.smtpPort > 65535 || a.username.empty() || !clean(a.username) || !clean(a.address) ||
        a.address.find('@') == a.address.npos)
        throw std::runtime_error("请填写有效的服务器、端口、登录名和邮件地址");
}
struct Download {
    std::string bytes;
    bool overflow = false;
};
size_t write(char* p, size_t size, size_t n, void* user) {
    auto& d = *static_cast<Download*>(user);
    size_t count = size * n;
    if (d.bytes.size() + count > limit) {
        d.overflow = true;
        return 0;
    }
    d.bytes.append(p, count);
    return count;
}
int progress(void* user, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    return static_cast<std::atomic<bool>*>(user)->load() ? 1 : 0;
}
struct Session {
    CURL* c = nullptr;
    Account account;
    std::atomic<bool>& cancel;
    Session(const Account& a, std::atomic<bool>& token) : account(a), cancel(token) {
        if (account.provider != "password") oauth::refresh(account, cancel);
        static const int init = []() { return curl_global_init(CURL_GLOBAL_DEFAULT); }();
        if (init != 0) throw std::runtime_error("网络组件初始化失败");
        c = curl_easy_init();
        if (!c) throw std::runtime_error("无法创建连接");
        curl_easy_setopt(c, CURLOPT_USERNAME, account.username.c_str());
        if (account.provider == "password")
            curl_easy_setopt(c, CURLOPT_PASSWORD, account.password.c_str());
        else {
            curl_easy_setopt(c, CURLOPT_XOAUTH2_BEARER, account.accessToken.c_str());
            curl_easy_setopt(c, CURLOPT_LOGIN_OPTIONS, "AUTH=XOAUTH2");
        }
        curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
        curl_easy_setopt(c, CURLOPT_TIMEOUT, 45L);
        curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 2L);
        curl_easy_setopt(c, CURLOPT_USE_SSL, static_cast<long>(CURLUSESSL_ALL));
        curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, progress);
        curl_easy_setopt(c, CURLOPT_XFERINFODATA, &cancel);
        curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
        if (const char* ca = std::getenv("SSL_CERT_FILE")) curl_easy_setopt(c, CURLOPT_CAINFO, ca);
    }
    ~Session() {
        if (c) curl_easy_cleanup(c);
    }
    std::string url(bool smtp, const std::string& folder = {}) {
        std::string u = (smtp ? (account.smtpImplicitTLS ? "smtps://" : "smtp://")
                              : (account.imapImplicitTLS ? "imaps://" : "imap://"));
        u += (smtp ? account.smtpHost : account.imapHost) + ":" +
             std::to_string(smtp ? account.smtpPort : account.imapPort) + "/";
        if (!folder.empty()) {
            auto* e = curl_easy_escape(c, folder.c_str(), folder.size());
            u += e;
            curl_free(e);
        }
        return u;
    }
    std::string request(const std::string& folder, const std::string& command) {
        Download d;
        auto u = url(false, folder);
        curl_easy_setopt(c, CURLOPT_URL, u.c_str());
        curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, command.c_str());
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, &d);
        auto code = curl_easy_perform(c);
        if (code != CURLE_OK)
            throw std::runtime_error(d.overflow ? "邮件超过 16 MB，已停止下载"
                                                : std::string("连接或邮件操作失败：") + curl_easy_strerror(code));
        return d.bytes;
    }
};
std::vector<Folder> listFolders(Session& session) {
    auto raw = session.request({}, "LIST \"\" \"*\"");
    std::vector<Folder> out;
    std::istringstream in(raw);
    std::string line;
    std::regex r(R"rx(^\* LIST \(([^)]*)\) (?:"(?:[^"\\]|\\.)*"|NIL) (.+)\r?$)rx", std::regex::icase);
    std::smatch m;
    while (std::getline(in, line)) {
        line = trim(line);
        if (!std::regex_match(line, m, r) || lower(m[1]).find("\\noselect") != std::string::npos) continue;
        auto name = trim(m[2]);
        if (name.size() >= 2 && name.front() == '"' && name.back() == '"') {
            name = name.substr(1, name.size() - 2);
            name = std::regex_replace(name, std::regex(R"(\\(.))"), "$1");
        }
        auto title = modifiedUTF7(name);
        auto flags = lower(m[1]);
        auto low = lower(name);
        if (low == "inbox")
            title = "收件箱";
        else if (flags.find("\\sent") != flags.npos || low == "sent" || low == "sent messages")
            title = "已发送";
        else if (flags.find("\\drafts") != flags.npos || low == "drafts")
            title = "服务器草稿";
        else if (flags.find("\\trash") != flags.npos || low == "trash" || low == "deleted messages")
            title = "废纸篓";
        else if (flags.find("\\junk") != flags.npos || low == "junk" || low == "spam")
            title = "垃圾邮件";
        else if (flags.find("\\archive") != flags.npos || low == "archive")
            title = "归档";
        out.push_back({name, title, 0});
    }
    if (std::none_of(out.begin(), out.end(), [](auto& f) { return lower(f.id) == "inbox"; }))
        out.insert(out.begin(), {"INBOX", "收件箱", 0});
    std::stable_sort(out.begin(), out.end(),
                     [](auto& a, auto& b) { return lower(a.id) == "inbox" && lower(b.id) != "inbox"; });
    return out;
}
std::vector<Message> fetchFolder(Session& session, const std::string& folder, size_t maximum = 40) {
    auto result = session.request(folder, "UID SEARCH ALL");
    std::vector<std::string> ids;
    std::istringstream in(result);
    std::string line;
    while (std::getline(in, line)) {
        if (line.starts_with("* SEARCH")) {
            std::istringstream words(line.substr(8));
            std::string id;
            while (words >> id)
                if (std::all_of(id.begin(), id.end(), [](unsigned char c) { return std::isdigit(c); }))
                    ids.push_back(id);
        }
    }
    if (ids.size() > maximum) ids.erase(ids.begin(), ids.end() - static_cast<std::ptrdiff_t>(maximum));
    std::vector<Message> out;
    for (auto it = ids.rbegin(); it != ids.rend(); ++it) {
        if (session.cancel.load()) throw std::runtime_error("操作已取消");
        auto raw = session.request(
            folder, "UID FETCH " + *it + " (BODY.PEEK[])");  // libcurl returns the single literal payload.
        // libcurl custom FETCH may include IMAP framing around the literal.
        if (raw.starts_with("* ")) {
            static const std::regex literal(R"(^\* [0-9]+ FETCH [^\r\n]*\{([0-9]+)\}\r?\n)", std::regex::icase);
            std::smatch framing;
            if (std::regex_search(raw, framing, literal)) {
                auto length = std::stoull(framing[1].str());
                auto start = static_cast<size_t>(framing.length());
                if (length > limit || length > raw.size() - start) throw std::runtime_error("邮件传输不完整");
                raw = raw.substr(start, static_cast<size_t>(length));
            }
        }
        Message msg;
        try {
            msg = Backend::parseMessage(raw, *it);
        } catch (const std::exception& e) {
            // Keep the raw message for diagnosis; the user's cache directory is private.
            if (const char* dir = std::getenv("FORMA_MAIL_DEBUG_RAW")) {
                std::error_code ec;
                std::filesystem::create_directories(dir, ec);
                std::ofstream(dir + std::string("/mail-") + *it + ".eml", std::ios::binary).write(raw.data(), raw.size());
            }
            msg.id = *it;
            msg.subject = "（无法解析的邮件：" + std::string(e.what()) + "）";
            msg.body = "这封邮件的编码或格式不受支持。原始邮件仍保留在服务器，未执行任何内容。\n\n解析错误：" +
                       std::string(e.what());
            msg.preview = msg.body;
        }
        msg.folder = folder;
        out.push_back(std::move(msg));
    }
    if (!ids.empty()) {
        auto flags = session.request(folder, "UID FETCH " + ids.front() + ":" + ids.back() + " (UID FLAGS)");
        std::istringstream lines(flags);
        std::regex uid(R"(UID ([0-9]+))", std::regex::icase);
        while (std::getline(lines, line)) {
            std::smatch m;
            if (!std::regex_search(line, m, uid)) continue;
            auto it = std::find_if(out.begin(), out.end(), [&](auto& msg) { return msg.id == m[1]; });
            if (it != out.end()) {
                it->unread = line.find("\\Seen") == line.npos;
                it->star = line.find("\\Flagged") != line.npos;
            }
        }
    }
    return out;
}
const SecretSchema* schema() {
    static const SecretSchema s = {"io.github.skjsbsnq.Forma.Mail",
                                   SECRET_SCHEMA_NONE,
                                   {{"username", SECRET_SCHEMA_ATTRIBUTE_STRING},
                                    {"server", SECRET_SCHEMA_ATTRIBUTE_STRING},
                                    {nullptr, SECRET_SCHEMA_ATTRIBUTE_STRING}},
                                   0,
                                   nullptr,
                                   nullptr,
                                   nullptr,
                                   nullptr,
                                   nullptr,
                                   nullptr,
                                   nullptr};
    return &s;
}
std::string password(Account& a) {
    if (!a.password.empty()) return {};
    GError* e = nullptr;
    gchar* p = secret_password_lookup_sync(schema(), nullptr, &e, "username", a.username.c_str(), "server",
                                           a.imapHost.c_str(), nullptr);
    if (p) {
        a.password = p;
        secret_password_free(p);
    }
    if (e) g_error_free(e);
    return a.password.empty() ? "请输入邮箱密码或应用专用密码（授权码）" : std::string{};
}
std::string remember(const Account& a) {
    if (!a.rememberPassword) return {};
    GError* e = nullptr;
    bool ok =
        secret_password_store_sync(schema(), SECRET_COLLECTION_DEFAULT, "Forma 邮件 · 应用密码", a.password.c_str(),
                                   nullptr, &e, "username", a.username.c_str(), "server", a.imapHost.c_str(), nullptr);
    if (e) g_error_free(e);
    return ok ? std::string{} : "已连接，但无法将密码存入系统钥匙环；本次会话仍可使用";
}
void protectSettings(mayo::Settings& settings) {
    std::error_code error;
    std::filesystem::permissions(settings.directory(), std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace, error);
    std::filesystem::permissions(settings.filePath(),
                                 std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                                 std::filesystem::perm_options::replace, error);
}
std::string newId() { return std::to_string(std::chrono::system_clock::now().time_since_epoch().count()); }
}  // namespace

Message Backend::parseMessage(std::string raw, std::string id) {
    return parseMimeMessage(std::move(raw), std::move(id));
}
std::string Backend::makeOutgoing(const Account& account, const Outgoing& message) {
    return buildOutgoing(account, message);
}
Backend::Backend(mayo::App& app, mayo::Settings& s) : app_(app), settings_(s) {
    account_.name = s.getString("account.name");
    account_.address = s.getString("account.address");
    account_.username = s.getString("account.username");
    account_.imapHost = s.getString("account.imapHost");
    account_.smtpHost = s.getString("account.smtpHost");
    account_.imapPort = s.getInt("account.imapPort", 993);
    account_.smtpPort = s.getInt("account.smtpPort", 465);
    account_.imapImplicitTLS = s.getBool("account.imapTLS", true);
    account_.smtpImplicitTLS = s.getBool("account.smtpTLS", true);
    account_.rememberPassword = s.getBool("account.remember", false);
    account_.provider = s.getString("account.provider", "password");
    account_.clientId = s.getString("account.clientId");
    account_.tenant = s.getString("account.tenant", "common");
    readOnly_ = std::getenv("MAYO_SCREENSHOT") != nullptr || std::getenv("FORMA_MAIL_UI_TEST") != nullptr;
    loadDrafts();
    if (account_.address.empty())
        useDemo();
    else {
        demo_ = false;
        folder_ = s.getString("account.lastFolder", "INBOX");
        int count = std::clamp(s.getInt("account.folders.count"), 0, 500);
        for (int i = 0; i < count; ++i) {
            auto key = "account.folders." + std::to_string(i);
            auto id = s.getString(key + ".id");
            if (!id.empty()) folders_.push_back({id, s.getString(key + ".title", id), 0});
        }
        if (folders_.empty()) folders_.push_back({"INBOX", "收件箱", 0});
        try {
            messages_ = storage::loadCache(settings_.directory(), account_, folder_);
        } catch (const std::exception& e) {
            warning_ = std::string("缓存读取失败，原文件已保留：") + e.what();
        }
        updateUnread();
    }
}
Backend::~Backend() {
    shuttingDown_ = true;
    cancelled_ = true;
    if (worker_.joinable()) worker_.join();
}
void Backend::run(std::function<std::string()> work, std::function<void()> apply, Done done) {
    if (busy_) {
        if (done) done("当前有操作正在进行，请稍候");
        return;
    }
    busy_ = true;
    cancelled_ = false;
    if (worker_.joinable()) worker_.join();
    worker_ = std::jthread([this, work = std::move(work), apply = std::move(apply), done = std::move(done)] {
        std::string error;
        try {
            error = work();
        } catch (const std::exception& e) {
            error = e.what();
        }
        if (shuttingDown_) return;
        if (cancelled_) error = "操作已取消";
        app_.post([this, error = std::move(error), apply, done] {
            busy_ = false;
            if (error.empty() && apply) {
                apply();
                updateUnread();
                if (!demo_) saveCurrentCache();
            }
            if (done) done(error);
        });
    });
}
void Backend::updateUnread() {
    for (auto& folder : folders_) {
        if (!demo_ && folder.id != folder_) continue;
        folder.unread = static_cast<int>(std::count_if(messages_.begin(), messages_.end(), [&](const Message& message) {
            return message.folder == folder.id && message.unread;
        }));
    }
}
void Backend::saveProfile() {
    settings_.set("account.provider", account_.provider);
    settings_.set("account.clientId", account_.clientId);
    settings_.set("account.tenant", account_.tenant);
    settings_.set("account.lastFolder", folder_);
    settings_.set("account.folders.count", static_cast<int>(folders_.size()));
    for (size_t i = 0; i < folders_.size(); ++i) {
        auto key = "account.folders." + std::to_string(i);
        settings_.set(key + ".id", folders_[i].id);
        settings_.set(key + ".title", folders_[i].title);
    }
    settings_.set("account.name", account_.name);
    settings_.set("account.address", account_.address);
    settings_.set("account.username", account_.username);
    settings_.set("account.imapHost", account_.imapHost);
    settings_.set("account.smtpHost", account_.smtpHost);
    settings_.set("account.imapPort", account_.imapPort);
    settings_.set("account.smtpPort", account_.smtpPort);
    settings_.set("account.imapTLS", account_.imapImplicitTLS);
    settings_.set("account.smtpTLS", account_.smtpImplicitTLS);
    settings_.set("account.remember", account_.rememberPassword);
    settings_.save();
    protectSettings(settings_);
}
void Backend::useDemo() {
    if (busy_) return;
    connected_ = false;
    demo_ = true;
    folder_ = "INBOX";
    folders_ = {{"INBOX", "收件箱", 2}, {"Archive", "归档", 0}, {"Trash", "废纸篓", 0}};
    messages_ = {
        {"demo1",
         "INBOX",
         "Forma 设计团队",
         "design@example.invalid",
         "欢迎使用 Forma 邮件",
         "这是一封本地示例邮件。",
         "这是一封本地示例邮件。\n\n你可以浏览示例内容，也可以从账户设置连接自己的 IMAP / SMTP "
         "邮箱。示例邮件不会发送到外部。",
         "今天 09:41",
         true,
         true,
         {}},
        {"demo2",
         "INBOX",
         "林予安",
         "yuan@example.invalid",
         "周末的阅读清单",
         "附上一份简短的阅读清单。",
         "你好，\n\n这周读了几篇有关字体与界面设计的文章，整理了一份清单。\n\n等你有空，我们一起聊聊。\n\n予安",
         "今天 08:30",
         true,
         false,
         {}},
        {"demo3",
         "INBOX",
         "项目协作",
         "team@example.invalid",
         "Re: 下一版界面评审",
         "阅读区与工具栏的调整建议。",
         "关于下一版界面，我的建议是优先保证内容的可读性，让工具栏保持紧凑。\n\n这是一封可离线浏览的示例。",
         "昨天",
         false,
         false,
         {}}};
}
void Backend::connect(Account a, Done done) {
    auto fs = std::make_shared<std::vector<Folder>>();
    auto msgs = std::make_shared<std::vector<Message>>();
    auto profile = std::make_shared<Account>(std::move(a));
    auto warning = std::make_shared<std::string>();
    run(
        [this, profile, fs, msgs, warning] {
            validate(*profile);
            if (profile->provider == "password") {
                auto e = password(*profile);
                if (!e.empty()) return e;
            } else
                oauth::refresh(*profile, cancelled_);
            Session s(*profile, cancelled_);
            *fs = listFolders(s);
            *msgs = fetchFolder(s, "INBOX");  // A keyring failure must not invalidate an otherwise working account.
            *warning = profile->provider == "password" ? remember(*profile) : oauth::remember(*profile);
            return std::string{};
        },
        [this, profile, fs, msgs, warning] {
            warning_ = *warning;
            account_ = *profile;
            folders_ = *fs;
            messages_ = *msgs;
            folder_ = "INBOX";
            connected_ = true;
            demo_ = false;
            saveProfile();
            saveCurrentCache();
        },
        std::move(done));
}
void Backend::disconnect() {
    if (busy_) return;
    account_.password.clear();
    connected_ = false;
    demo_ = false;
    messages_.clear();
    folders_.clear();
}
void Backend::sync(Done done) { loadFolder(folder_, std::move(done)); }
void Backend::loadFolder(const std::string& id, Done done) {
    if (demo_) {
        folder_ = id;
        if (done) done({});
        return;
    }
    if (!connected_) {
        try {
            messages_ = storage::loadCache(settings_.directory(), account_, id);
            folder_ = id;
            updateUnread();
            if (done) done({});
        } catch (const std::exception& error) {
            if (done) done(error.what());
        }
        return;
    }
    auto msgs = std::make_shared<std::vector<Message>>();
    auto account = account_;
    auto maximum = messageLimit_;
    run(
        [this, account, id, msgs, maximum] {
            Session s(account, cancelled_);
            *msgs = fetchFolder(s, id, maximum);
            return std::string{};
        },
        [this, id, msgs] {
            folder_ = id;
            messages_ = *msgs;
            saveCurrentCache();
            saveProfile();
        },
        std::move(done));
}
void Backend::setRead(const std::string& id, bool read, Done done) {
    auto apply = [this, id, read] {
        for (auto& m : messages_)
            if (m.id == id) m.unread = !read;
    };
    if (demo_) {
        apply();
        updateUnread();
        if (done) done({});
        return;
    }
    if (!connected_) {
        if (done) done("请先连接邮箱账户");
        return;
    }
    if (id.empty() || id.find_first_not_of("0123456789") != id.npos) {
        if (done) done("邮件 UID 无效");
        return;
    }
    auto a = account_;
    auto f = folder_;
    run(
        [this, a, f, id, read] {
            Session s(a, cancelled_);
            s.request(f, "UID STORE " + id + (read ? " +FLAGS.SILENT (\\Seen)" : " -FLAGS.SILENT (\\Seen)"));
            return std::string{};
        },
        apply, std::move(done));
}
void Backend::setStar(const std::string& id, bool star, Done done) {
    auto apply = [this, id, star] {
        for (auto& m : messages_)
            if (m.id == id) m.star = star;
    };
    if (demo_) {
        apply();
        updateUnread();
        if (done) done({});
        return;
    }
    if (!connected_) {
        if (done) done("请先连接邮箱账户");
        return;
    }
    if (id.empty() || id.find_first_not_of("0123456789") != id.npos) {
        if (done) done("邮件 UID 无效");
        return;
    }
    auto a = account_;
    auto f = folder_;
    run(
        [this, a, f, id, star] {
            Session s(a, cancelled_);
            s.request(f, "UID STORE " + id + (star ? " +FLAGS.SILENT (\\Flagged)" : " -FLAGS.SILENT (\\Flagged)"));
            return std::string{};
        },
        apply, std::move(done));
}
void Backend::move(const std::string& id, const std::string& dest, Done done) {
    auto apply = [this, id, dest] {
        if (demo_) {
            for (auto& m : messages_)
                if (m.id == id) m.folder = dest;
        } else
            std::erase_if(messages_, [&](auto& m) { return m.id == id; });
    };
    if (demo_) {
        apply();
        updateUnread();
        if (done) done({});
        return;
    }
    if (!connected_) {
        if (done) done("请先连接邮箱账户");
        return;
    }
    if (id.empty() || id.find_first_not_of("0123456789") != id.npos) {
        if (done) done("邮件 UID 无效");
        return;
    }
    auto a = account_;
    auto f = folder_;
    run(
        [this, a, f, id, dest] {
            Session s(a, cancelled_);
            s.request(f, "UID MOVE " + id + " " + quoted(dest));
            return std::string{};
        },
        apply, std::move(done));
}
void Backend::send(Outgoing msg, Done done) {
    if (!connected_ || demo_) {
        if (done) done("尚未连接真实账户，邮件未发送；可以保存为草稿");
        return;
    }
    auto a = account_;
    run(
        [this, a, msg] {
            auto bytes = makeOutgoing(a, msg);
            Session s(a, cancelled_);
            auto url = s.url(true);
            curl_easy_setopt(s.c, CURLOPT_URL, url.c_str());
            curl_easy_setopt(s.c, CURLOPT_MAIL_FROM, a.address.c_str());
            curl_slist* recipients = nullptr;
            for (const auto& address : recipientAddresses(msg))
                recipients = curl_slist_append(recipients, address.c_str());
            std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> list(recipients, curl_slist_free_all);
            curl_easy_setopt(s.c, CURLOPT_MAIL_RCPT, list.get());
            struct Payload {
                const std::string& data;
                size_t pos = 0;
            } payload{bytes};
            curl_easy_setopt(s.c, CURLOPT_UPLOAD, 1L);
            curl_easy_setopt(
                s.c, CURLOPT_READFUNCTION, +[](char* target, size_t size, size_t count, void* user) -> size_t {
                    auto& p = *static_cast<Payload*>(user);
                    auto n = std::min(size * count, p.data.size() - p.pos);
                    std::copy_n(p.data.data() + p.pos, n, target);
                    p.pos += n;
                    return n;
                });
            curl_easy_setopt(s.c, CURLOPT_READDATA, &payload);
            curl_easy_setopt(s.c, CURLOPT_INFILESIZE_LARGE, static_cast<curl_off_t>(bytes.size()));
            auto code = curl_easy_perform(s.c);
            if (code != CURLE_OK) return std::string("发送失败，草稿仍保留：") + curl_easy_strerror(code);
            return std::string{};
        },
        {}, std::move(done));
}
void Backend::loadDrafts() {
    try {
        if (std::filesystem::exists(std::filesystem::path(settings_.directory()) / "drafts.json")) {
            drafts_ = storage::loadDrafts(settings_.directory());
            return;
        }
    } catch (const std::exception& error) {
        draftStoreHealthy_ = false;
        draftError_ = std::string("草稿读取失败，已保留原文件：") + error.what();
        return;
    }
    auto count = std::clamp(settings_.getInt("drafts.count"), 0, 200);
    for (int i = 0; i < count; ++i) {
        auto p = "drafts." + std::to_string(i) + ".";
        Draft d{settings_.getString(p + "id"),
                settings_.getString(p + "to"),
                settings_.getString(p + "subject"),
                unb64(settings_.getString(p + "body")),
                {},
                {},
                {}};
        if (!d.id.empty()) drafts_.push_back(d);
    }
    if (drafts_.empty() && !settings_.getString("draft.body").empty()) {
        drafts_.push_back({newId(),
                           settings_.getString("draft.to"),
                           settings_.getString("draft.subject"),
                           settings_.getString("draft.body"),
                           {},
                           {},
                           {}});
        persistDrafts();
    }
}
void Backend::persistDrafts() {
    if (readOnly_ || !draftStoreHealthy_) return;
    try {
        storage::saveDrafts(settings_.directory(), drafts_);
        draftError_.clear();
    } catch (const std::exception& error) {
        draftError_ = std::string("草稿未能保存：") + error.what();
    }
}
void Backend::saveCurrentCache() {
    if (readOnly_ || demo_ || account_.address.empty()) return;
    try {
        storage::saveCache(settings_.directory(), account_, folder_, messages_);
    } catch (const std::exception& error) {
        warning_ = std::string("邮件已加载，但无法更新离线缓存：") + error.what();
    }
}
void Backend::loadMore(Done done) {
    messageLimit_ = std::min(200, messageLimit_ + 40);
    sync(std::move(done));
}
void Backend::signIn(Account account, ShowAuth show, Done done) {
    auto profile = std::make_shared<Account>(std::move(account));
    auto folders = std::make_shared<std::vector<Folder>>();
    auto messages = std::make_shared<std::vector<Message>>();
    auto warning = std::make_shared<std::string>();
    run(
        [this, profile, show, folders, messages, warning] {
            validate(*profile);
            oauth::authorize(*profile, cancelled_, [this, show](const AuthPrompt& prompt) {
                app_.post([show, prompt] {
                    if (show) show(prompt);
                });
            });
            *warning = oauth::remember(*profile);
            Session session(*profile, cancelled_);
            *folders = listFolders(session);
            *messages = fetchFolder(session, "INBOX");
            return std::string{};
        },
        [this, profile, folders, messages, warning] {
            account_ = *profile;
            folders_ = *folders;
            messages_ = *messages;
            folder_ = "INBOX";
            demo_ = false;
            connected_ = true;
            warning_ = *warning;
            saveProfile();
            saveCurrentCache();
        },
        std::move(done));
}
std::string Backend::saveDraft(Draft d) {
    if (d.id.empty()) d.id = newId();
    auto id = d.id;
    auto i = std::find_if(drafts_.begin(), drafts_.end(), [&](auto& v) { return v.id == id; });
    if (i == drafts_.end())
        drafts_.push_back(std::move(d));
    else
        *i = std::move(d);
    persistDrafts();
    return id;
}
void Backend::removeDraft(const std::string& id) {
    std::erase_if(drafts_, [&](auto& d) { return d.id == id; });
    persistDrafts();
}
}  // namespace forma::mail
