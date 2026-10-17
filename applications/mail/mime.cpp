#include "mime.hpp"
#include <glib.h>
#include <algorithm>
#include <map>
#include <optional>
#include <stdexcept>
#include <string_view>
namespace forma::mail {
namespace {
constexpr size_t maxRaw = 64 * 1024 * 1024, maxPart = 16 * 1024 * 1024;
void ensure(bool b, const char* why) {
    if (!b) throw std::runtime_error(why);
}
std::string lower(std::string s) {
    for (auto& c : s) c = g_ascii_tolower(c);
    return s;
}
std::string trim(std::string s) {
    auto a = s.find_first_not_of(" \t\r\n");
    return a == s.npos ? "" : s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}
std::string utf8(const std::string& s, const std::string& charset) {
    ensure(s.size() <= maxPart, "MIME text exceeds limit");
    auto cs = trim(charset);
    if (cs.empty()) cs = "UTF-8";
    ensure(cs.size() <= 80 && cs.find_first_of("\r\n\0", 0, 3) == cs.npos, "Invalid charset");
    GError* error = nullptr;
    gsize n = 0;
    auto p = g_convert(s.data(), s.size(), "UTF-8", cs.c_str(), nullptr, &n, &error);
    if (!p) {
        std::string why = error ? error->message : "Invalid text encoding";
        g_clear_error(&error);
        throw std::runtime_error(why);
    }
    std::string out(p, n);
    g_free(p);
    ensure(out.find('\0') == out.npos && g_utf8_validate(out.data(), out.size(), nullptr), "Invalid MIME text");
    return out;
}
std::string base64(std::string s) {
    s.erase(std::remove_if(s.begin(), s.end(),
                           [](unsigned char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }),
            s.end());
    ensure(s.size() % 4 == 0, "Invalid base64 length");
    size_t pad = 0;
    for (size_t i = 0; i < s.size(); i++) {
        auto c = s[i];
        if (c == '=') {
            pad++;
            ensure(pad <= 2 && i >= s.size() - 2, "Invalid base64 padding");
        } else
            ensure(!pad && (g_ascii_isalnum(c) || c == '+' || c == '/'), "Invalid base64 data");
    }
    gsize n;
    auto p = g_base64_decode(s.c_str(), &n);
    std::string out(reinterpret_cast<char*>(p), n);
    g_free(p);
    ensure(out.size() <= maxPart, "MIME part exceeds limit");
    return out;
}
std::string qp(const std::string& s, bool word = false) {
    std::string r;
    for (size_t i = 0; i < s.size(); i++) {
        char c = s[i];
        if (word && c == '_')
            r += ' ';
        else if (c == '=') {
            if (!word && i + 1 < s.size() && s[i + 1] == '\n') {
                i++;
                continue;
            }
            if (!word && i + 2 < s.size() && s[i + 1] == '\r' && s[i + 2] == '\n') {
                i += 2;
                continue;
            }
            ensure(i + 2 < s.size() && g_ascii_isxdigit(s[i + 1]) && g_ascii_isxdigit(s[i + 2]),
                   "Invalid quoted-printable data");
            r += char(g_ascii_xdigit_value(s[i + 1]) * 16 + g_ascii_xdigit_value(s[i + 2]));
            i += 2;
        } else
            r += c;
    }
    ensure(r.size() <= maxPart, "MIME part exceeds limit");
    return r;
}
std::string words(const std::string& s) {
    std::string r;
    size_t at = 0;
    bool previous = false;
    while (at < s.size()) {
        auto a = s.find("=?", at);
        if (a == s.npos) {
            r += s.substr(at);
            break;
        }
        auto b = s.find('?', a + 2), c = b == s.npos ? s.npos : s.find('?', b + 1),
             e = c == s.npos ? s.npos : s.find("?=", c + 1);
        if (b == s.npos || c == s.npos || e == s.npos) {
            r += s.substr(at);
            break;
        }
        auto encoding = lower(s.substr(b + 1, c - b - 1));
        if (encoding != "b" && encoding != "q") {
            r += s.substr(at, a + 2 - at);
            at = a + 2;
            previous = false;
            continue;
        }
        auto gap = s.substr(at, a - at);
        if (!previous || !trim(gap).empty()) r += gap;
        auto payload = s.substr(c + 1, e - c - 1);
        r += utf8(encoding == "b" ? base64(payload) : qp(payload, true), s.substr(a + 2, b - a - 2));
        at = e + 2;
        previous = true;
    }
    return utf8(r, "UTF-8");
}
using Headers = std::map<std::string, std::string>;
struct Part {
    Headers headers;
    std::string body;
};
Part split(const std::string& raw) {
    auto end = raw.find("\r\n\r\n");
    size_t skip = 4;
    if (end == raw.npos) {
        end = raw.find("\n\n");
        skip = 2;
    }
    ensure(end != raw.npos && end <= 256 * 1024, "Invalid MIME header block");
    Part p;
    std::string key, last;
    size_t at = 0, count = 0;
    while (at < end) {
        auto next = raw.find('\n', at);
        if (next == raw.npos || next > end) next = end;
        auto line = raw.substr(at, next - at);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        ensure(line.size() <= 16384 && line.find('\0') == line.npos, "Invalid MIME header");
        if (!line.empty() && (line[0] == ' ' || line[0] == '\t')) {
            // Folded continuation lines belong to the most recent header, even when
            // a repeated header was skipped (Gmail mail has several Received: lines).
            ensure(!last.empty(), "Invalid header continuation");
            if (!key.empty()) p.headers[key] += " " + trim(line);
            else p.headers[last] += " " + trim(line);
        } else {
            auto colon = line.find(':');
            ensure(colon != line.npos && colon > 0, "Invalid MIME header");
            key = lower(line.substr(0, colon));
            for (unsigned char c : key) ensure(g_ascii_isalnum(c) || c == '-', "Invalid header name");
            ensure(++count <= 1000, "Too many headers");
            last = key;
            if (!p.headers.contains(key))
                p.headers[key] = trim(line.substr(colon + 1));
            else
                key.clear();
        }
        at = next + 1;
    }
    p.body = raw.substr(end + skip);
    return p;
}
struct Content {
    std::string type;
    Headers params;
};
Content content(const std::string& s) {
    Content r;
    auto sem = s.find(';');
    r.type = lower(trim(s.substr(0, sem)));
    size_t i = sem;
    while (i != s.npos && i < s.size()) {
        i++;
        while (i < s.size() && g_ascii_isspace(s[i])) i++;
        size_t start = i;
        while (i < s.size() && s[i] != '=' && s[i] != ';') i++;
        if (i == s.size() || s[i] != '=') {
            i = s.find(';', i);
            continue;
        }
        auto key = lower(trim(s.substr(start, i - start)));
        i++;
        while (i < s.size() && g_ascii_isspace(s[i])) i++;
        std::string value;
        if (i < s.size() && s[i] == '"') {
            i++;
            bool closed = false;
            while (i < s.size()) {
                if (s[i] == '"') {
                    i++;
                    closed = true;
                    break;
                }
                if (s[i] == '\\' && i + 1 < s.size()) i++;
                value += s[i++];
            }
            ensure(closed, "Unclosed MIME parameter");
        } else {
            start = i;
            while (i < s.size() && s[i] != ';') i++;
            value = trim(s.substr(start, i - start));
        }
        if (!key.empty()) r.params[key] = value;
        i = s.find(';', i);
    }
    return r;
}
std::string percent(const std::string& s) {
    std::string r;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%') {
            ensure(i + 2 < s.size() && g_ascii_isxdigit(s[i + 1]) && g_ascii_isxdigit(s[i + 2]),
                   "Invalid RFC2231 escape");
            r += char(g_ascii_xdigit_value(s[i + 1]) * 16 + g_ascii_xdigit_value(s[i + 2]));
            i += 2;
        } else
            r += s[i];
    }
    return r;
}
std::string parameter(const Content& c, const std::string& name) {
    std::string data;
    bool encoded = false;
    auto direct = c.params.find(name + "*");
    if (direct != c.params.end()) {
        data = direct->second;
        encoded = true;
    } else if (c.params.contains(name + "*0") || c.params.contains(name + "*0*")) {
        for (int n = 0; n < 100; n++) {
            auto key = name + "*" + std::to_string(n);
            bool enc = c.params.contains(key + "*");
            auto it = c.params.find(enc ? key + "*" : key);
            if (it == c.params.end()) break;
            if (n == 0) encoded = enc;
            data += enc ? percent(it->second) : it->second;
            ensure(data.size() <= 4096, "MIME filename exceeds limit");
        }
        if (encoded) {
            auto a = data.find('\''), b = a == data.npos ? data.npos : data.find('\'', a + 1);
            ensure(b != data.npos, "Invalid RFC2231 parameter");
            return utf8(data.substr(b + 1), data.substr(0, a));
        }
        return words(data);
    } else {
        auto it = c.params.find(name);
        return it == c.params.end() ? "" : words(it->second);
    }
    auto a = data.find('\''), b = a == data.npos ? data.npos : data.find('\'', a + 1);
    ensure(b != data.npos, "Invalid RFC2231 parameter");
    return utf8(percent(data.substr(b + 1)), data.substr(0, a));
}
std::string entities(const std::string& s) {
    std::string r;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] != '&') {
            r += s[i];
            continue;
        }
        auto end = s.find(';', i + 1);
        if (end == s.npos || end - i > 32) {
            r += '&';
            continue;
        }
        auto token = s.substr(i + 1, end - i - 1);
        std::string out;
        static const Headers known = {{"amp", "&"},     {"lt", "<"},       {"gt", ">"},
                                      {"quot", "\""}, {"apos", "'"},     {"nbsp", " "},
                                      {"trade", "™"}, {"reg", "®"},      {"copy", "©"},
                                      {"bull", "•"},  {"middot", "·"},  {"hellip", "…"},
                                      {"ndash", "–"}, {"mdash", "—"},   {"ldquo", "“"},
                                      {"rdquo", "”"}, {"lsquo", "‘"},   {"rsquo", "’"},
                                      {"deg", "°"},   {"times", "×"},    {"divide", "÷"},
                                      {"euro", "€"},  {"pound", "£"},   {"yen", "¥"},
                                      {"cent", "¢"},  {"sect", "§"},    {"para", "¶"},
                                      {"laquo", "«"}, {"raquo", "»"},   {"shy", ""},
                                      {"zwnj", ""},   {"zwj", ""},      {"ensp", " "},
                                      {"emsp", " "},  {"thinsp", " "}};
        if (known.contains(token))
            out = known.at(token);
        else if (token.starts_with('#')) {
            bool hex = token.size() > 1 && (token[1] == 'x' || token[1] == 'X');
            auto digits = token.substr(hex ? 2 : 1);
            guint64 value = 0;
            bool valid = !digits.empty();
            for (char c : digits) {
                int d = hex ? g_ascii_xdigit_value(c) : (g_ascii_isdigit(c) ? c - '0' : -1);
                if (d < 0 || value > 0x10ffff / (hex ? 16 : 10)) {
                    valid = false;
                    break;
                }
                value = value * (hex ? 16 : 10) + d;
            }
            if (valid && value && g_unichar_validate(value)) {
                char buf[8];
                auto n = g_unichar_to_utf8(value, buf);
                out.assign(buf, n);
            }
        }
        if (out.empty())
            r += s.substr(i, end - i + 1);
        else
            r += out;
        i = end;
    }
    return r;
}
std::string htmlText(const std::string& s) {
    // Convert real-world marketing HTML to readable plain text. Rules follow the
    // common practice of mutt's handler, K-9 Mail's HtmlConverter and Thunderbird's
    // libmime: block tags become newlines, cells are separated by spaces, whitespace
    // runs collapse, blank lines are capped, and invisible content is skipped.
    // Pending-break output core (Thunderbird mFloatingLines / K-9 startNewLine):
    // a break is requested by block tags but emitted only before the next visible
    // text, so trailing blanks and doubled blank lines never survive.
    std::string out;
    out.reserve(s.size() / 2);
    size_t i = 0;
    int pending = 0;   // requested newlines: 1 = end line, 2 = one blank line
    bool started = false;
    bool pre = false;  // <pre> keeps its whitespace
    auto emit = [&](char c) {
        // Collapse whitespace runs from layout markup; a single space between words.
        // A pending block break is flushed first so paragraphs stay separated.
        bool space = c == ' ' || c == '\t' || c == '\n' || c == '\r';
        if (pre && c != '\r') {
            // <pre>: keep runs and line breaks verbatim (tabs become a space).
            static const char* preBreaks[] = {"", "\n", "\n\n"};
            if (pending && !out.empty()) {
                auto breaks = preBreaks[std::min(pending, 2)];
                while (!out.empty() && out.back() == ' ') out.pop_back();
                out += breaks;
            }
            pending = 0;
            out += c == '\t' ? ' ' : c;
            started = true;
            return;
        }
        if (space && (out.empty() || out.back() == ' ' || out.back() == '\n')) return;
        if (pending) {
            // Emit up to `pending` newlines. A trailing newline already in the buffer
            // counts as the first one, so two pending yields exactly one blank line.
            int want = pending;
            pending = 0;
            while (!out.empty() && out.back() == ' ') out.pop_back();
            int have = 0;
            for (size_t at = out.size(); at > 0 && out[at - 1] == '\n'; --at) have++;
            if (!out.empty())
                for (; have < want; ++have) out += '\n';
        }
        if (out.empty() && space) return;
        out += space ? ' ' : c;
        started = true;
    };
    auto newline = [&](int count) {
        if (!started) return;
        pending = std::max(pending, count);
    };
    bool link = false;
    std::string href;
    size_t linkTextStart = 0;
    // Marketing mail routes every link through a redirector with opaque parameters
    // hundreds of characters long ("ls/click?upn=", "/CL0/https:..."). Printing them
    // drowns the message, so they are treated like K-9's "url same as text" case: the
    // link text alone is shown. Genuine links keep their address so it can be copied.
    auto trackingURL = [](const std::string& url) -> bool {
        if (url.size() > 160) return true;
        auto lower = [](std::string v) {
            for (auto& c : v) c = static_cast<char>(g_ascii_tolower(c));
            return v;
        };
        auto u = lower(url);
        if (u.starts_with("mailto:")) return false;  // an address is always useful
        // Redirector style paths and parameters.
        for (const char* marker :
             {"ls/click", "click?", "upn=", "/cl0/", "utm_", "?url=", "&url=", "redirect=", "tracking", "/t/c/",
              "/e/c/", "mkto", "safelinks", "click.", "trk.", "links.", "email.", "campaign", "subscri"}) {
            if (u.find(marker) != std::string::npos) return true;
        }
        auto query = url.find('?');
        if (query != std::string::npos && url.size() - query > 100) return true;
        return false;
    };
    auto flushLink = [&](bool atEnd) {
        // K-9 / Thunderbird: append the URL after the link text unless the text already
        // shows it. Suppressed for tracking redirects and overlong URLs.
        if (!link || href.empty()) {
            link = false;
            href.clear();
            return;
        }
        if (atEnd) {
            auto text = out.substr(linkTextStart);
            while (!text.empty() && (text.back() == ' ' || text.back() == '\n')) text.pop_back();
            while (!text.empty() && (text.front() == '\n')) text.erase(text.begin());
            bool shown = text.find(href) != std::string::npos || href.find(text) != std::string::npos;
            if (!shown && !trackingURL(href) && !text.empty()) {
                {
                    auto suffix = " (" + href + ")";
                    for (size_t k = 0; k < suffix.size(); ++k) emit(suffix[k]);
                }
            }
        }
        link = false;
        href.clear();
    };
    auto skipBlock = [&](std::string_view tag) {
        // Skip a balanced element (used for script/style/head and hidden blocks).
        std::string close = "</" + std::string(tag);
        size_t depth = 1;
        while (i < s.size() && depth) {
            if (s.compare(i, 2, "<!--") == 0) {
                auto e = s.find("-->", i + 4);
                i = e == s.npos ? s.size() : e + 3;
                continue;
            }
            if (s[i] == '<') {
                auto name = s.substr(i + 1, tag.size());
                for (auto& c : name) c = g_ascii_tolower(c);
                if (name == tag && (i + 1 + tag.size() >= s.size() || !g_ascii_isalpha(s[i + 1 + tag.size()])))
                    depth++;
                else if (s.compare(i, close.size(), close) == 0) {
                    // Consume the whole closing tag including its '>'.
                    depth--;
                    auto gt = s.find('>', i + close.size());
                    i = gt == std::string::npos ? s.size() : gt + 1;
                    break;
                }
            }
            i++;
        }
    };
    while (i < s.size()) {
        if (s.compare(i, 4, "<!--") == 0) {
            // MS Outlook conditional comments. "Downlevel-hidden" wraps content that
            // only Outlook shows:
            //   <!--[if gte mso 9]> ... <![endif]-->      -> skip the content too
            // "Downlevel-revealed" is two adjacent comments around visible content:
            //   <!--[if !mso]><!--> content <!--<![endif]-->
            // The first comment ends at the "<!-->" one-liner, so the content that
            // follows must be kept; the trailing <!--<![endif]--> is a no-op.
            auto end = s.find("-->", i + 4);
            if (end == s.npos) {
                i = s.size();
                continue;
            }
            auto body = s.substr(i + 4, end - i - 4);
            if (body.find("[if") != std::string::npos) {
                // Downlevel-revealed: the comment body ends with a bare "<!--" that opens
                // the next (empty) comment; the revealed content is outside this comment.
                bool revealed = body.rfind("<!", body.size() - 2) != std::string::npos;
                if (revealed) {
                    i = end + 3;
                    continue;
                }
                // Downlevel-hidden: drop everything through the matching [endif].
                auto stop = s.find("[endif]", i);
                auto tail = stop == s.npos ? s.npos : s.find("-->", stop);
                i = tail == s.npos ? s.size() : tail + 3;
                // Stray termination such as "<![endif]-->" right after the marker.
                if (i < s.size() && s.compare(i, 4, "<!--") == 0) {
                    auto e2 = s.find("-->", i + 4);
                    auto b2 = s.substr(i + 4, (e2 == s.npos ? s.size() : e2) - i - 4);
                    if (b2.find("[endif]") != std::string::npos) {
                        i = e2 == s.npos ? s.size() : e2 + 3;
                    }
                }
            } else
                i = end + 3;
            continue;
        }
        if (s[i] != '<') {
            emit(s[i++]);
            continue;
        }
        size_t j = i + 1;
        bool quote = false;
        char quoteChar = 0;
        for (; j < s.size(); j++) {
            char c = s[j];
            if (quote) {
                if (c == quoteChar) quote = false;
            } else if (c == '\'' || c == '"') {
                quote = true;
                quoteChar = c;
            } else if (c == '>')
                break;
        }
        if (j >= s.size()) {
            emit(s[i++]);
            continue;
        }
        auto raw = s.substr(i + 1, j - i - 1);
        i = j + 1;
        auto tag = lower(trim(raw));
        bool close = tag.starts_with('/');
        if (close) tag = trim(tag.substr(1));
        tag = tag.substr(0, tag.find_first_of(" \t\r\n/"));
        if (!close) {
            // Outlook preheaders and hidden preview text use display:none / mso-hide.
            auto style = lower(raw);
            auto hid = style.find("style=");
            if (hid != std::string::npos) {
                auto tail = style.substr(hid, style.find('>', hid) == std::string::npos ? std::string::npos
                                                                                       : style.find('>', hid) - hid);
                if (tail.find("display:none") != std::string::npos || tail.find("display: none") != std::string::npos ||
                    tail.find("mso-hide") != std::string::npos || tail.find("visibility:hidden") != std::string::npos ||
                    tail.find("visibility: hidden") != std::string::npos ||
                    tail.find("max-height:0") != std::string::npos || tail.find("max-height: 0") != std::string::npos ||
                    tail.find("font-size:0") != std::string::npos || tail.find("font-size: 0") != std::string::npos) {
                    if (!(tag == "img" || tag == "br" || tag == "td" || tag == "input" || tag == "meta"))
                        skipBlock(tag);
                    continue;
                }
            }
        }
        if (tag == "script" || tag == "style" || tag == "head" || tag == "title" || tag == "svg") {
            // Balance the already-consumed opening tag inside skipBlock by treating
            // it as seen: start depth at 1, and after the close continue after '>'.
            skipBlock(tag);
            continue;
        }
        if (tag == "br") {
            newline(1);
            continue;
        }
        if (tag == "pre") {
            if (close) {
                pre = false;
                newline(2);
            } else {
                newline(1);
                pre = true;
            }
            continue;
        }
        if (close) {
            if (tag == "p" || tag == "h1" || tag == "h2" || tag == "h3" || tag == "h4" || tag == "h5" ||
                tag == "h6" || tag == "table" || tag == "section" || tag == "blockquote" || tag == "ul" ||
                tag == "ol" || tag == "tr")
                newline(2);
            else if (tag == "div")
                newline(1);
            else if (tag == "li" || tag == "ul" || tag == "ol") {
                newline(tag == "li" ? 1 : 2);
            }
            else if (tag == "dt" || tag == "dd" || tag == "tr") {
                newline(1);
            } else if (tag == "blockquote") {
                newline(2);
            } else if (tag == "a")
                flushLink(true);
            continue;
        }
        if (tag == "a") {
            link = true;
            linkTextStart = out.size();
            pending = 0;
            href.clear();
            auto at = raw.find("href");
            if (at != raw.npos) {
                auto eq = raw.find('=', at);
                if (eq != raw.npos) {
                    size_t k = eq + 1;
                    while (k < raw.size() && g_ascii_isspace(raw[k])) k++;
                    if (k < raw.size() && raw[k] == '"') {
                        auto e = raw.find('"', k + 1);
                        if (e != raw.npos) href = raw.substr(k + 1, e - k - 1);
                    } else {
                        auto e = raw.find_first_of(" \t\r\n>", k);
                        href = raw.substr(k, e == raw.npos ? raw.npos : e - k);
                    }
                }
            }
            continue;
        }
        if (tag == "img") {
            auto attr = [&](std::string_view name) -> std::string {
                auto hay = lower(raw);
                auto at = hay.find(std::string(name) + "=");
                if (at == std::string::npos) return {};
                size_t k = at + name.size() + 1;
                while (k < raw.size() && g_ascii_isspace(raw[k])) k++;
                if (k < raw.size() && (raw[k] == '"' || raw[k] == '\'')) {
                    auto e = raw.find(raw[k], k + 1);
                    return raw.substr(k + 1, e == std::string::npos ? std::string::npos : e - k - 1);
                }
                auto e = raw.find_first_of(" \t\r\n>", k);
                return raw.substr(k, e == std::string::npos ? std::string::npos : e - k);
            };
            auto rawSrc = trim(attr("src"));
            auto src = lower(rawSrc);
            auto alt = trim(attr("alt"));
            // Open-tracking pixels stay invisible: 1x1 beacons and spacer images.
            bool beacon = [&] {
                auto w = lower(attr("width")), h = lower(attr("height"));
                return w == "1" || h == "1" || src.find("spacer") != std::string::npos ||
                       (src.find("open") != std::string::npos && src.find(".gif") != std::string::npos);
            }();
            if (beacon) continue;
            // Inline images become replaceable placeholders the reader view turns
            // into real pictures; remote and alt-less images show a caption instead.
            if (src.starts_with("cid:")) {
                {
                    auto record = std::string("\x1eimg:") + rawSrc.substr(4) + "\x1e";
                    for (size_t k = 0; k < record.size(); ++k) emit(record[k]);
                }
                continue;
            }
            auto label = alt.empty() ? std::string("图片") : alt;
            if (label.size() > 40) {
                label.resize(40);
                while (!label.empty() && !g_utf8_validate(label.data(), label.size(), nullptr)) label.pop_back();
                label += "…";
            }
            // Only absolute http(s) images are fetched; anything else (relative
            // paths, file:, data:) would be a broken or unsafe request.
            if (src.starts_with("http://") || src.starts_with("https://")) {
                {
                    auto record = std::string("\x1eimgurl:") + rawSrc + "|alt:" + label + "\x1e";
                    for (size_t k = 0; k < record.size(); ++k) emit(record[k]);
                }
            } else {
                {
                    auto record = "[" + label + "] ";
                    for (size_t k = 0; k < record.size(); ++k) emit(record[k]);
                }
            }
            continue;
        }
        if (tag == "li") {
            newline(1);
            for (char ch : std::string("\342\200\242 ")) emit(ch);
            continue;
        }
        if (tag == "td" || tag == "th") {
            if (started) emit(' ');
            continue;
        }
        if (tag == "p" || tag == "h1" || tag == "h2" || tag == "h3" || tag == "h4" || tag == "h5" ||
            tag == "h6" || tag == "table" || tag == "ul" || tag == "ol" || tag == "blockquote")
            newline(2);
        else if (tag == "div" || tag == "tr")
            newline(1);
    }
    flushLink(false);
    // Decode entities, then sanitize: drop characters mail HTML uses for layout
    // (soft hyphen, zero-width joiners, bidi marks, private-use icon glyphs) since a
    // plain text view would draw them as tofu boxes.
    auto text = trim(entities(out));
    std::string clean;
    clean.reserve(text.size());
    int nl = 0;
    for (size_t at = 0; at < text.size();) {
        auto c = static_cast<unsigned char>(text[at]);
        if (c < 0x80) {
            if (c == '\r') {
                at++;
                continue;
            }
            if (c == '\t') {
                clean += ' ';
                at++;
                continue;
            }
            if (c == '\n') {
                if (++nl > 2) {  // at most one blank line
                    at++;
                    continue;
                }
            } else {
                nl = 0;
                if (c < 0x20 && c != 0x1e) {  // C0 controls, except our image marker
                    at++;
                    continue;
                }
                if (c == ' ' && !clean.empty() && clean.back() == ' ') {
                    at++;
                    continue;
                }
            }
            clean += static_cast<char>(c);
            at++;
            continue;
        }
        gunichar u = g_utf8_get_char_validated(text.data() + at, text.size() - at);
        if (u == static_cast<gunichar>(-1) || u == static_cast<gunichar>(-2)) {
            at++;
            continue;
        }
        int len = u < 0x80 ? 1 : u < 0x800 ? 2 : u < 0x10000 ? 3 : 4;
        bool drop = u == 0x00AD || u == 0x200B || u == 0x200C || u == 0x200D || u == 0x200E || u == 0x200F ||
                    u == 0x2060 || u == 0xFEFF || (u >= 0x202A && u <= 0x202E) || (u >= 0xE000 && u <= 0xF8FF) ||
                    u == 0xFFFC || u == 0xFFFD || (u >= 0x7F && u <= 0x9F);
        if (u == 0x00A0) {
            clean += ' ';
        } else if (!drop) {
            clean.append(text, at, static_cast<size_t>(len));
            nl = 0;
        }
        at += static_cast<size_t>(len);
    }
    // No trailing spaces before a newline anywhere.
    std::string result;
    result.reserve(clean.size());
    for (char c : clean) {
        if (c == '\n')
            while (!result.empty() && result.back() == ' ') result.pop_back();
        if (c == ' ' && (result.empty() || result.back() == '\n')) continue;
        result += c;
    }
    return trim(result);
}
struct Result {
    std::string text;
    int quality = 0;
    std::vector<Attachment> attachments;
    // Related-part images keyed by their Content-ID (without angle brackets).
    std::vector<Attachment> inlineImages;
};
struct Budget {
    size_t parts = 0, attachments = 0, decoded = 0;
};
Result parse(const std::string& raw, int depth, Budget& budget) {
    ensure(depth <= 16 && ++budget.parts <= 1000, "MIME nesting or part limit exceeded");
    auto p = split(raw);
    auto ct = content(p.headers["content-type"]), cd = content(p.headers["content-disposition"]);
    if (ct.type.empty()) ct.type = "text/plain";
    Result r;
    if (ct.type.starts_with("multipart/")) {
        auto boundary = ct.params["boundary"];
        ensure(!boundary.empty() && boundary.size() <= 200 && boundary.find_first_of("\r\n\0", 0, 3) == boundary.npos,
               "Invalid multipart boundary");
        std::string mark = "--" + boundary;
        size_t at = 0, start = std::string::npos;
        bool closed = false;
        while (at < p.body.size()) {
            auto end = p.body.find('\n', at);
            if (end == p.body.npos) end = p.body.size();
            auto line = p.body.substr(at, end - at);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            line = trim(line);
            if (line == mark || line == mark + "--") {
                if (start != std::string::npos) {
                    size_t stop = at;
                    if (stop > start && p.body[stop - 1] == '\n') stop--;
                    if (stop > start && p.body[stop - 1] == '\r') stop--;
                    auto child = parse(p.body.substr(start, stop - start), depth + 1, budget);
                    for (auto& a : child.attachments) r.attachments.push_back(std::move(a));
                    if (child.quality &&
                        (r.quality == 0 || (ct.type == "multipart/alternative" && child.quality >= r.quality))) {
                        r.text = std::move(child.text);
                        r.quality = child.quality;
                    }
                for (auto& im : child.inlineImages) r.inlineImages.push_back(std::move(im));
                }
                if (line == mark + "--") {
                    closed = true;
                    break;
                }
                start = end + 1;
            }
            at = end + 1;
        }
        ensure(closed, "Unterminated MIME multipart");
        return r;
    }
    auto enc = lower(trim(p.headers["content-transfer-encoding"]));
    std::string data;
    if (enc == "base64")
        data = base64(p.body);
    else if (enc == "quoted-printable")
        data = qp(p.body);
    else {
        ensure(enc.empty() || enc == "7bit" || enc == "8bit" || enc == "binary", "Unsupported transfer encoding");
        data = std::move(p.body);
    }
    ensure(data.size() <= maxPart && budget.decoded <= maxRaw - data.size(), "MIME decoded size limit exceeded");
    budget.decoded += data.size();
    auto name = parameter(cd, "filename");
    if (name.empty()) name = parameter(ct, "name");
    bool attachment =
        !name.empty() || cd.type == "attachment" || (!ct.type.starts_with("text/") && ct.type != "message/rfc822");
    auto contentId = p.headers["content-id"];
    if (!contentId.empty() && contentId.front() == '<') contentId = contentId.substr(1, contentId.size() - 2);
    bool inlineImage = !contentId.empty() && ct.type.starts_with("image/");
    if (attachment && !inlineImage) {
        ensure(++budget.attachments <= 100, "Too many MIME attachments");
        auto slash = name.find_last_of("/\\");
        if (slash != name.npos) name = name.substr(slash + 1);
        for (auto& c : name)
            if (static_cast<unsigned char>(c) < 32 || c == 127) c = '_';
        if (name.empty() || name == "." || name == "..") name = "附件";
        ensure(name.size() <= 4096, "Attachment name exceeds limit");
        r.attachments.push_back({name, ct.type, std::move(data)});
    } else if (inlineImage) {
        ensure(++budget.attachments <= 100, "Too many MIME attachments");
        if (name.empty()) name = contentId;
        r.inlineImages.push_back({name, ct.type, std::move(data), contentId});
    } else if (ct.type == "text/plain") {
        r.text = utf8(data, ct.params["charset"]);
        // IMAP bodies use CRLF; a bare CR renders as a box glyph in text views.
        r.text.erase(std::remove(r.text.begin(), r.text.end(), '\r'), r.text.end());
        // Some generators (Google notifications) leak Outlook conditional comment
        // markers into the plain part; they are never content.
        for (size_t cm = r.text.find("<!--["); cm != std::string::npos;
             cm = r.text.find("<!--[", cm)) {
            auto end = r.text.find("-->", cm);
            r.text.erase(cm, end == std::string::npos ? std::string::npos : end + 3 - cm);
        }
        // Marketing mail writes markdown into the plain part while the HTML part is
        // properly formatted; demote such plain parts so the HTML rendition wins.
        auto& t = r.text;
        int markdown = 0;
        for (size_t p = t.find("###"); p != t.npos; p = t.find("###", p + 3)) markdown += 2;
        for (size_t p = t.find("**"); p != t.npos; p = t.find("**", p + 2)) markdown++;
        r.quality = markdown >= 3 ? 1 : 2;
    } else if (ct.type == "text/html") {
        r.text = htmlText(utf8(data, ct.params["charset"]));
        // Prefer the HTML rendition when the plain part is markdown-styled marketing text.
        r.quality = 1;
        r.text.erase(std::remove(r.text.begin(), r.text.end(), '\r'), r.text.end());
    } else if (ct.type == "message/rfc822") {
        r = parse(data, depth + 1, budget);
    }
    return r;
}
}  // namespace
Message parseMimeMessage(std::string raw, std::string id) {
    ensure(raw.size() <= maxRaw, "Mail exceeds 64 MiB limit");
    auto p = split(raw);
    Message m;
    m.id = std::move(id);
    auto from = words(p.headers["from"]);
    auto a = from.find('<'), b = from.find('>');
    m.address = a != from.npos && b > a ? trim(from.substr(a + 1, b - a - 1)) : trim(from);
    m.sender = a != from.npos ? trim(from.substr(0, a)) : m.address;
    if (m.sender.size() > 1 && m.sender.front() == '"' && m.sender.back() == '"')
        m.sender = m.sender.substr(1, m.sender.size() - 2);
    m.subject = words(p.headers["subject"]);
    if (m.subject.empty()) m.subject = "（无主题）";
    m.date = words(p.headers["date"]);
    Budget budget;
    auto result = parse(raw, 0, budget);
    m.body = std::move(result.text);
    m.attachments = std::move(result.attachments);
    m.inlineImages = std::move(result.inlineImages);
    // Plain text parts may carry runs of blank lines; keep at most one blank line so
    // the reader does not show huge gaps.
    {
        std::string capped;
        capped.reserve(m.body.size());
        int nl = 0;
        for (char c : m.body) {
            if (c == '\n') {
                if (++nl > 2) continue;
            } else
                nl = 0;
            capped += c;
        }
        m.body = trim(capped);
    }
    if (m.body.empty()) m.body = m.attachments.empty() ? "（此邮件没有可显示的正文）" : "（此邮件仅包含附件）";
    m.preview = m.body;
    // Image records are rendering directives, never preview text.
    for (size_t cm = m.preview.find('\x1e'); cm != std::string::npos; cm = m.preview.find('\x1e')) {
        auto end = m.preview.find('\x1e', cm + 1);
        m.preview.erase(cm, end == std::string::npos ? std::string::npos : end + 1 - cm);
    }
    std::replace(m.preview.begin(), m.preview.end(), '\n', ' ');
    std::replace(m.preview.begin(), m.preview.end(), '\r', ' ');
    if (m.preview.size() > 220) {
        m.preview.resize(220);
        while (!g_utf8_validate(m.preview.data(), m.preview.size(), nullptr)) m.preview.pop_back();
    }
    return m;
}
}  // namespace forma::mail
