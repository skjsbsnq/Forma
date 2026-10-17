#include "oauth.hpp"
#include <curl/curl.h>
#include <glib.h>
#include <json-glib/json-glib.h>
#include <libsecret/secret.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <unistd.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <ctime>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace forma::mail::oauth {
namespace {
std::mutex tokenMutex;
std::map<std::string, Account> sessionTokens;
std::string identity(const Account& a) { return a.provider + "\n" + a.clientId + "\n" + a.username; }
void cacheTokens(const Account& a) {
    std::lock_guard lock(tokenMutex);
    sessionTokens[identity(a)] = a;
}
using Clock = std::chrono::steady_clock;
struct Parser {
    JsonParser* p = json_parser_new();
    ~Parser() { g_object_unref(p); }
    JsonObject* parse(const std::string& s) {
        GError* error = nullptr;
        if (!json_parser_load_from_data(p, s.data(), s.size(), &error)) {
            if (error) g_error_free(error);
            throw std::runtime_error("授权服务器返回了无效数据");
        }
        auto* root = json_parser_get_root(p);
        if (!JSON_NODE_HOLDS_OBJECT(root)) throw std::runtime_error("授权服务器返回了无效数据");
        return json_node_get_object(root);
    }
};
std::string member(JsonObject* o, const char* name) {
    if (!json_object_has_member(o, name)) return {};
    auto* n = json_object_get_member(o, name);
    if (json_node_get_value_type(n) != G_TYPE_STRING) return {};
    return json_node_get_string(n);
}
long integer(JsonObject* o, const char* name, long fallback = 0) {
    if (!json_object_has_member(o, name)) return fallback;
    auto* n = json_object_get_member(o, name);
    if (json_node_get_value_type(n) != G_TYPE_INT64 && json_node_get_value_type(n) != G_TYPE_DOUBLE) return fallback;
    return static_cast<long>(json_node_get_int(n));
}
std::string encode(const std::string& s) {
    auto* p = g_uri_escape_string(s.c_str(), nullptr, false);
    std::string out = p;
    g_free(p);
    return out;
}
std::string decode(const std::string& s) {
    auto* p = g_uri_unescape_string(s.c_str(), nullptr);
    if (!p) return {};
    std::string out = p;
    g_free(p);
    return out;
}
std::string form(const std::map<std::string, std::string>& values) {
    std::string out;
    for (auto& [k, v] : values) {
        if (!out.empty()) out += '&';
        out += encode(k) + "=" + encode(v);
    }
    return out;
}
std::map<std::string, std::string> params(const std::string& query) {
    std::map<std::string, std::string> out;
    size_t at = 0;
    while (at < query.size()) {
        auto end = query.find('&', at);
        auto item = query.substr(at, end == query.npos ? query.npos : end - at);
        auto equal = item.find('=');
        if (equal != item.npos) out[decode(item.substr(0, equal))] = decode(item.substr(equal + 1));
        if (end == query.npos) break;
        at = end + 1;
    }
    return out;
}
std::string randomString() {
    unsigned char bytes[32];
    size_t at = 0;
    while (at < sizeof bytes) {
        auto n = getrandom(bytes + at, sizeof bytes - at, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            throw std::runtime_error("无法生成安全授权随机数");
        }
        at += static_cast<size_t>(n);
    }
    auto* p = g_base64_encode(bytes, sizeof bytes);
    std::string out = p;
    g_free(p);
    std::replace(out.begin(), out.end(), '+', '-');
    std::replace(out.begin(), out.end(), '/', '_');
    while (out.ends_with('=')) out.pop_back();
    return out;
}
std::string challenge(const std::string& verifier) {
    auto* hash = g_checksum_new(G_CHECKSUM_SHA256);
    g_checksum_update(hash, reinterpret_cast<const guchar*>(verifier.data()), verifier.size());
    guint8 digest[32];
    gsize size = 32;
    g_checksum_get_digest(hash, digest, &size);
    g_checksum_free(hash);
    auto* b = g_base64_encode(digest, size);
    std::string out = b;
    g_free(b);
    std::replace(out.begin(), out.end(), '+', '-');
    std::replace(out.begin(), out.end(), '/', '_');
    while (out.ends_with('=')) out.pop_back();
    return out;
}
void sleep(std::atomic<bool>& cancel, int seconds) {
    for (int i = 0; i < seconds * 10; ++i) {
        if (cancel) throw std::runtime_error("授权已取消");
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}
size_t response(char* bytes, size_t size, size_t count, void* user) {
    auto& out = *static_cast<std::string*>(user);
    size_t n = size * count;
    if (out.size() + n > 1024 * 1024) return 0;
    out.append(bytes, n);
    return n;
}
int progress(void* data, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    return static_cast<std::atomic<bool>*>(data)->load() ? 1 : 0;
}
std::string post(const std::string& url, const std::map<std::string, std::string>& fields, std::atomic<bool>& cancel) {
    static int init = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (init) throw std::runtime_error("无法初始化授权网络");
    if (!url.starts_with("https://")) throw std::runtime_error("授权端点必须使用 HTTPS");
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> c(curl_easy_init(), curl_easy_cleanup);
    if (!c) throw std::runtime_error("无法创建授权连接");
    std::string body = form(fields), out;
    curl_easy_setopt(c.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(c.get(), CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(c.get(), CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    curl_easy_setopt(c.get(), CURLOPT_WRITEFUNCTION, response);
    curl_easy_setopt(c.get(), CURLOPT_WRITEDATA, &out);
    curl_easy_setopt(c.get(), CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(c.get(), CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(c.get(), CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c.get(), CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(c.get(), CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(c.get(), CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c.get(), CURLOPT_XFERINFOFUNCTION, progress);
    curl_easy_setopt(c.get(), CURLOPT_XFERINFODATA, &cancel);
    curl_easy_setopt(c.get(), CURLOPT_PROTOCOLS_STR, "https");
    if (const char* ca = std::getenv("SSL_CERT_FILE")) curl_easy_setopt(c.get(), CURLOPT_CAINFO, ca);
    auto code = curl_easy_perform(c.get());
    if (code != CURLE_OK) throw std::runtime_error(cancel ? "授权已取消" : "无法连接授权服务器，请检查网络或代理");
    long status = 0;
    curl_easy_getinfo(c.get(), CURLINFO_RESPONSE_CODE, &status);
    if (status == 429) throw std::runtime_error("授权请求过于频繁，请稍后重试");
    if (status >= 500) throw std::runtime_error("授权服务器暂时不可用");
    return out;
}
std::string errorText(const std::string& error) {
    if (error == "invalid_client" || error == "unauthorized_client")
        return "OAuth 客户端配置无效：请检查已注册的桌面/公共客户端 ID";
    if (error == "invalid_grant") return "授权已过期或被撤销，请重新登录";
    if (error == "invalid_scope") return "OAuth 应用未配置所需邮件权限，请检查应用注册";
    if (error == "access_denied" || error == "authorization_declined") return "用户或组织管理员拒绝了邮件权限授权";
    if (error == "expired_token") return "授权已超时，请重新登录";
    if (!error.empty()) return "授权未完成（" + error + "），请检查客户端配置、账户权限或重新登录";
    return "授权未完成，请检查客户端配置、账户权限或重新登录";
}
void tokens(Account& account, JsonObject* object) {
    auto error = member(object, "error");
    if (!error.empty()) {
        auto detail = member(object, "error_description");
        throw std::runtime_error(errorText(error) + (detail.empty() ? "" : "（" + detail + "）"));
    }
    auto access = member(object, "access_token");
    if (access.empty()) throw std::runtime_error("授权服务器未返回访问令牌");
    account.accessToken = access;
    auto refresh = member(object, "refresh_token");
    if (!refresh.empty()) account.refreshToken = refresh;
    auto expires = std::clamp(integer(object, "expires_in", 3600), 60L, 86400L);
    account.tokenExpires = std::time(nullptr) + expires;
}
const SecretSchema* schema() {
    static const SecretSchema s = {"io.github.skjsbsnq.Forma.Mail.OAuth",
                                   SECRET_SCHEMA_NONE,
                                   {{"username", SECRET_SCHEMA_ATTRIBUTE_STRING},
                                    {"provider", SECRET_SCHEMA_ATTRIBUTE_STRING},
                                    {"client", SECRET_SCHEMA_ATTRIBUTE_STRING},
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
void lookup(Account& a) {
    GError* error = nullptr;
    auto* value = secret_password_lookup_sync(schema(), nullptr, &error, "username", a.username.c_str(), "provider",
                                              a.provider.c_str(), "client", a.clientId.c_str(), nullptr);
    if (value) {
        try {
            Parser parser;
            auto* o = parser.parse(value);
            a.refreshToken = member(o, "refresh_token");
            if (a.clientSecret.empty()) a.clientSecret = member(o, "client_secret");
        } catch (...) {
        }
        secret_password_free(value);
    }
    if (error) g_error_free(error);
}
struct Socket {
    int value = -1;
    ~Socket() {
        if (value >= 0) ::close(value);
    }
};
std::string awaitCode(const std::string& state, std::atomic<bool>& cancel, const Backend::ShowAuth& show,
                      const std::string& base, const std::map<std::string, std::string>& fields,
                      std::string& redirect) {
    Socket server;
    server.value = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (server.value < 0) throw std::runtime_error("无法启动本机授权回调");
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(server.value, reinterpret_cast<sockaddr*>(&addr), sizeof addr) || listen(server.value, 4))
        throw std::runtime_error("无法监听本机授权回调");
    socklen_t n = sizeof addr;
    getsockname(server.value, reinterpret_cast<sockaddr*>(&addr), &n);
    auto host = "127.0.0.1:" + std::to_string(ntohs(addr.sin_port));
    redirect = "http://" + host + "/oauth/callback";
    auto query = fields;
    query["redirect_uri"] = redirect;
    show({base + "?" + form(query), {}, "请在浏览器完成 Google 登录，授权结果将返回本机应用。"});
    auto until = Clock::now() + std::chrono::minutes(5);
    while (Clock::now() < until) {
        if (cancel) throw std::runtime_error("授权已取消");
        pollfd fd{server.value, POLLIN, 0};
        if (poll(&fd, 1, 200) <= 0) continue;
        Socket client;
        client.value = accept4(server.value, nullptr, nullptr, SOCK_CLOEXEC);
        if (client.value < 0) continue;
        std::string request;
        auto deadline = Clock::now() + std::chrono::seconds(3);
        while (request.find("\r\n\r\n") == request.npos && request.size() < 8192 && Clock::now() < deadline) {
            pollfd readable{client.value, POLLIN, 0};
            if (poll(&readable, 1, 100) <= 0) {
                if (cancel) throw std::runtime_error("授权已取消");
                continue;
            }
            char buf[1024];
            auto got = recv(client.value, buf, sizeof buf, 0);
            if (got <= 0) break;
            request.append(buf, got);
        }
        auto end = request.find(" HTTP/");
        bool valid = request.starts_with("GET /oauth/callback?") && end != request.npos &&
                     request.find("\r\nHost: " + host + "\r\n") != request.npos;
        std::map<std::string, std::string> data;
        if (valid) {
            auto start = request.find('?') + 1;
            data = params(request.substr(start, end - start));
            valid = data["state"] == state;
        }
        std::string page = valid ? "Authorization received. You can close this tab and return to Forma Mail."
                                 : "Invalid authorization request.";
        std::string reply =
            std::string("HTTP/1.1 ") + (valid ? "200 OK" : "400 Bad Request") +
            "\r\nContent-Type: text/plain; charset=utf-8\r\nCache-Control: no-store\r\nContent-Security-Policy: "
            "default-src 'none'\r\nConnection: close\r\nContent-Length: " +
            std::to_string(page.size()) + "\r\n\r\n" + page;
        ::send(client.value, reply.data(), reply.size(), MSG_NOSIGNAL);
        if (!valid) continue;
        if (!data["error"].empty()) throw std::runtime_error(errorText(data["error"]));
        if (data["code"].empty()) throw std::runtime_error("授权回调中缺少授权码");
        return data["code"];
    }
    throw std::runtime_error("等待浏览器授权超时，请重新登录");
}
}  // namespace
Endpoints endpoints(const Account& a) {
    if (a.provider == "google")
        return {"https://accounts.google.com/o/oauth2/v2/auth",
                "https://oauth2.googleapis.com/token",
                {},
                "https://mail.google.com/"};
    if (a.provider == "microsoft") {
        auto tenant = a.tenant.empty() ? "common" : a.tenant;
        if (tenant.size() > 100 || !std::all_of(tenant.begin(), tenant.end(), [](unsigned char c) {
                return std::isalnum(c) || c == '-' || c == '.';
            }))
            throw std::runtime_error("Microsoft 租户标识无效");
        auto base = "https://login.microsoftonline.com/" + tenant + "/oauth2/v2.0/";
        return {base + "authorize", base + "token", base + "devicecode",
                "offline_access https://outlook.office.com/IMAP.AccessAsUser.All https://outlook.office.com/SMTP.Send"};
    }
    throw std::runtime_error("未选择受支持的 OAuth 服务商");
}
void authorize(Account& account, std::atomic<bool>& cancel, const Backend::ShowAuth& show, const Endpoints* testing) {
    if (account.clientId.empty())
        throw std::runtime_error("请先填写已注册的 OAuth 客户端 ID；账户密码不能替代客户端注册");
    auto e = testing ? *testing : endpoints(account);
    if (account.provider == "google") {
        auto verifier = randomString(), state = randomString();
        std::string redirect;
        auto code = awaitCode(state, cancel, show, e.authorize,
                              {{"client_id", account.clientId},
                               {"response_type", "code"},
                               {"scope", e.scopes},
                               {"state", state},
                               {"code_challenge", challenge(verifier)},
                               {"code_challenge_method", "S256"},
                               {"access_type", "offline"},
                               {"prompt", "consent"},
                               {"login_hint", account.address}},
                              redirect);
        std::map<std::string, std::string> fields{{"grant_type", "authorization_code"},
                                                  {"client_id", account.clientId},
                                                  {"code", code},
                                                  {"redirect_uri", redirect},
                                                  {"code_verifier", verifier}};
        if (!account.clientSecret.empty()) fields["client_secret"] = account.clientSecret;
        Parser parser;
        tokens(account, parser.parse(post(e.token, fields, cancel)));
    } else if (account.provider == "microsoft") {
        Parser parser;
        auto* result = parser.parse(post(e.device, {{"client_id", account.clientId}, {"scope", e.scopes}}, cancel));
        auto device = member(result, "device_code");
        if (device.empty()) throw std::runtime_error(errorText(member(result, "error")));
        auto url = member(result, "verification_uri");
        if (!url.starts_with("https://")) throw std::runtime_error("授权服务器返回了不安全的验证地址");
        show({url, member(result, "user_code"), "打开微软登录页面并输入设备代码。"});
        int interval = static_cast<int>(std::clamp(integer(result, "interval", 5), 1L, 30L));
        auto until = Clock::now() + std::chrono::seconds(std::clamp(integer(result, "expires_in", 900), 60L, 900L));
        while (Clock::now() < until) {
            sleep(cancel, interval);
            Parser poll;
            auto* o = poll.parse(post(e.token,
                                      {{"grant_type", "urn:ietf:params:oauth:grant-type:device_code"},
                                       {"client_id", account.clientId},
                                       {"device_code", device}},
                                      cancel));
            auto error = member(o, "error");
            if (error == "authorization_pending") continue;
            if (error == "slow_down") {
                interval = std::min(60, interval + 5);
                continue;
            }
            tokens(account, o);
            return;
        }
        throw std::runtime_error("设备代码已过期，请重新登录");
    } else
        throw std::runtime_error("未选择 OAuth 服务商");
}
void refresh(Account& account, std::atomic<bool>& cancel, const Endpoints* testing) {
    if (!account.accessToken.empty() && account.tokenExpires > std::time(nullptr) + 60) return;
    {
        std::lock_guard lock(tokenMutex);
        auto cached = sessionTokens.find(identity(account));
        if (cached != sessionTokens.end()) {
            account.accessToken = cached->second.accessToken;
            account.refreshToken = cached->second.refreshToken;
            account.tokenExpires = cached->second.tokenExpires;
        }
    }
    if (!account.accessToken.empty() && account.tokenExpires > std::time(nullptr) + 60) return;
    if (account.refreshToken.empty()) lookup(account);
    if (account.refreshToken.empty()) throw std::runtime_error("未找到有效登录，请使用浏览器重新授权");
    auto e = testing ? *testing : endpoints(account);
    std::map<std::string, std::string> fields{
        {"grant_type", "refresh_token"}, {"client_id", account.clientId}, {"refresh_token", account.refreshToken}};
    if (!account.clientSecret.empty()) fields["client_secret"] = account.clientSecret;
    if (account.provider == "microsoft") fields["scope"] = e.scopes;
    Parser parser;
    tokens(account, parser.parse(post(e.token, fields, cancel)));
    cacheTokens(account);
    (void)remember(account);
}
std::string remember(const Account& account) {
    if (!account.rememberPassword) return {};
    if (account.refreshToken.empty()) return "已登录，但服务商未返回刷新令牌；下次需要重新登录";
    auto* builder = json_builder_new();
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "refresh_token");
    json_builder_add_string_value(builder, account.refreshToken.c_str());
    json_builder_set_member_name(builder, "client_secret");
    json_builder_add_string_value(builder, account.clientSecret.c_str());
    json_builder_end_object(builder);
    auto* generator = json_generator_new();
    auto* root = json_builder_get_root(builder);
    json_generator_set_root(generator, root);
    auto* bytes = json_generator_to_data(generator, nullptr);
    GError* error = nullptr;
    bool ok = secret_password_store_sync(schema(), SECRET_COLLECTION_DEFAULT, "Forma 邮件 · OAuth 登录", bytes, nullptr,
                                         &error, "username", account.username.c_str(), "provider",
                                         account.provider.c_str(), "client", account.clientId.c_str(), nullptr);
    g_free(bytes);
    json_node_free(root);
    g_object_unref(generator);
    g_object_unref(builder);
    if (error) g_error_free(error);
    return ok ? std::string{} : "登录成功，但系统钥匙环不可用；本次会话可用，下次需重新授权";
}
void forget(const Account& a) {
    {
        std::lock_guard lock(tokenMutex);
        sessionTokens.erase(identity(a));
    }
    GError* error = nullptr;
    secret_password_clear_sync(schema(), nullptr, &error, "username", a.username.c_str(), "provider",
                               a.provider.c_str(), "client", a.clientId.c_str(), nullptr);
    if (error) g_error_free(error);
}
}  // namespace forma::mail::oauth
