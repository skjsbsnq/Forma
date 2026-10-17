#include "storage.hpp"
#include <fcntl.h>
#include <json-glib/json-glib.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
namespace forma::mail::storage {
namespace {
constexpr size_t limit = 128 * 1024 * 1024, attachmentLimit = 16 * 1024 * 1024;
using Node = std::unique_ptr<JsonNode, decltype(&json_node_free)>;
using Object = std::unique_ptr<JsonObject, decltype(&json_object_unref)>;
void check(bool ok, const char* reason) {
    if (!ok) throw std::runtime_error(reason);
}
std::string hash(const std::string& s) {
    gchar* p = g_compute_checksum_for_data(G_CHECKSUM_SHA256, reinterpret_cast<const guchar*>(s.data()), s.size());
    std::string r = p;
    g_free(p);
    return r;
}
std::string profile(const Account& a) { return hash(hash(a.address) + hash(a.imapHost) + hash(a.username)); }
std::filesystem::path dir(const std::string& d) {
    check(!d.empty(), "Empty storage directory");
    std::filesystem::create_directories(d);
    struct stat st{};
    check(lstat(d.c_str(), &st) == 0 && S_ISDIR(st.st_mode) && st.st_uid == getuid(), "Unsafe storage directory");
    check(chmod(d.c_str(), 0700) == 0, "Cannot protect storage directory");
    return d;
}
void put(JsonObject* o, const char* k, const std::string& s) {
    check(s.size() <= limit && s.find('\0') == std::string::npos && g_utf8_validate(s.c_str(), s.size(), nullptr),
          "Invalid stored text");
    json_object_set_string_member(o, k, s.c_str());
}
std::string get(JsonObject* o, const char* k) {
    check(json_object_has_member(o, k), "Missing stored field");
    auto n = json_object_get_member(o, k);
    check(JSON_NODE_HOLDS_VALUE(n) && json_node_get_value_type(n) == G_TYPE_STRING, "Invalid stored field");
    std::string s = json_node_get_string(n);
    check(s.size() <= limit && g_utf8_validate(s.c_str(), s.size(), nullptr), "Invalid stored text");
    return s;
}
bool boolean(JsonObject* o, const char* k) {
    check(json_object_has_member(o, k), "Missing flag");
    auto n = json_object_get_member(o, k);
    check(JSON_NODE_HOLDS_VALUE(n) && json_node_get_value_type(n) == G_TYPE_BOOLEAN, "Invalid flag");
    return json_node_get_boolean(n);
}
void attachments(JsonObject* o, const std::vector<Attachment>& as) {
    check(as.size() <= 100, "Too many attachments");
    auto arr = json_array_new();
    size_t total = 0;
    for (auto& a : as) {
        check(a.data.size() <= attachmentLimit && (total += a.data.size()) <= limit, "Attachment limit exceeded");
        auto x = json_object_new();
        put(x, "name", a.name);
        put(x, "mime", a.mime);
        gchar* b = g_base64_encode(reinterpret_cast<const guchar*>(a.data.data()), a.data.size());
        put(x, "data", b);
        g_free(b);
        json_array_add_object_element(arr, x);
    }
    json_object_set_array_member(o, "attachments", arr);
}
std::vector<Attachment> attachments(JsonObject* o) {
    check(json_object_has_member(o, "attachments") && JSON_NODE_HOLDS_ARRAY(json_object_get_member(o, "attachments")),
          "Invalid attachments");
    auto arr = json_object_get_array_member(o, "attachments");
    check(json_array_get_length(arr) <= 100, "Too many attachments");
    std::vector<Attachment> result;
    size_t total = 0;
    for (guint i = 0; i < json_array_get_length(arr); i++) {
        auto n = json_array_get_element(arr, i);
        check(JSON_NODE_HOLDS_OBJECT(n), "Invalid attachment");
        auto x = json_node_get_object(n);
        auto b = get(x, "data");
        check(b.size() <= ((attachmentLimit + 2) / 3) * 4 && b.size() % 4 == 0, "Attachment limit exceeded");
        size_t padding = 0;
        for (size_t j = 0; j < b.size(); j++) {
            char c = b[j];
            if (c == '=') {
                padding++;
                check(j >= b.size() - 2 && padding <= 2, "Invalid base64");
            } else
                check(padding == 0 && (g_ascii_isalnum(c) || c == '+' || c == '/'), "Invalid base64");
        }
        gsize len = 0;
        auto bytes = g_base64_decode(b.c_str(), &len);
        std::string data(reinterpret_cast<char*>(bytes), len);
        g_free(bytes);
        check(len <= attachmentLimit && (total += len) <= limit, "Attachment limit exceeded");
        result.push_back({get(x, "name"), get(x, "mime"), std::move(data)});
    }
    return result;
}
void write(const std::filesystem::path& path, JsonObject* obj) {
    Node n(json_node_new(JSON_NODE_OBJECT), json_node_free);
    json_node_take_object(n.get(), json_object_ref(obj));
    gchar* raw = json_to_string(n.get(), false);
    std::string bytes = raw;
    g_free(raw);
    check(bytes.size() <= limit, "Storage size exceeded");
    std::string tmp = path.string() + ".tmp-XXXXXX";
    int fd = mkstemp(tmp.data());
    check(fd >= 0, "Cannot create storage file");
    try {
        check(fchmod(fd, 0600) == 0, "Cannot protect storage file");
        size_t p = 0;
        while (p < bytes.size()) {
            auto k = ::write(fd, bytes.data() + p, bytes.size() - p);
            if (k < 0 && errno == EINTR) continue;
            check(k > 0, "Storage write failed");
            p += k;
        }
        check(fsync(fd) == 0, "Storage sync failed");
        close(fd);
        fd = -1;
        check(rename(tmp.c_str(), path.c_str()) == 0, "Storage rename failed");
        int d = open(path.parent_path().c_str(), O_RDONLY | O_DIRECTORY);
        check(d >= 0, "Storage directory open failed");
        int r = fsync(d);
        close(d);
        check(r == 0, "Storage directory sync failed");
    } catch (...) {
        if (fd >= 0) close(fd);
        unlink(tmp.c_str());
        throw;
    }
}
Object read(const std::filesystem::path& p) {
    int fd = open(p.c_str(), O_RDONLY | O_NOFOLLOW);
    if (fd < 0 && errno == ENOENT) return Object(nullptr, json_object_unref);
    check(fd >= 0, "Cannot read storage");
    struct stat s{};
    if (fstat(fd, &s) || !S_ISREG(s.st_mode) || s.st_size < 0 || static_cast<size_t>(s.st_size) > limit) {
        close(fd);
        throw std::runtime_error("Invalid storage file");
    }
    std::string data(static_cast<size_t>(s.st_size), '\0');
    size_t off = 0;
    while (off < data.size()) {
        auto n = ::read(fd, data.data() + off, data.size() - off);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            close(fd);
            throw std::runtime_error("Storage read failed");
        }
        off += n;
    }
    close(fd);
    auto parser = json_parser_new();
    GError* error = nullptr;
    bool ok = json_parser_load_from_data(parser, data.data(), data.size(), &error);
    if (!ok) {
        std::string msg = error ? error->message : "Invalid JSON";
        g_clear_error(&error);
        g_object_unref(parser);
        throw std::runtime_error(msg);
    }
    auto root = json_parser_get_root(parser);
    if (!JSON_NODE_HOLDS_OBJECT(root)) {
        g_object_unref(parser);
        throw std::runtime_error("Invalid storage root");
    }
    Object o(json_object_ref(json_node_get_object(root)), json_object_unref);
    g_object_unref(parser);
    check(get(o.get(), "version") == "1", "Unsupported storage version");
    return o;
}
JsonArray* items(JsonObject* o) {
    check(json_object_has_member(o, "items") && JSON_NODE_HOLDS_ARRAY(json_object_get_member(o, "items")),
          "Invalid stored items");
    auto a = json_object_get_array_member(o, "items");
    check(json_array_get_length(a) <= 10000, "Too many stored items");
    return a;
}
Object root() {
    Object o(json_object_new(), json_object_unref);
    put(o.get(), "version", "1");
    return o;
}
}  // namespace
namespace {
void budget(size_t& total, const std::string& text) {
    check(text.size() <= limit && total <= limit - text.size(), "Storage size exceeded");
    total += text.size();
}
void budgetAttachments(size_t& total, const std::vector<Attachment>& as) {
    check(as.size() <= 100, "Too many attachments");
    for (auto& a : as) {
        check(a.data.size() <= attachmentLimit, "Attachment limit exceeded");
        budget(total, a.name);
        budget(total, a.mime);
        size_t expanded = ((a.data.size() + 2) / 3) * 4 + 256;
        check(total <= limit - expanded, "Storage size exceeded");
        total += expanded;
    }
}
}  // namespace
void saveDrafts(const std::string& d, const std::vector<Draft>& ds) {
    check(ds.size() <= 10000, "Too many drafts");
    size_t total = 1024;
    for (auto& v : ds) {
        for (auto* s : {&v.id, &v.to, &v.cc, &v.bcc, &v.subject, &v.body}) budget(total, *s);
        budgetAttachments(total, v.attachments);
        check(total <= limit - 512, "Storage size exceeded");
        total += 512;
    }
    auto o = root();
    auto a = json_array_new();
    json_object_set_array_member(o.get(), "items", a);
    for (auto& v : ds) {
        auto x = json_object_new();
        json_array_add_object_element(a, x);
        put(x, "id", v.id);
        put(x, "to", v.to);
        put(x, "cc", v.cc);
        put(x, "bcc", v.bcc);
        put(x, "subject", v.subject);
        put(x, "body", v.body);
        attachments(x, v.attachments);
    }
    write(dir(d) / "drafts.json", o.get());
}
std::vector<Draft> loadDrafts(const std::string& d) {
    auto o = read(dir(d) / "drafts.json");
    if (!o) return {};
    std::vector<Draft> r;
    auto a = items(o.get());
    for (guint i = 0; i < json_array_get_length(a); i++) {
        auto n = json_array_get_element(a, i);
        check(JSON_NODE_HOLDS_OBJECT(n), "Invalid draft");
        auto x = json_node_get_object(n);
        Draft v;
        v.id = get(x, "id");
        v.to = get(x, "to");
        v.cc = get(x, "cc");
        v.bcc = get(x, "bcc");
        v.subject = get(x, "subject");
        v.body = get(x, "body");
        v.attachments = attachments(x);
        r.push_back(std::move(v));
    }
    return r;
}
void saveCache(const std::string& d, const Account& account, const std::string& folder,
               const std::vector<Message>& ms) {
    check(ms.size() <= 10000, "Too many messages");
    size_t total = 1024;
    for (auto& v : ms) {
        for (auto* s : {&v.id, &v.folder, &v.sender, &v.address, &v.subject, &v.preview, &v.body, &v.date})
            budget(total, *s);
        budgetAttachments(total, v.attachments);
        check(total <= limit - 512, "Storage size exceeded");
        total += 512;
    }
    auto o = root();
    put(o.get(), "profile", profile(account));
    put(o.get(), "folder", folder);
    auto a = json_array_new();
    json_object_set_array_member(o.get(), "items", a);
    for (auto& v : ms) {
        auto x = json_object_new();
        json_array_add_object_element(a, x);
        put(x, "id", v.id);
        put(x, "folder", v.folder);
        put(x, "sender", v.sender);
        put(x, "address", v.address);
        put(x, "subject", v.subject);
        put(x, "preview", v.preview);
        put(x, "body", v.body);
        put(x, "date", v.date);
        json_object_set_boolean_member(x, "unread", v.unread);
        json_object_set_boolean_member(x, "star", v.star);
        attachments(x, v.attachments);
    }
    write(dir(d) / ("cache-" + profile(account) + "-" + hash(folder) + ".json"), o.get());
}
std::vector<Message> loadCache(const std::string& d, const Account& account, const std::string& folder) {
    auto o = read(dir(d) / ("cache-" + profile(account) + "-" + hash(folder) + ".json"));
    if (!o) return {};
    check(get(o.get(), "profile") == profile(account) && get(o.get(), "folder") == folder, "Cache identity mismatch");
    std::vector<Message> r;
    auto a = items(o.get());
    for (guint i = 0; i < json_array_get_length(a); i++) {
        auto n = json_array_get_element(a, i);
        check(JSON_NODE_HOLDS_OBJECT(n), "Invalid message");
        auto x = json_node_get_object(n);
        Message v;
        v.id = get(x, "id");
        v.folder = get(x, "folder");
        v.sender = get(x, "sender");
        v.address = get(x, "address");
        v.subject = get(x, "subject");
        v.preview = get(x, "preview");
        v.body = get(x, "body");
        v.date = get(x, "date");
        v.unread = boolean(x, "unread");
        v.star = boolean(x, "star");
        v.attachments = attachments(x);
        r.push_back(std::move(v));
    }
    return r;
}
void clearAccountCache(const std::string& d, const Account& a) {
    auto prefix = "cache-" + profile(a) + "-";
    for (auto& p : std::filesystem::directory_iterator(dir(d)))
        if (p.path().filename().string().starts_with(prefix) && p.path().extension() == ".json")
            std::filesystem::remove(p.path());
}
}  // namespace forma::mail::storage
