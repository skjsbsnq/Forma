// Small JSON reader for Flatpak's --json output. Lenient: invalid input yields null values, never throws.
#pragma once

#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace forma::store::json {

class Value {
public:
    enum class Type : unsigned char { Null, Bool, Number, String, Array, Object };

    [[nodiscard]] Type type() const { return m_type; }
    [[nodiscard]] bool isNull() const { return m_type == Type::Null; }
    [[nodiscard]] bool isArray() const { return m_type == Type::Array; }
    [[nodiscard]] bool isObject() const { return m_type == Type::Object; }
    [[nodiscard]] bool boolean(bool fallback = false) const { return m_type == Type::Bool ? m_bool : fallback; }
    [[nodiscard]] double number(double fallback = 0) const { return m_type == Type::Number ? m_number : fallback; }
    [[nodiscard]] const std::string& string() const;
    [[nodiscard]] const std::vector<Value>& array() const;
    [[nodiscard]] const std::map<std::string, Value, std::less<>>& object() const;
    [[nodiscard]] const Value& operator[](std::string_view key) const;
    [[nodiscard]] std::string str(std::string_view fallback = {}) const {
        return m_type == Type::String ? m_string : std::string(fallback);
    }

    static Value parse(std::string_view text);

private:
    friend class Parser;
    Type m_type = Type::Null;
    bool m_bool = false;
    double m_number = 0;
    std::string m_string;
    std::vector<Value> m_array;
    std::map<std::string, Value, std::less<>> m_object;
};

}  // namespace forma::store::json
