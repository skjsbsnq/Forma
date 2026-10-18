#include "json_lite.hpp"

#include <cstdlib>

namespace forma::store::json {

namespace {

const Value& nullValue() {
    static const Value v;
    return v;
}

const std::vector<Value>& emptyArray() {
    static const std::vector<Value> v;
    return v;
}

const std::map<std::string, Value, std::less<>>& emptyObject() {
    static const std::map<std::string, Value, std::less<>> v;
    return v;
}

const std::string& emptyString() {
    static const std::string s;
    return s;
}

void appendUtf8(std::string& out, unsigned cp) {
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
}

}  // namespace

class Parser {
public:
    explicit Parser(std::string_view text) : m_text(text) {}

    Value document() {
        skipSpace();
        Value v = value(0);
        // Trailing garbage means the document was truncated or is not JSON at all.
        skipSpace();
        if (m_at != m_text.size()) return Value();
        return v;
    }

private:
    static constexpr int kMaxDepth = 48;

    void skipSpace() {
        while (m_at < m_text.size()) {
            const char c = m_text[m_at];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++m_at;
            } else {
                break;
            }
        }
    }

    [[nodiscard]] bool consume(char c) {
        skipSpace();
        if (m_at < m_text.size() && m_text[m_at] == c) {
            ++m_at;
            return true;
        }
        return false;
    }

    [[nodiscard]] std::string parseString() {
        std::string out;
        if (!consume('"')) return out;
        while (m_at < m_text.size()) {
            const char c = m_text[m_at++];
            if (c == '"') break;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (m_at >= m_text.size()) break;
            const char esc = m_text[m_at++];
            switch (esc) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    unsigned cp = 0;
                    if (m_at + 4 <= m_text.size()) {
                        for (int i = 0; i < 4; ++i) {
                            const char h = m_text[m_at + static_cast<std::size_t>(i)];
                            cp <<= 4;
                            if (h >= '0' && h <= '9') cp |= static_cast<unsigned>(h - '0');
                            else if (h >= 'a' && h <= 'f') cp |= static_cast<unsigned>(h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') cp |= static_cast<unsigned>(h - 'A' + 10);
                        }
                        m_at += 4;
                    }
                    appendUtf8(out, cp);
                    break;
                }
                default: out += esc; break;
            }
        }
        return out;
    }

    Value value(int depth) {
        Value v;
        if (depth > kMaxDepth) return v;
        skipSpace();
        if (m_at >= m_text.size()) return v;
        const char c = m_text[m_at];
        if (c == '{') {
            ++m_at;
            v.m_type = Value::Type::Object;
            skipSpace();
            if (consume('}')) return v;
            while (m_at < m_text.size()) {
                const std::string key = parseString();
                if (!consume(':')) break;
                v.m_object[key] = value(depth + 1);
                if (!consume(',')) {
                    (void)consume('}');
                    break;
                }
            }
            return v;
        }
        if (c == '[') {
            ++m_at;
            v.m_type = Value::Type::Array;
            skipSpace();
            if (consume(']')) return v;
            while (m_at < m_text.size()) {
                v.m_array.push_back(value(depth + 1));
                if (!consume(',')) {
                    (void)consume(']');
                    break;
                }
            }
            return v;
        }
        if (c == '"') {
            v.m_type = Value::Type::String;
            v.m_string = parseString();
            return v;
        }
        if (c == 't' && m_text.substr(m_at, 4) == "true") {
            m_at += 4;
            v.m_type = Value::Type::Bool;
            v.m_bool = true;
            return v;
        }
        if (c == 'f' && m_text.substr(m_at, 5) == "false") {
            m_at += 5;
            v.m_type = Value::Type::Bool;
            v.m_bool = false;
            return v;
        }
        if (c == 'n' && m_text.substr(m_at, 4) == "null") {
            m_at += 4;
            return v;
        }
        // Number: take everything that could belong to one and let strtod decide.
        const std::size_t start = m_at;
        while (m_at < m_text.size()) {
            const char n = m_text[m_at];
            if ((n >= '0' && n <= '9') || n == '-' || n == '+' || n == '.' || n == 'e' || n == 'E') {
                ++m_at;
            } else {
                break;
            }
        }
        if (m_at > start) {
            const std::string number(m_text.substr(start, m_at - start));
            char* end = nullptr;
            const double d = std::strtod(number.c_str(), &end);
            if (end && *end == '\0') {
                v.m_type = Value::Type::Number;
                v.m_number = d;
            }
        }
        return v;
    }

    std::string_view m_text;
    std::size_t m_at = 0;
};

Value Value::parse(std::string_view text) { return Parser(text).document(); }

const std::string& Value::string() const { return m_type == Type::String ? m_string : emptyString(); }
const std::vector<Value>& Value::array() const { return m_type == Type::Array ? m_array : emptyArray(); }
const std::map<std::string, Value, std::less<>>& Value::object() const {
    return m_type == Type::Object ? m_object : emptyObject();
}
const Value& Value::operator[](std::string_view key) const {
    if (m_type != Type::Object) return nullValue();
    const auto it = m_object.find(key);
    return it == m_object.end() ? nullValue() : it->second;
}

}  // namespace forma::store::json
