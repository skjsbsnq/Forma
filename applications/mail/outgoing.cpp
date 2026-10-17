#include "outgoing.hpp"
#include <glib.h>
#include <algorithm>
#include <ctime>
#include <set>
#include <stdexcept>
namespace forma::mail {
namespace {
void require(bool b, const char* s) {
    if (!b) throw std::runtime_error(s);
}
void header(const std::string& s) {
    require(s.size() <= 8192 && s.find_first_of("\r\n\0", 0, 3) == std::string::npos &&
                g_utf8_validate(s.data(), s.size(), nullptr),
            "Invalid mail header");
}
std::string trim(std::string s) {
    auto p = s.find_first_not_of(" \t");
    if (p == std::string::npos) return {};
    return s.substr(p, s.find_last_not_of(" \t") - p + 1);
}
std::string mailbox(std::string s) {
    header(s);
    s = trim(s);
    auto p = s.find('<');
    if (p != std::string::npos) {
        require(s.back() == '>' && s.find('<', p + 1) == std::string::npos && s.find('>') == s.size() - 1,
                "Invalid mailbox");
        s = s.substr(p + 1, s.size() - p - 2);
    }
    require(s.size() <= 254 && !s.empty(), "Invalid email address");
    auto at = s.find('@');
    require(at > 0 && at != std::string::npos && at == s.rfind('@') && at + 1 < s.size() && at <= 64,
            "Invalid email address");
    auto local = s.substr(0, at), domain = s.substr(at + 1);
    require(local.front() != '.' && local.back() != '.' && local.find("..") == std::string::npos,
            "Invalid email address");
    for (unsigned char c : local)
        require(g_ascii_isalnum(c) || std::string(".!#$%&'*+-/=?^_`{|}~").find(c) != std::string::npos,
                "Invalid email address");
    require(domain.size() <= 253, "Invalid email domain");
    size_t start = 0;
    while (start < domain.size()) {
        auto end = domain.find('.', start);
        if (end == std::string::npos) end = domain.size();
        require(end > start && end - start <= 63 && domain[start] != '-' && domain[end - 1] != '-',
                "Invalid email domain");
        for (size_t i = start; i < end; i++)
            require(g_ascii_isalnum(domain[i]) || domain[i] == '-', "Invalid email domain");
        start = end + 1;
    }
    require(domain.back() != '.', "Invalid email domain");
    return s;
}
std::vector<std::string> parse(const std::string& s) {
    header(s);
    std::vector<std::string> r;
    size_t start = 0;
    bool quoted = false, angle = false;
    for (size_t i = 0; i <= s.size(); i++) {
        char c = i < s.size() ? s[i] : ',';
        if (c == '"') quoted = !quoted;
        if (!quoted && c == '<') angle = true;
        if (!quoted && c == '>') angle = false;
        if (!quoted && !angle && (c == ',' || c == ';')) {
            auto v = trim(s.substr(start, i - start));
            require(!v.empty() || trim(s).empty(), "Empty recipient");
            if (!v.empty()) r.push_back(mailbox(v));
            start = i + 1;
        }
    }
    require(!quoted && !angle, "Unclosed recipient");
    return r;
}
std::string b64(const std::string& s) {
    auto p = g_base64_encode(reinterpret_cast<const guchar*>(s.data()), s.size());
    std::string r = p;
    g_free(p);
    return r;
}
std::string encoded(const std::string& s) {
    header(s);
    std::string out;
    size_t off = 0;
    while (off < s.size()) {
        size_t end = std::min(off + 42, s.size());
        while (end < s.size() && (static_cast<unsigned char>(s[end]) & 0xc0) == 0x80) --end;
        if (!out.empty()) out += "\r\n ";
        out += "=?UTF-8?B?" + b64(s.substr(off, end - off)) + "?=";
        off = end;
    }
    return out;
}
std::string body64(const std::string& s) {
    auto b = b64(s);
    std::string r;
    for (size_t i = 0; i < b.size(); i += 76) r += b.substr(i, 76) + "\r\n";
    return r;
}
std::string addresses(const std::string& s) {
    std::string r;
    for (auto& v : parse(s)) {
        if (!r.empty()) r += ",\r\n ";
        r += v;
    }
    return r;
}
std::string fileName(std::string s) {
    header(s);
    auto p = s.find_last_of("/\\");
    if (p != std::string::npos) s = s.substr(p + 1);
    for (char& c : s)
        if (static_cast<unsigned char>(c) < 32 || c == 127) c = '_';
    require(s.size() <= 255, "Attachment filename too long");
    return s.empty() ? "attachment" : s;
}
std::string pct(const std::string& s) {
    const char* hex = "0123456789ABCDEF";
    std::string r;
    for (unsigned char c : s) {
        if (g_ascii_isalnum(c) || c == '-' || c == '.' || c == '_')
            r += c;
        else {
            r += '%';
            r += hex[c >> 4];
            r += hex[c & 15];
        }
    }
    return r;
}
}  // namespace
std::vector<std::string> recipientAddresses(const Outgoing& m) {
    std::vector<std::string> r;
    std::set<std::string> seen;
    for (auto* s : {&m.to, &m.cc, &m.bcc})
        for (auto& v : parse(*s)) {
            auto at = v.find('@');
            auto key = v.substr(0, at);
            for (size_t i = at; i < v.size(); i++) key += g_ascii_tolower(v[i]);
            if (seen.insert(key).second) r.push_back(v);
        }
    require(!r.empty() && r.size() <= 1000, "No recipients or too many recipients");
    return r;
}
std::string buildOutgoing(const Account& a, const Outgoing& m) {
    recipientAddresses(m);
    auto from = mailbox(a.address);
    header(a.name);
    header(m.subject);
    require(m.body.size() <= 25 * 1024 * 1024 && g_utf8_validate(m.body.data(), m.body.size(), nullptr),
            "Invalid or oversized message body");
    require(m.attachments.size() <= 100, "Too many attachments");
    size_t total = 0;
    for (auto& at : m.attachments) {
        total += at.data.size();
        require(total <= 25 * 1024 * 1024, "Attachments exceed 25 MiB");
    }
    auto uuid = g_uuid_string_random();
    std::string boundary = "forma-" + std::string(uuid);
    g_free(uuid);
    std::time_t now = std::time(nullptr);
    std::tm utc{};
    gmtime_r(&now, &utc);
    const char* days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    const char* months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    char date[100];
    snprintf(date, sizeof(date), "%s, %02d %s %04d %02d:%02d:%02d +0000", days[utc.tm_wday], utc.tm_mday,
             months[utc.tm_mon], utc.tm_year + 1900, utc.tm_hour, utc.tm_min, utc.tm_sec);
    std::string r = "From: " + (a.name.empty() ? from : encoded(a.name) + " <" + from + ">") + "\r\n";
    auto to = addresses(m.to), cc = addresses(m.cc);
    if (!to.empty()) r += "To: " + to + "\r\n";
    if (!cc.empty()) r += "Cc: " + cc + "\r\n";
    r += "Subject: " + encoded(m.subject) + "\r\nDate: " + date + "\r\nMessage-ID: <" + boundary + "@" +
         from.substr(from.find('@') + 1) + ">\r\nMIME-Version: 1.0\r\n";
    std::string part =
        "Content-Type: text/plain; charset=UTF-8\r\nContent-Transfer-Encoding: base64\r\n\r\n" + body64(m.body);
    if (m.attachments.empty()) return r + part;
    r += "Content-Type: multipart/mixed; boundary=\"" + boundary + "\"\r\n\r\n--" + boundary + "\r\n" + part;
    for (auto& at : m.attachments) {
        auto mime = at.mime.empty() ? "application/octet-stream" : at.mime;
        header(mime);
        require(mime.size() <= 127 && mime.find('/') != std::string::npos, "Invalid attachment MIME type");
        for (unsigned char c : mime)
            require(g_ascii_isalnum(c) || c == '/' || c == '-' || c == '+' || c == '.', "Invalid attachment MIME type");
        auto name = pct(fileName(at.name));
        r += "--" + boundary + "\r\nContent-Type: " + mime +
             "\r\nContent-Transfer-Encoding: base64\r\nContent-Disposition: attachment;\r\n";
        size_t n = 0;
        for (size_t off = 0; off < name.size();) {
            size_t end = std::min(off + 48, name.size());
            while (end < name.size() && (name[end - 1] == '%' || (end >= 2 && name[end - 2] == '%'))) --end;
            r += " filename*" + std::to_string(n) + "*=" + (n == 0 ? "UTF-8''" : "") + name.substr(off, end - off) +
                 (end < name.size() ? ";" : "") + "\r\n";
            off = end;
            n++;
        }
        r += "\r\n" + body64(at.data);
    }
    return r + "--" + boundary + "--\r\n";
}
}  // namespace forma::mail
