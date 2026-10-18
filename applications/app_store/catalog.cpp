#include "catalog.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <sstream>

#include "json_lite.hpp"

namespace fs = std::filesystem;

namespace forma::store {

namespace {

constexpr std::size_t kMaxCaptured = 64u * 1024u * 1024u;  // a runaway flatpak must not eat memory

std::string lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::string trim(std::string_view s) {
    std::size_t b = 0;
    std::size_t e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return std::string(s.substr(b, e - b));
}

bool endsWith(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
}

bool iconExists(const std::string& root, const std::string& id) {
    if (root.empty()) return false;
    std::error_code ec;
    return fs::is_regular_file(root + "/128x128/" + id + ".png", ec);
}

// --- tiny XML helpers ---------------------------------------------------------------------------

std::string decodeEntities(std::string_view in) {
    std::string out;
    out.reserve(in.size());
    for (std::size_t i = 0; i < in.size(); ++i) {
        if (in[i] != '&') {
            out += in[i];
            continue;
        }
        const std::size_t semi = in.find(';', i + 1);
        if (semi == std::string_view::npos || semi - i > 12) {
            out += in[i];
            continue;
        }
        const std::string_view name = in.substr(i + 1, semi - i - 1);
        if (name == "amp") out += '&';
        else if (name == "lt") out += '<';
        else if (name == "gt") out += '>';
        else if (name == "quot") out += '"';
        else if (name == "apos") out += '\'';
        else if (name == "nbsp") out += ' ';
        else if (name.size() > 1 && name[0] == '#') {
            unsigned cp = 0;
            const bool hex = name[1] == 'x' || name[1] == 'X';
            for (std::size_t k = hex ? 2 : 1; k < name.size(); ++k) {
                const char c = name[k];
                if (hex) {
                    if (c >= '0' && c <= '9') cp = cp * 16 + static_cast<unsigned>(c - '0');
                    else if (c >= 'a' && c <= 'f') cp = cp * 16 + static_cast<unsigned>(c - 'a' + 10);
                    else if (c >= 'A' && c <= 'F') cp = cp * 16 + static_cast<unsigned>(c - 'A' + 10);
                } else if (c >= '0' && c <= '9') {
                    cp = cp * 10 + static_cast<unsigned>(c - '0');
                }
            }
            if (cp <= 0x7F) {
                out += static_cast<char>(cp);
            } else if (cp <= 0x7FF) {
                out += static_cast<char>(0xC0 | (cp >> 6));
                out += static_cast<char>(0x80 | (cp & 0x3F));
            } else if (cp <= 0xFFFF) {
                out += static_cast<char>(0xE0 | (cp >> 12));
                out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                out += static_cast<char>(0x80 | (cp & 0x3F));
            } else {
                out += static_cast<char>(0xF0 | (cp >> 18));
                out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
                out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                out += static_cast<char>(0x80 | (cp & 0x3F));
            }
        } else {
            out += in[i];
            i = semi - 1;
            continue;
        }
        i = semi;
    }
    return out;
}

struct Tag {
    std::string name;
    std::string attributes;  ///< raw text inside <...> after the name
    bool closing = false;
    bool selfClosing = false;
};

// Parses the tag starting at `at` (which must be '<'); advances past it.
Tag readTag(std::string_view xml, std::size_t& at) {
    Tag tag;
    if (at >= xml.size() || xml[at] != '<') return tag;
    std::size_t e = xml.find('>', at + 1);
    if (e == std::string_view::npos) e = xml.size() - 1;
    std::string_view body = xml.substr(at + 1, e - at - 1);
    tag.closing = !body.empty() && body[0] == '/';
    if (tag.closing) body.remove_prefix(1);
    tag.selfClosing = !body.empty() && body.back() == '/';
    if (tag.selfClosing) body.remove_suffix(1);
    std::size_t k = 0;
    while (k < body.size() && !std::isspace(static_cast<unsigned char>(body[k])) && body[k] != '/') ++k;
    tag.name = std::string(body.substr(0, k));
    tag.attributes = std::string(body.substr(k));
    at = e + 1;
    return tag;
}

bool attrEquals(std::string_view attributes, std::string_view key, std::string_view value) {
    const std::string needle = std::string(key) + "=\"" + std::string(value) + "\"";
    return attributes.find(needle) != std::string_view::npos;
}

std::string attrValue(std::string_view attributes, std::string_view key) {
    std::string needle = std::string(key) + "=\"";
    const std::size_t at = attributes.find(needle);
    if (at == std::string_view::npos) return {};
    const std::size_t start = at + needle.size();
    const std::size_t end = attributes.find('"', start);
    if (end == std::string_view::npos) return {};
    return decodeEntities(attributes.substr(start, end - start));
}

std::string textOf(std::string_view xml, std::string_view name) {
    const std::string open = "<" + std::string(name) + ">";
    const std::size_t at = xml.find(open);
    if (at == std::string_view::npos) return {};
    const std::size_t start = at + open.size();
    const std::size_t end = xml.find("</" + std::string(name) + ">", start);
    if (end == std::string_view::npos) return {};
    return decodeEntities(trim(xml.substr(start, end - start)));
}

/// Localised text of `name`: prefers zh_CN/zh-CN, then the untagged element, then anything.
std::string localized(std::string_view xml, std::string_view name, bool chinese) {
    std::string fallback;
    std::string english;
    const std::string open = "<" + std::string(name);
    std::size_t at = 0;
    while ((at = xml.find(open, at)) != std::string_view::npos) {
        const std::size_t lineEnd = xml.find('>', at);
        if (lineEnd == std::string_view::npos) break;
        const std::string_view head = xml.substr(at, lineEnd - at);
        if (head.size() > open.size() && head[open.size()] != ' ' && head[open.size()] != '>') {
            at = lineEnd;
            continue;
        }
        const std::size_t start = lineEnd + 1;
        const std::size_t end = xml.find("</" + std::string(name) + ">", start);
        if (end == std::string_view::npos) break;
        const std::string value = decodeEntities(trim(xml.substr(start, end - start)));
        const std::size_t langAt = head.find("xml:lang=\"");
        if (langAt == std::string_view::npos) {
            if (fallback.empty()) fallback = value;
        } else {
            const std::size_t langStart = langAt + 10;
            const std::size_t langEnd = head.find('"', langStart);
            const std::string lang(head.substr(langStart, langEnd - langStart));
            const bool zh = lang.rfind("zh", 0) == 0;
            if (chinese ? zh : (lang.rfind("en", 0) == 0)) return value;
            if (!chinese && english.empty()) english = value;
        }
        at = end + 1;
    }
    if (!fallback.empty()) return fallback;
    return english;
}

/// Raw inner markup of the localised `name` element (entities stay encoded for the caller).
std::string localizedMarkup(std::string_view xml, std::string_view name, bool chinese) {
    std::string fallback;
    const std::string open = "<" + std::string(name);
    std::size_t at = 0;
    while ((at = xml.find(open, at)) != std::string_view::npos) {
        const std::size_t lineEnd = xml.find('>', at);
        if (lineEnd == std::string_view::npos) break;
        const std::string_view head = xml.substr(at, lineEnd - at);
        if (head.size() > open.size() && head[open.size()] != ' ' && head[open.size()] != '>') {
            at = lineEnd;
            continue;
        }
        const std::size_t start = lineEnd + 1;
        const std::size_t end = xml.find("</" + std::string(name) + ">", start);
        if (end == std::string_view::npos) break;
        const std::string block(xml.substr(start, end - start));
        const std::size_t langAt = head.find("xml:lang=\"");
        if (langAt == std::string_view::npos) {
            if (fallback.empty()) fallback = block;
        } else {
            const std::size_t langStart = langAt + 10;
            const std::size_t langEnd = head.find('"', langStart);
            const std::string lang(head.substr(langStart, langEnd - langStart));
            const bool zh = lang.rfind("zh", 0) == 0;
            if (chinese && zh) return block;
            if (!chinese && lang.rfind("en", 0) == 0) return block;
        }
        at = end + 1;
    }
    return fallback;
}

/// AppStream description markup -> plain text with blank lines between paragraphs and "• " bullets.
std::string descriptionText(std::string_view xml, bool chinese) {
    const std::string block = localizedMarkup(xml, "description", chinese);
    if (block.empty()) return {};
    std::string out;
    std::string paragraph;
    auto flush = [&](bool bullet) {
        // Source XML wraps paragraphs across lines; join them with a single space.
        std::string text = trim(paragraph);
        for (auto& c : text) {
            if (c == '\n' || c == '\t' || c == '\r') c = ' ';
        }
        for (std::size_t at = 0; (at = text.find("  ", at)) != std::string::npos;) text.erase(at, 1);
        if (text.empty()) return;
        if (!out.empty()) out += "\n\n";
        if (bullet) out += "• ";
        out += text;
        paragraph.clear();
    };
    bool listItem = false;
    std::size_t at = 0;
    while (at < block.size()) {
        const std::size_t lt = block.find('<', at);
        if (lt == std::string::npos) {
            paragraph += decodeEntities(block.substr(at));
            break;
        }
        paragraph += decodeEntities(block.substr(at, lt - at));
        at = lt;
        const Tag tag = readTag(block, at);
        if (tag.name == "p") {
            if (tag.closing) flush(false);
        } else if (tag.name == "li") {
            if (tag.closing) {
                flush(true);
                listItem = false;
            } else {
                if (!listItem) flush(false);
                listItem = true;
            }
        } else if (tag.name == "br") {
            paragraph += '\n';
        } else if ((tag.name == "ul" || tag.name == "ol") && tag.closing) {
            flush(false);
        }
    }
    flush(listItem);
    return out;
}

std::string plainText(std::string_view markup, bool chinese) {
    if (markup.empty()) return {};
    std::string out;
    std::size_t at = 0;
    while (at < markup.size()) {
        const std::size_t lt = markup.find('<', at);
        if (lt == std::string::npos) {
            out += decodeEntities(markup.substr(at));
            break;
        }
        out += decodeEntities(markup.substr(at, lt - at));
        at = lt;
        const Tag tag = readTag(markup, at);
        if (tag.name == "p" || tag.name == "br" || tag.name == "li") out += '\n';
        if (tag.name == "li" && !tag.closing) out += "• ";
    }
    std::string cleaned;
    for (char c : out) {
        if (c == '\n') {
            if (!cleaned.empty() && cleaned.back() != '\n') cleaned += '\n';
        } else {
            cleaned += c;
        }
    }
    (void)chinese;
    return trim(cleaned);
}

std::string categoryOf(const std::string& id) {
    std::string best;
    const std::string lowerId = lower(id);
    if (lowerId.find("game") != std::string::npos) best = "Game";
    return best;
}

std::uint64_t sizeFromText(std::string_view text) {
    std::string cleaned;
    for (char c : text) {
        if (c == '\xC2' || c == '\xA0' || c == ',') continue;  // nbsp / thin spaces as bytes
        cleaned += c;
    }
    double value = 0;
    char unit[8] = {};
    if (std::sscanf(cleaned.c_str(), "%lf%7s", &value, unit) != 2) return 0;
    const std::string u = unit;
    double scale = 1;
    if (u == "B" || u == "byte" || u == "bytes") scale = 1;
    else if (u == "kB" || u == "KB" || u == "kilobyte" || u == "kilobytes") scale = 1000;
    else if (u == "MB" || u == "megabyte" || u == "megabytes") scale = 1000.0 * 1000;
    else if (u == "GB" || u == "gigabyte" || u == "gigabytes") scale = 1000.0 * 1000 * 1000;
    else if (u == "KiB") scale = 1024;
    else if (u == "MiB") scale = 1024.0 * 1024;
    else if (u == "GiB") scale = 1024.0 * 1024 * 1024;
    return static_cast<std::uint64_t>(value * scale);
}

}  // namespace

bool validAppId(std::string_view id) {
    if (id.empty() || id.size() > 255 || id[0] == '-') return false;
    bool dot = false;
    for (char c : id) {
        if (c == '.') {
            dot = true;
            continue;
        }
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-')) return false;
    }
    return dot;
}

CommandResult run(const std::vector<std::string>& argv, int timeoutSeconds,
                  const std::function<void(std::string_view)>& onOutput) {
    CommandResult result;
    if (argv.empty()) return result;
    for (const auto& a : argv) {
        if (a.find('\0') != std::string::npos) return result;
    }
    int pipeFds[2];
    if (::pipe2(pipeFds, O_CLOEXEC) != 0) return result;
    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(pipeFds[0]);
        ::close(pipeFds[1]);
        return result;
    }
    if (pid == 0) {
        ::setpgid(0, 0);
        ::dup2(pipeFds[1], STDOUT_FILENO);
        ::dup2(pipeFds[1], STDERR_FILENO);
        ::close(pipeFds[0]);
        ::close(pipeFds[1]);
        const int devnull = ::open("/dev/null", O_RDONLY);
        if (devnull >= 0) {
            ::dup2(devnull, STDIN_FILENO);
            ::close(devnull);
        }
        ::setenv("LC_ALL", "C", 1);
        ::setenv("LANG", "C", 1);
        ::unsetenv("LANGUAGE");
        std::vector<char*> args;
        args.reserve(argv.size() + 1);
        for (const auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
        args.push_back(nullptr);
        ::execvp(args[0], args.data());
        ::_exit(127);
    }
    ::setpgid(pid, pid);
    ::close(pipeFds[1]);
    const auto start = std::chrono::steady_clock::now();
    bool killed = false;
    std::array<char, 16384> buffer{};
    for (;;) {
        const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        int waitMs = 50;
        if (timeoutSeconds > 0) {
            const double remaining = static_cast<double>(timeoutSeconds) - elapsed;
            if (remaining <= 0) {
                if (!killed) {
                    ::kill(-pid, SIGKILL);
                    ::kill(pid, SIGKILL);
                    killed = true;
                    result.timedOut = true;
                }
                waitMs = 20;
            } else {
                waitMs = std::min(50, std::max(1, static_cast<int>(remaining * 1000)));
            }
        }
        pollfd pfd{pipeFds[0], POLLIN, 0};
        const int ready = ::poll(&pfd, 1, waitMs);
        if (ready > 0) {
            const ssize_t n = ::read(pipeFds[0], buffer.data(), buffer.size());
            if (n > 0) {
                // Always drain the pipe (a full pipe blocks flatpak), but stop growing the captured text
                // once the cap is reached.
                if (result.output.size() < kMaxCaptured) {
                    const std::size_t keep = std::min(static_cast<std::size_t>(n), kMaxCaptured - result.output.size());
                    result.output.append(buffer.data(), keep);
                }
                if (onOutput) onOutput(std::string_view(buffer.data(), static_cast<std::size_t>(n)));
                continue;
            }
            if (n == 0) break;
            if (errno == EINTR) continue;
            break;
        } else if (ready == 0 && result.timedOut) {
            // The child is gone after the kill; the loop below reaps it.
            int status = 0;
            if (::waitpid(pid, &status, WNOHANG) == pid) {
                result.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
                ::close(pipeFds[0]);
                return result;
            }
        }
    }
    ::close(pipeFds[0]);
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    if (result.timedOut) {
        result.exitCode = 124;
    } else {
        result.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    }
    return result;
}

bool launchDetached(const std::vector<std::string>& argv) {
    if (argv.empty()) return false;
    const pid_t pid = ::fork();
    if (pid < 0) return false;
    if (pid == 0) {
        ::setsid();
        const pid_t second = ::fork();
        if (second != 0) ::_exit(0);
        const int devnull = ::open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            ::dup2(devnull, STDIN_FILENO);
            ::dup2(devnull, STDOUT_FILENO);
            ::dup2(devnull, STDERR_FILENO);
            if (devnull > 2) ::close(devnull);
        }
        std::vector<char*> args;
        for (const auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
        args.push_back(nullptr);
        ::execvp(args[0], args.data());
        ::_exit(127);
    }
    int status = 0;
    ::waitpid(pid, &status, 0);
    return true;
}

// --- AppStream ----------------------------------------------------------------------------------

std::vector<AppEntry> parseAppStream(std::string_view xml, const std::string& iconRoot) {
    std::vector<AppEntry> apps;
    const bool chinese = true;  // the store UI is Simplified Chinese; fall back to English
    std::size_t at = 0;
    while ((at = xml.find("<component ", at)) != std::string_view::npos) {
        const std::size_t headEnd = xml.find('>', at);
        if (headEnd == std::string_view::npos) break;
        const std::string_view head = xml.substr(at, headEnd - at);
        if (!attrEquals(head, "type", "desktop-application")) {
            at = headEnd;
            continue;
        }
        const std::size_t end = xml.find("</component>", headEnd);
        if (end == std::string_view::npos) break;
        const std::string_view body = xml.substr(headEnd, end - headEnd);

        AppEntry app;
        app.id = textOf(body, "id");
        if (!validAppId(app.id)) {
            at = end + 1;
            continue;
        }
        app.name = localized(body, "name", chinese);
        if (app.name.empty()) app.name = app.id;
        app.summary = localized(body, "summary", chinese);
        app.developer = localized(body, "developer_name", chinese);
        if (app.developer.empty()) {
            const std::size_t devAt = body.find("<developer");
            if (devAt != std::string_view::npos) {
                const std::size_t devEnd = body.find("</developer>", devAt);
                if (devEnd != std::string_view::npos)
                    app.developer = localized(body.substr(devAt, devEnd - devAt), "name", chinese);
            }
        }
        app.description = descriptionText(body, chinese);
        app.license = textOf(body, "project_license");
        app.version = textOf(body, "version");
        app.runtime = attrValue(body, "runtime");

        // URLs.
        std::size_t urlAt = 0;
        while ((urlAt = body.find("<url ", urlAt)) != std::string_view::npos) {
            const std::size_t tagEnd = body.find('>', urlAt);
            const std::size_t close = body.find("</url>", tagEnd);
            if (tagEnd == std::string_view::npos || close == std::string_view::npos) break;
            const std::string_view tagHead = body.substr(urlAt, tagEnd - urlAt);
            if (attrEquals(tagHead, "type", "homepage"))
                app.homepage = decodeEntities(trim(body.substr(tagEnd + 1, close - tagEnd - 1)));
            urlAt = close + 6;
        }

        // Categories.
        std::size_t catAt = 0;
        while ((catAt = body.find("<category>", catAt)) != std::string_view::npos) {
            const std::size_t catEnd = body.find("</category>", catAt);
            if (catEnd == std::string_view::npos) break;
            app.categories.push_back(decodeEntities(trim(body.substr(catAt + 10, catEnd - catAt - 10))));
            catAt = catEnd + 11;
        }

        // Keywords (untranslated set only; enough for search).
        const std::size_t kwAt = body.find("<keywords>");
        if (kwAt != std::string_view::npos) {
            const std::size_t kwEnd = body.find("</keywords>", kwAt);
            if (kwEnd != std::string_view::npos) {
                const std::string_view block = body.substr(kwAt, kwEnd - kwAt);
                std::size_t kAt = 0;
                while ((kAt = block.find("<keyword>", kAt)) != std::string_view::npos) {
                    const std::size_t kEnd = block.find("</keyword>", kAt);
                    if (kEnd == std::string_view::npos) break;
                    app.keywords.push_back(decodeEntities(trim(block.substr(kAt + 9, kEnd - kAt - 9))));
                    kAt = kEnd + 10;
                }
            }
        }

        // Screenshots: prefer the default shot, then document order. Thumbnails of about 752 px are the
        // right size for cards and still crisp in the detail gallery.
        std::size_t shotAt = 0;
        while ((shotAt = body.find("<screenshot", shotAt)) != std::string_view::npos) {
            const std::size_t shotTagEnd = body.find('>', shotAt);
            const std::size_t shotEnd = body.find("</screenshot>", shotAt);
            if (shotTagEnd == std::string_view::npos || shotEnd == std::string_view::npos) break;
            const std::string_view block = body.substr(shotTagEnd, shotEnd - shotTagEnd);
            Screenshot shot;
            shot.caption = localized(block, "caption", chinese);
            std::size_t imgAt = 0;
            while ((imgAt = block.find("<image ", imgAt)) != std::string_view::npos) {
                const std::size_t imgTagEnd = block.find('>', imgAt);
                const std::size_t imgEnd = block.find("</image>", imgAt);
                if (imgTagEnd == std::string_view::npos || imgEnd == std::string_view::npos) break;
                const std::string_view headTag = block.substr(imgAt, imgTagEnd - imgAt);
                const std::string url = decodeEntities(trim(block.substr(imgTagEnd + 1, imgEnd - imgTagEnd - 1)));
                const int width = std::atoi(attrValue(headTag, "width").c_str());
                const int height = std::atoi(attrValue(headTag, "height").c_str());
                if (attrEquals(headTag, "type", "source")) {
                    shot.source = url;
                    shot.width = width;
                    shot.height = height;
                } else if (attrEquals(headTag, "type", "thumbnail")) {
                    if (shot.thumbnail.empty() || (width >= 600 && width <= 900)) {
                        if (width >= 600 || shot.thumbnail.empty()) shot.thumbnail = url;
                    }
                }
                imgAt = imgEnd + 8;
            }
            if (shot.thumbnail.empty()) shot.thumbnail = shot.source;
            if (!shot.thumbnail.empty() || !shot.source.empty()) app.screenshots.push_back(std::move(shot));
            shotAt = shotEnd + 13;
        }

        // Branding colour.
        const std::size_t brandAt = body.find("<branding>");
        if (brandAt != std::string_view::npos) {
            const std::size_t brandEnd = body.find("</branding>", brandAt);
            if (brandEnd != std::string_view::npos) {
                const std::string_view block = body.substr(brandAt, brandEnd - brandAt);
                std::size_t colorAt = 0;
                while ((colorAt = block.find("<color ", colorAt)) != std::string_view::npos) {
                    const std::size_t tagEnd = block.find('>', colorAt);
                    const std::size_t close = block.find("</color>", colorAt);
                    if (tagEnd == std::string_view::npos || close == std::string_view::npos) break;
                    const std::string_view headTag = block.substr(colorAt, tagEnd - colorAt);
                    const std::string value = trim(block.substr(tagEnd + 1, close - tagEnd - 1));
                    if (attrEquals(headTag, "type", "primary") && attrEquals(headTag, "scheme_preference", "light")) {
                        app.accent = value;
                        break;
                    }
                    if (attrEquals(headTag, "type", "primary") && app.accent.empty()) app.accent = value;
                    colorAt = close + 8;
                }
            }
        }

        // Newest release (the first <release> in document order).
        const std::size_t relAt = body.find("<release ");
        if (relAt != std::string_view::npos) {
            const std::size_t tagEnd = body.find('>', relAt);
            if (tagEnd != std::string_view::npos) {
                const std::string_view headTag = body.substr(relAt, tagEnd - relAt);
                const std::string version = attrValue(headTag, "version");
                if (!version.empty()) app.version = version;
                app.releaseTimestamp = std::atoll(attrValue(headTag, "timestamp").c_str());
                if (headTag.back() != '/') {
                    const std::size_t close = body.find("</release>", tagEnd);
                    if (close != std::string_view::npos)
                        app.releaseNotes = plainText(body.substr(tagEnd + 1, close - tagEnd - 1), chinese);
                }
            }
        }

        // Flathub verification flag.
        app.verified = body.find("flathub::verification::verified\">true<") != std::string_view::npos;
        app.eol = body.find("<eol") != std::string_view::npos;

        // Age rating, the way OARS maps content attributes onto a minimum age.
        int age = 0;
        const std::size_t ratingAt = body.find("<content_rating");
        if (ratingAt != std::string_view::npos) {
            const std::size_t ratingEnd = body.find("</content_rating>", ratingAt);
            const std::string_view rating = body.substr(ratingAt, ratingEnd == std::string_view::npos
                                                                       ? body.size() - ratingAt
                                                                       : ratingEnd - ratingAt);
            age = 4;  // any rating at all is at least 4+
            std::size_t attrAt = 0;
            while ((attrAt = rating.find("<content_attribute id=\"", attrAt)) != std::string_view::npos) {
                const std::size_t idStart = attrAt + std::strlen("<content_attribute id=\"");
                const std::size_t idEnd = rating.find('"', idStart);
                if (idEnd == std::string_view::npos) break;
                const std::string_view id = rating.substr(idStart, idEnd - idStart);
                const std::size_t valueStart = rating.find('>', idEnd);
                const std::size_t valueEnd = rating.find('<', valueStart);
                const std::string_view level = valueStart == std::string_view::npos
                                                   ? std::string_view()
                                                   : rating.substr(valueStart + 1, valueEnd - valueStart - 1);
                const bool drugs = id.rfind("drugs-", 0) == 0;
                const bool intense = level == "intense";
                const bool moderate = level == "moderate";
                if (drugs && (intense || moderate)) age = std::max(age, 17);
                else if (intense) age = std::max(age, 17);
                else if (moderate) age = std::max(age, 12);
                else if (level == "mild") age = std::max(age, 9);
                attrAt = valueEnd;
            }
        }
        app.ageRating = age;

        // Icons: a cached PNG next to the AppStream file, else a remote URL.
        if (iconExists(iconRoot, app.id)) {
            app.iconPath = iconRoot + "/128x128/" + app.id + ".png";
        } else {
            std::size_t iconAt = 0;
            while ((iconAt = body.find("<icon ", iconAt)) != std::string_view::npos) {
                const std::size_t tagEnd = body.find('>', iconAt);
                const std::size_t close = body.find("</icon>", iconAt);
                if (tagEnd == std::string_view::npos || close == std::string_view::npos) break;
                const std::string_view headTag = body.substr(iconAt, tagEnd - iconAt);
                if (attrEquals(headTag, "type", "remote")) {
                    app.iconUrl = decodeEntities(trim(body.substr(tagEnd + 1, close - tagEnd - 1)));
                    if (app.iconUrl.find("128x128/") != std::string::npos) break;
                }
                iconAt = close + 7;
            }
        }
        if (app.categories.empty() && !categoryOf(app.id).empty()) app.categories.push_back(categoryOf(app.id));
        apps.push_back(std::move(app));
        at = end + 1;
    }
    return apps;
}

std::string findAppStreamFile(const std::vector<std::string>& extraRoots) {
    std::vector<std::string> roots = extraRoots;
    // Flatpak keeps its AppStream cache under its own data roots, not necessarily on XDG_DATA_DIRS
    // (/var/lib/flatpak/appstream is the system one on most distributions).
    for (const char* standard : {"/var/lib/flatpak/appstream", "/usr/local/share/flatpak/appstream",
                                 "/usr/share/flatpak/appstream"}) {
        if (fs::is_directory(standard)) roots.emplace_back(standard);
    }
    if (const char* home = std::getenv("HOME")) {
        for (const std::string& data : {std::string(home) + "/.local/share/flatpak/appstream",
                                        std::string(home) + "/.var/app"}) {
            if (fs::is_directory(data)) roots.push_back(data);
        }
    }
    const char* xdg = std::getenv("XDG_DATA_HOME");
    if (xdg && *xdg) {
        const std::string data = std::string(xdg) + "/flatpak/appstream";
        if (fs::is_directory(data)) roots.push_back(data);
    }
    const char* dirs = std::getenv("XDG_DATA_DIRS");
    const std::string searchDirs = dirs && *dirs ? dirs : "/usr/local/share:/usr/share";
    std::size_t start = 0;
    while (start <= searchDirs.size()) {
        const std::size_t end = searchDirs.find(':', start);
        const std::string dir = searchDirs.substr(start, end == std::string::npos ? std::string::npos : end - start);
        const std::string data = dir + "/flatpak/appstream";
        if (!dir.empty() && fs::is_directory(data)) roots.push_back(data);
        if (end == std::string::npos) break;
        start = end + 1;
    }
    std::string newest;
    fs::file_time_type newestTime{};
    for (const auto& root : roots) {
        std::error_code ec;
        for (const auto& remote : fs::directory_iterator(root, ec)) {
            if (!remote.is_directory()) continue;
            for (const auto& arch : fs::directory_iterator(remote.path(), ec)) {
                const fs::path file = arch.path() / "active/appstream.xml";
                if (!fs::is_regular_file(file, ec)) continue;
                const auto time = fs::last_write_time(file, ec);
                if (ec) continue;
                if (newest.empty() || time > newestTime) {
                    newest = file.string();
                    newestTime = time;
                }
            }
        }
    }
    return newest;
}

// --- Flatpak queries ----------------------------------------------------------------------------

std::vector<AppEntry> listInstalled() {
    std::vector<AppEntry> apps;
    const CommandResult r =
        run({"flatpak", "list", "--app", "--json",
             "--columns=application,name,version,branch,arch,origin,installation"}, 20);
    if (r.exitCode != 0) return apps;
    const json::Value parsed = json::Value::parse(r.output);
    for (const auto& item : parsed.array()) {
        AppEntry app;
        app.id = item["application_id"].str();
        if (!validAppId(app.id)) continue;
        app.name = item["name"].str(app.id);
        app.installedVersion = item["version"].str();
        app.version = app.installedVersion;
        app.branch = item["branch"].str("stable");
        app.arch = item["arch"].str("x86_64");
        app.origin = item["origin"].str();
        app.installation = item["installation"].str();
        app.installed = true;
        apps.push_back(std::move(app));
    }
    return apps;
}

std::vector<std::string> listRemotes() {
    std::vector<std::string> remotes;
    const CommandResult r = run({"flatpak", "remotes", "--json", "--columns=name,url,options"}, 15);
    if (r.exitCode != 0) return remotes;
    const json::Value parsed = json::Value::parse(r.output);
    for (const auto& item : parsed.array()) {
        const std::string name = item["name"].str();
        if (!name.empty()) remotes.push_back(name);
    }
    return remotes;
}

std::map<std::string, std::pair<std::uint64_t, std::uint64_t>> remoteSizes() {
    std::map<std::string, std::pair<std::uint64_t, std::uint64_t>> sizes;
    const CommandResult r = run(
        {"flatpak", "remote-ls", "--app", "--cached", "--json",
         "--columns=application,download-size,installed-size"}, 60);
    if (r.exitCode != 0) return sizes;
    const json::Value parsed = json::Value::parse(r.output);
    for (const auto& item : parsed.array()) {
        const std::string id = item["application_id"].str();
        if (id.empty()) continue;
        sizes[id] = {sizeFromText(item["download_size"].str()), sizeFromText(item["installed_size"].str())};
    }
    return sizes;
}

std::string iconRootFor(const std::string& appStreamPath) {
    if (appStreamPath.empty()) return {};
    std::string dir = appStreamPath;
    const std::string file = "/appstream.xml";
    if (endsWith(dir, file)) dir.resize(dir.size() - file.size());
    // The AppStream file lives in <remote>/<arch>/active/; its icons/ folder holds the cached PNGs
    // (some flatpak versions put them straight into the active folder).
    std::error_code ec;
    if (!fs::exists(dir + "/128x128", ec) && fs::exists(dir + "/icons/128x128", ec)) return dir + "/icons";
    return dir;
}

Catalog loadCatalog(std::string appStreamPath) {
    Catalog catalog;
    catalog.flatpakAvailable = std::filesystem::exists("/usr/bin/flatpak") ||
                               std::filesystem::exists("/usr/local/bin/flatpak");
    if (appStreamPath.empty()) appStreamPath = findAppStreamFile();
    catalog.appStreamPath = appStreamPath;
    const std::string iconRoot = iconRootFor(appStreamPath);
    if (!appStreamPath.empty()) {
        std::ifstream in(appStreamPath, std::ios::binary);
        if (in) {
            std::ostringstream buffer;
            buffer << in.rdbuf();
            catalog.apps = parseAppStream(buffer.str(), iconRoot);
        } else {
            catalog.error = "无法读取 AppStream 元数据：" + appStreamPath;
        }
    }
    if (catalog.apps.empty() && !appStreamPath.empty()) {
        // Fall back to flatpak's own view of the catalogue when AppStream cannot be parsed.
        const CommandResult r = run({"flatpak", "remote-ls", "--app", "--cached", "--json",
                                     "--columns=application,name,version,branch,arch,origin"}, 60);
        if (r.exitCode == 0) {
            const json::Value parsed = json::Value::parse(r.output);
            for (const auto& item : parsed.array()) {
                AppEntry app;
                app.id = item["application_id"].str();
                if (!validAppId(app.id)) continue;
                app.name = item["name"].str(app.id);
                app.version = item["version"].str();
                app.branch = item["branch"].str("stable");
                app.arch = item["arch"].str("x86_64");
                app.origin = item["origin"].str();
                catalog.apps.push_back(std::move(app));
            }
        }
    }
    for (auto& app : catalog.apps) catalog.byId.emplace(app.id, catalog.byId.size());

    // Installed applications: enrich entries AppStream knows, add the rest (local bundles, side-loads).
    for (auto& installed : listInstalled()) {
        auto it = catalog.byId.find(installed.id);
        if (it != catalog.byId.end()) {
            AppEntry& app = catalog.apps[it->second];
            app.installed = true;
            app.installedVersion = installed.installedVersion;
            app.installation = installed.installation;
            if (app.branch.empty()) app.branch = installed.branch;
            if (!installed.origin.empty()) app.origin = installed.origin;
            if (app.name.empty()) app.name = installed.name;
        } else {
            catalog.byId.emplace(installed.id, catalog.apps.size());
            catalog.apps.push_back(std::move(installed));
        }
    }

    const auto sizes = remoteSizes();
    for (const auto& [id, size] : sizes) {
        auto it = catalog.byId.find(id);
        if (it == catalog.byId.end()) continue;
        catalog.apps[it->second].downloadSize = size.first;
        catalog.apps[it->second].installedSize = size.second;
    }
    for (auto& app : catalog.apps) {
        if (app.installed && !app.installedVersion.empty() && !app.version.empty() &&
            app.installedVersion != app.version) {
            // Optimistic: AppStream's latest version is newer than what is installed.
            app.updateAvailable = true;
        }
    }
    catalog.remotes = listRemotes();
    return catalog;
}

std::vector<std::string> queryUpdates() {
    std::vector<std::string> ids;
    const CommandResult r =
        run({"flatpak", "remote-ls", "--app", "--updates", "--json", "--columns=application"}, 90);
    if (r.exitCode != 0) return ids;
    const json::Value parsed = json::Value::parse(r.output);
    for (const auto& item : parsed.array()) {
        const std::string id = item["application_id"].str();
        if (validAppId(id)) ids.push_back(id);
    }
    return ids;
}

// --- Catalog queries ----------------------------------------------------------------------------

const AppEntry* Catalog::find(std::string_view id) const {
    const auto it = byId.find(std::string(id));
    return it == byId.end() ? nullptr : &apps[it->second];
}

namespace {

bool matches(const AppEntry& app, std::string_view query) {
    const std::string q = lower(query);
    if (q.empty()) return false;
    if (lower(app.name).find(q) != std::string::npos) return true;
    if (lower(app.id).find(q) != std::string::npos) return true;
    if (lower(app.summary).find(q) != std::string::npos) return true;
    if (lower(app.developer).find(q) != std::string::npos) return true;
    for (const auto& k : app.keywords)
        if (lower(k).find(q) != std::string::npos) return true;
    for (const auto& c : app.categories)
        if (lower(c).find(q) != std::string::npos) return true;
    return false;
}

}  // namespace

std::vector<const AppEntry*> Catalog::search(std::string_view query, std::size_t limit) const {
    std::vector<const AppEntry*> found;
    const std::string q = trim(query);
    if (q.empty()) return found;
    std::vector<std::pair<int, const AppEntry*>> scored;
    for (const auto& app : apps) {
        if (!matches(app, q)) continue;
        const std::string name = lower(app.name);
        int score = 0;
        if (name == lower(q)) score = 100;
        else if (name.rfind(lower(q), 0) == 0) score = 80;
        else if (name.find(lower(q)) != std::string::npos) score = 60;
        else if (lower(app.summary).find(lower(q)) != std::string::npos) score = 40;
        else if (lower(app.developer).find(lower(q)) != std::string::npos) score = 20;
        else score = 10;
        if (app.installed) score += 5;
        scored.emplace_back(-score, &app);
    }
    std::stable_sort(scored.begin(), scored.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& [score, app] : scored) {
        if (found.size() >= limit) break;
        found.push_back(app);
    }
    return found;
}

std::vector<const AppEntry*> Catalog::inCategory(std::string_view category, std::size_t limit) const {
    std::vector<const AppEntry*> found;
    for (const auto& app : apps) {
        if (app.categories.empty()) continue;
        if (std::find(app.categories.begin(), app.categories.end(), category) != app.categories.end()) {
            found.push_back(&app);
            if (found.size() >= limit) break;
        }
    }
    return found;
}

std::vector<const AppEntry*> Catalog::installedApps() const {
    std::vector<const AppEntry*> found;
    for (const auto& app : apps)
        if (app.installed) found.push_back(&app);
    std::sort(found.begin(), found.end(), [](const AppEntry* a, const AppEntry* b) {
        return lower(a->name) < lower(b->name);
    });
    return found;
}

std::vector<const AppEntry*> Catalog::updates() const {
    std::vector<const AppEntry*> found;
    for (const auto& app : apps)
        if (app.installed && app.updateAvailable) found.push_back(&app);
    return found;
}

std::string AppEntry::primaryCategory() const {
    for (const auto& c : categories) {
        if (c != "GTK" && c != "Qt" && c != "GNOME" && c != "KDE") return c;
    }
    return categories.empty() ? std::string() : categories.front();
}

// --- formatting ----------------------------------------------------------------------------------

std::string formatBytes(std::uint64_t bytes) {
    char buffer[48];
    if (bytes == 0) return "—";
    if (bytes < 1000ull * 1000) {
        std::snprintf(buffer, sizeof buffer, "%.1f KB", static_cast<double>(bytes) / 1000.0);
    } else if (bytes < 1000ull * 1000 * 1000) {
        std::snprintf(buffer, sizeof buffer, "%.1f MB", static_cast<double>(bytes) / (1000.0 * 1000.0));
    } else {
        std::snprintf(buffer, sizeof buffer, "%.2f GB", static_cast<double>(bytes) / (1000.0 * 1000.0 * 1000.0));
    }
    return buffer;
}

std::string formatDate(std::int64_t unixSeconds) {
    if (unixSeconds <= 0) return {};
    std::time_t t = static_cast<std::time_t>(unixSeconds);
    std::tm tm{};
    localtime_r(&t, &tm);
    char buffer[64];
    std::strftime(buffer, sizeof buffer, "%Y年%m月%d日", &tm);
    return buffer;
}

std::string categoryName(std::string_view category) {
    static const std::map<std::string_view, std::string_view, std::less<>> names{
        {"AudioVideo", "音频与视频"}, {"Audio", "音乐"}, {"Video", "视频"}, {"Player", "播放器"},
        {"Music", "音乐"}, {"Graphics", "图形与设计"}, {"Photography", "摄影"}, {"2DGraphics", "二维图形"},
        {"3DGraphics", "三维图形"}, {"VectorGraphics", "矢量图形"}, {"RasterGraphics", "位图"},
        {"Publishing", "出版"}, {"Office", "办公"}, {"Finance", "财务"}, {"Productivity", "效率"},
        {"ProjectManagement", "项目管理"}, {"Calculator", "计算器"}, {"Spreadsheet", "电子表格"},
        {"WordProcessor", "文字处理"}, {"Presentation", "演示文稿"}, {"Development", "开发"},
        {"IDE", "集成开发环境"}, {"Debugger", "调试器"}, {"WebDevelopment", "网页开发"},
        {"TextEditor", "文本编辑"}, {"Building", "构建"}, {"Education", "教育"}, {"Science", "科学"},
        {"Math", "数学"}, {"Dictionary", "词典"}, {"Literature", "文学"}, {"Languages", "语言"},
        {"Utility", "实用工具"}, {"System", "系统"}, {"Settings", "设置"}, {"FileTools", "文件工具"},
        {"FileManager", "文件管理"}, {"TerminalEmulator", "终端"}, {"Security", "安全"},
        {"Network", "网络"}, {"FileTransfer", "文件传输"}, {"WebBrowser", "浏览器"},
        {"InstantMessaging", "即时通讯"}, {"Chat", "聊天"}, {"Email", "邮件"}, {"Game", "游戏"},
        {"ArcadeGame", "街机游戏"}, {"ActionGame", "动作游戏"}, {"StrategyGame", "策略游戏"},
        {"Simulation", "模拟"}, {"Emulator", "模拟器"}, {"Engineering", "工程"}, {"Medical", "医疗"},
        {"Accessibility", "辅助功能"}, {"DesktopSettings", "桌面设置"}, {"Amusement", "娱乐"},
        {"Clock", "时钟"}, {"Calendar", "日历"}, {"ContactManagement", "联系人"},
    };
    if (const auto it = names.find(category); it != names.end()) return std::string(it->second);
    return std::string(category);
}

std::string sectionName(std::string_view section) {
    static const std::map<std::string_view, std::string_view, std::less<>> names{
        {"discover", "精选"}, {"create", "创意"}, {"work", "工作"}, {"play", "娱乐"},
        {"develop", "开发"}, {"learn", "学习"}, {"utilities", "实用工具"}, {"categories", "类别"},
        {"updates", "更新"}, {"installed", "已安装"}, {"search", "搜索"},
    };
    if (const auto it = names.find(section); it != names.end()) return std::string(it->second);
    return std::string(section);
}

const std::vector<Section>& sections() {
    static const std::vector<Section> all{
        {"create", "创意", "绘画、摄影、音乐与视频创作", {"Graphics", "Photography", "AudioVideoEditing",
                                                    "2DGraphics", "3DGraphics", "VectorGraphics",
                                                    "RasterGraphics", "Publishing", "Audio"},
         {"org.kde.krita", "org.blender.Blender", "org.inkscape.Inkscape", "org.gimp.GIMP",
          "com.obsproject.Studio", "org.audacityteam.Audacity", "org.gnome.Shotwell"}},
        {"work", "工作", "写作、办公与协作", {"Office", "Finance", "Productivity", "ProjectManagement",
                                          "Calculator", "Spreadsheet", "WordProcessor", "Presentation"},
         {"org.libreoffice.LibreOffice", "org.onlyoffice.desktopeditors", "md.obsidian.Obsidian",
          "org.gnome.Calendar", "org.gnome.TextEditor"}},
        {"play", "娱乐", "游戏、音乐与影音", {"Game", "ArcadeGame", "ActionGame", "StrategyGame", "Simulation",
                                          "Emulator", "AudioVideo", "Video", "Player", "Music"},
         {"com.spotify.Client", "org.videolan.VLC", "io.github.celluloid_player.Celluloid", "org.gnome.Music"}},
        {"develop", "开发", "编辑器、工具链与终端", {"Development", "IDE", "Debugger", "WebDevelopment",
                                               "TextEditor", "Building", "TerminalEmulator"},
         {"com.visualstudio.code", "org.gnome.Builder", "app.devsuite.Ptyxis", "org.remmina.Remmina",
          "org.filezillaproject.Filezilla"}},
        {"learn", "学习", "阅读、科学与语言", {"Education", "Science", "Math", "Dictionary", "Literature",
                                          "Languages", "Engineering", "Medical"},
         {"org.gnome.Maps", "org.gnome.Weather", "com.belmoussaoui.Decoder"}},
        {"utilities", "实用工具", "让桌面更顺手的小工具", {"Utility", "System", "Settings", "FileTools",
                                                    "FileManager", "Security", "Network", "FileTransfer",
                                                    "WebBrowser", "Accessibility", "Clock", "Calendar"},
         {"com.github.tchx84.Flatseal", "net.nokyan.Resources", "io.missioncenter.MissionCenter",
          "org.gnome.Loupe", "org.gnome.Evince", "org.chromium.Chromium", "com.google.Chrome"}},
    };
    return all;
}

const Section* section(std::string_view id) {
    for (const auto& s : sections())
        if (s.id == id) return &s;
    return nullptr;
}

std::vector<std::string> discoverOrder() {
    return {
        "org.telegram.desktop", "com.spotify.Client", "org.videolan.VLC", "org.gimp.GIMP",
        "org.libreoffice.LibreOffice", "md.obsidian.Obsidian", "com.github.tchx84.Flatseal",
        "org.gnome.Loupe", "net.nokyan.Resources", "org.gnome.Calendar", "org.gnome.Maps",
        "org.gnome.TextEditor", "org.onlyoffice.desktopeditors", "org.remmina.Remmina",
        "com.belmoussaoui.Decoder", "io.github.celluloid_player.Celluloid", "org.gnome.Builder",
        "org.kde.krita", "org.inkscape.Inkscape", "org.blender.Blender", "org.audacityteam.Audacity",
        "com.obsproject.Studio", "org.gnome.Shotwell", "org.gnome.Fractal", "io.missioncenter.MissionCenter",
        "org.gnome.Epiphany", "org.chromium.Chromium", "com.google.Chrome", "com.discordapp.Discord",
        "org.signal.Signal", "us.zoom.Zoom", "org.filezillaproject.Filezilla", "com.usebottles.bottles",
        "io.github.dvlv.boxbuddyrs", "com.visualstudio.code", "org.gnome.Music", "org.gnome.Weather",
        "org.gnome.Snapshot", "org.gnome.SoundRecorder", "org.gnome.Evince", "app.devsuite.Ptyxis",
        "app.drey.Damask", "com.github.phase1geo.minder", "org.gnome.gitlab.somas.Apostrophe",
        "org.gnome.Boxes", "org.gnome.Polari", "org.gnome.Cheese", "org.gnome.Clocks",
        "org.gnome.Contacts", "org.gnome.Dictionary", "org.gnome.DiskUtility", "org.gnome.Logs",
        "org.gnome.SystemMonitor", "org.gnome.Totem", "org.gnome.Tour", "org.gnome.Console",
    };
}

std::vector<std::string> heroOrder() {
    return {"com.spotify.Client", "org.blender.Blender", "org.telegram.desktop", "org.kde.krita",
            "com.obsproject.Studio", "org.gnome.Maps"};
}

std::vector<std::size_t> matchPositions(std::string_view text, std::string_view query) {
    std::vector<std::size_t> positions;
    if (query.empty()) return positions;
    std::size_t at = 0;
    for (char c : query) {
        const char want = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        bool found = false;
        while (at < text.size()) {
            if (static_cast<char>(std::tolower(static_cast<unsigned char>(text[at]))) == want) {
                positions.push_back(at);
                ++at;
                found = true;
                break;
            }
            ++at;
        }
        if (!found) return {};
    }
    return positions;
}

}  // namespace forma::store
