#ifndef SIMPLE_JSON_H
#define SIMPLE_JSON_H

#include <string>
#include <map>
#include <vector>
#include <variant>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <cmath>
#include <algorithm>

namespace simple_json {

class Json;
using JsonNull = std::monostate;
using JsonBool = bool;
using JsonInt = int;
using JsonFloat = double;
using JsonString = std::string;
using JsonObject = std::map<std::string, Json>;
using JsonArray = std::vector<Json>;

class Json {
    enum class Type { Null, Bool, Int, Float, String, Object, Array };
    std::variant<JsonNull, JsonBool, JsonInt, JsonFloat, JsonString, JsonObject, JsonArray> m_data;
    Type m_type;

    static std::string trim(const std::string& s) {
        auto start = s.find_first_not_of(" \t\n\r");
        auto end = s.find_last_not_of(" \t\n\r");
        return (start == std::string::npos) ? "" : s.substr(start, end - start + 1);
    }

    static std::string unescape(const std::string& s) {
        std::string result;
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '\\' && i + 1 < s.size()) {
                switch (s[i + 1]) {
                    case '"': result += '"'; ++i; break;
                    case '\\': result += '\\'; ++i; break;
                    case '/': result += '/'; ++i; break;
                    case 'n': result += '\n'; ++i; break;
                    case 't': result += '\t'; ++i; break;
                    case 'r': result += '\r'; ++i; break;
                    default: result += s[i]; break;
                }
            } else {
                result += s[i];
            }
        }
        return result;
    }

    static std::string escape(const std::string& s) {
        std::string result;
        for (char c : s) {
            switch (c) {
                case '"': result += "\\\""; break;
                case '\\': result += "\\\\"; break;
                case '\n': result += "\\n"; break;
                case '\t': result += "\\t"; break;
                case '\r': result += "\\r"; break;
                default: result += c; break;
            }
        }
        return result;
    }

    JsonObject& getObj() { return std::get<JsonObject>(m_data); }
    const JsonObject& getObj() const { return std::get<JsonObject>(m_data); }
    JsonArray& getArr() { return std::get<JsonArray>(m_data); }
    const JsonArray& getArr() const { return std::get<JsonArray>(m_data); }

    void ensureObject() {
        if (m_type != Type::Object) {
            m_data = JsonObject{};
            m_type = Type::Object;
        }
    }
    void ensureArray() {
        if (m_type != Type::Array) {
            m_data = JsonArray{};
            m_type = Type::Array;
        }
    }

public:
    Json() : m_data(JsonNull{}), m_type(Type::Null) {}
    
    Json(bool v) : m_data(v), m_type(Type::Bool) {}
    Json(int v) : m_data(v), m_type(Type::Int) {}
    Json(double v) : m_data(v), m_type(Type::Float) {}
    Json(float v) : m_data((double)v), m_type(Type::Float) {}
    Json(const std::string& v) : m_data(v), m_type(Type::String) {}
    Json(const char* v) : m_data(std::string(v)), m_type(Type::String) {}
    Json(const JsonObject& v) : m_data(v), m_type(Type::Object) {}
    Json(const JsonArray& v) : m_data(v), m_type(Type::Array) {}

    static Json object() { return Json(JsonObject{}); }
    static Json array() { return Json(JsonArray{}); }

    static Json parse(const std::string& s) {
        size_t pos = 0;
        return parseAt(s, pos);
    }

    static void skipWhitespace(const std::string& s, size_t& pos) {
        while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t' || s[pos] == '\n' || s[pos] == '\r'))
            ++pos;
    }

    static Json parseAt(const std::string& s, size_t& pos) {
        skipWhitespace(s, pos);
        if (pos >= s.size()) return Json();
        char c = s[pos];
        if (c == '{') return parseObject(s, pos);
        if (c == '[') return parseArray(s, pos);
        if (c == '"') return Json(parseString(s, pos));
        if (c == 't' || c == 'f') return Json(parseBoolVal(s, pos));
        if (c == 'n') return parseNullVal(s, pos);
        return parseNumber(s, pos);
    }

    static Json parseObject(const std::string& s, size_t& pos) {
        JsonObject obj;
        ++pos;
        if (pos < s.size() && s[pos] == '}') { ++pos; return Json(obj); }
        while (pos < s.size()) {
            skipWhitespace(s, pos);
            auto key = parseString(s, pos);
            skipWhitespace(s, pos);
            if (pos >= s.size() || s[pos] != ':') throw std::runtime_error("Expected ':' in object");
            ++pos;
            auto val = parseAt(s, pos);
            obj[key] = val;
            skipWhitespace(s, pos);
            if (pos < s.size() && s[pos] == ',') { ++pos; continue; }
            if (pos < s.size() && s[pos] == '}') { ++pos; break; }
        }
        return Json(obj);
    }

    static Json parseArray(const std::string& s, size_t& pos) {
        JsonArray arr;
        ++pos;
        if (pos < s.size() && s[pos] == ']') { ++pos; return Json(arr); }
        while (pos < s.size()) {
            auto val = parseAt(s, pos);
            arr.push_back(val);
            skipWhitespace(s, pos);
            if (pos < s.size() && s[pos] == ',') { ++pos; continue; }
            if (pos < s.size() && s[pos] == ']') { ++pos; break; }
        }
        return Json(arr);
    }

    static std::string parseString(const std::string& s, size_t& pos) {
        ++pos;
        std::string result;
        while (pos < s.size() && s[pos] != '"') {
            if (s[pos] == '\\' && pos + 1 < s.size()) {
                ++pos;
                switch (s[pos]) {
                    case '"': result += '"'; break;
                    case '\\': result += '\\'; break;
                    case '/': result += '/'; break;
                    case 'n': result += '\n'; break;
                    case 't': result += '\t'; break;
                    case 'r': result += '\r'; break;
                    default: result += s[pos]; break;
                }
            } else {
                result += s[pos];
            }
            ++pos;
        }
        if (pos < s.size()) ++pos;
        return result;
    }

    static bool parseBoolVal(const std::string& s, size_t& pos) {
        if (s.substr(pos, 4) == "true") { pos += 4; return true; }
        if (s.substr(pos, 5) == "false") { pos += 5; return false; }
        throw std::runtime_error("Invalid boolean");
    }

    static Json parseNullVal(const std::string& s, size_t& pos) {
        pos += 4;
        return Json();
    }

    static Json parseNumber(const std::string& s, size_t& pos) {
        size_t start = pos;
        bool isFloat = false;
        if (pos < s.size() && s[pos] == '-') ++pos;
        while (pos < s.size() && std::isdigit(s[pos])) ++pos;
        if (pos < s.size() && s[pos] == '.') { isFloat = true; ++pos; }
        while (pos < s.size() && std::isdigit(s[pos])) ++pos;
        if (pos < s.size() && (s[pos] == 'e' || s[pos] == 'E')) { isFloat = true; ++pos; }
        if (pos < s.size() && (s[pos] == '+' || s[pos] == '-')) ++pos;
        while (pos < s.size() && std::isdigit(s[pos])) ++pos;
        std::string numStr = s.substr(start, pos - start);
        if (isFloat) return Json(std::stod(numStr));
        return Json(std::stoi(numStr));
    }

    // Type queries
    Type type() const { return m_type; }
    bool isObject() const { return m_type == Type::Object; }
    bool isArray() const { return m_type == Type::Array; }
    bool isString() const { return m_type == Type::String; }
    bool isNumber() const { return m_type == Type::Int || m_type == Type::Float; }
    bool isBool() const { return m_type == Type::Bool; }
    bool isNull() const { return m_type == Type::Null; }
    bool contains(const std::string& key) const {
        if (m_type != Type::Object) return false;
        return getObj().count(key) > 0;
    }

    // Value access with defaults
    std::string value(const std::string& key, const std::string& def) const {
        if (m_type == Type::Object) {
            auto it = getObj().find(key);
            if (it != getObj().end() && it->second.isString()) return it->second.getStr();
        }
        return def;
    }
    std::string value(const std::string& key, const char* def) const {
        if (m_type == Type::Object) {
            auto it = getObj().find(key);
            if (it != getObj().end() && it->second.isString()) return it->second.getStr();
        }
        return std::string(def);
    }
    int value(const std::string& key, int def) const {
        if (m_type == Type::Object) {
            auto it = getObj().find(key);
            if (it != getObj().end()) {
                if (it->second.m_type == Type::Int) return std::get<JsonInt>(it->second.m_data);
                if (it->second.m_type == Type::Float) return (int)std::get<JsonFloat>(it->second.m_data);
            }
        }
        return def;
    }
    double value(const std::string& key, double def) const {
        if (m_type == Type::Object) {
            auto it = getObj().find(key);
            if (it != getObj().end()) {
                if (it->second.m_type == Type::Float) return std::get<JsonFloat>(it->second.m_data);
                if (it->second.m_type == Type::Int) return (double)std::get<JsonInt>(it->second.m_data);
            }
        }
        return def;
    }
    bool value(const std::string& key, bool def) const {
        if (m_type == Type::Object) {
            auto it = getObj().find(key);
            if (it != getObj().end() && it->second.m_type == Type::Bool) return std::get<JsonBool>(it->second.m_data);
        }
        return def;
    }
    float value(const std::string& key, float def) const {
        return (float)value(key, (double)def);
    }

    // Non-const operator[] for setting values (takes const char* to avoid ambiguity)
    Json& operator[](const char* key) {
        ensureObject();
        return getObj()[std::string(key)];
    }
    Json& operator[](const std::string& key) {
        ensureObject();
        return getObj()[key];
    }
    Json& operator[](size_t idx) {
        ensureArray();
        while (getArr().size() <= idx) getArr().push_back(Json());
        return getArr()[idx];
    }

    // Const accessor via at()
    const Json& at(const std::string& key) const {
        static Json nullJson;
        if (m_type == Type::Object) {
            auto it = getObj().find(key);
            if (it != getObj().end()) return it->second;
        }
        return nullJson;
    }
    const Json& at(size_t idx) const {
        static Json nullJson;
        if (m_type == Type::Array && idx < getArr().size()) return getArr()[idx];
        return nullJson;
    }

    void push_back(const Json& val) {
        ensureArray();
        getArr().push_back(val);
    }

    // Get raw types
    std::string getStr() const {
        if (m_type == Type::String) return std::get<JsonString>(m_data);
        return "";
    }
    int getInt() const {
        if (m_type == Type::Int) return std::get<JsonInt>(m_data);
        if (m_type == Type::Float) return (int)std::get<JsonFloat>(m_data);
        return 0;
    }
    double getFloat() const {
        if (m_type == Type::Float) return std::get<JsonFloat>(m_data);
        if (m_type == Type::Int) return (double)std::get<JsonInt>(m_data);
        return 0.0;
    }
    bool getBool() const {
        if (m_type == Type::Bool) return std::get<JsonBool>(m_data);
        return false;
    }

    // Size
    size_t size() const {
        if (m_type == Type::Object) return getObj().size();
        if (m_type == Type::Array) return getArr().size();
        return 0;
    }

    // begin/end for range-for on arrays
    auto begin() const { return getArr().cbegin(); }
    auto end() const { return getArr().cend(); }

    // Iteration via items() for objects
    class ItemsView {
        const JsonObject& obj;
    public:
        ItemsView(const JsonObject& o) : obj(o) {}
        auto begin() const { return obj.cbegin(); }
        auto end() const { return obj.cend(); }
    };
    ItemsView items() const {
        return ItemsView(getObj());
    }

    // Array iteration
    class ArrayView {
        const JsonArray& arr;
    public:
        ArrayView(const JsonArray& a) : arr(a) {}
        auto begin() const { return arr.cbegin(); }
        auto end() const { return arr.cend(); }
    };
    ArrayView arrItems() const {
        return ArrayView(getArr());
    }

    // Serialization
    std::string dump(int indent = -1) const {
        std::ostringstream oss;
        dumpTo(oss, indent, 0);
        return oss.str();
    }

    void dumpTo(std::ostringstream& oss, int baseIndent, int currentIndent) const {
        int ind = (baseIndent >= 0) ? currentIndent : 0;
        std::string pad = (baseIndent >= 0) ? std::string(ind * baseIndent, ' ') : "";
        std::string newline = (baseIndent >= 0) ? "\n" : "";

        switch (m_type) {
            case Type::Null: oss << "null"; break;
            case Type::Bool: oss << (std::get<JsonBool>(m_data) ? "true" : "false"); break;
            case Type::Int: oss << std::get<JsonInt>(m_data); break;
            case Type::Float: {
                double v = std::get<JsonFloat>(m_data);
                if (v == (int)v && std::abs(v) < 1e15) oss << (int)v;
                else oss << v;
                break;
            }
            case Type::String: oss << "\"" << escape(std::get<JsonString>(m_data)) << "\""; break;
            case Type::Object: {
                auto& obj = getObj();
                if (obj.empty()) { oss << "{}"; break; }
                oss << "{" << newline;
                bool first = true;
                for (auto& [k, v] : obj) {
                    if (!first) oss << "," << newline;
                    first = false;
                    oss << pad << "  " << "\"" << escape(k) << "\": ";
                    v.dumpTo(oss, baseIndent, ind + 1);
                }
                oss << newline << pad << "}";
                break;
            }
            case Type::Array: {
                auto& arr = getArr();
                if (arr.empty()) { oss << "[]"; break; }
                oss << "[" << newline;
                bool first = true;
                for (auto& item : arr) {
                    if (!first) oss << "," << newline;
                    first = false;
                    oss << pad << "  ";
                    item.dumpTo(oss, baseIndent, ind + 1);
                }
                oss << newline << pad << "]";
                break;
            }
        }
    }

    // Stream operators
    friend std::istream& operator>>(std::istream& is, Json& j) {
        std::stringstream ss;
        ss << is.rdbuf();
        j = parse(ss.str());
        return is;
    }
    friend std::ostream& operator<<(std::ostream& os, const Json& j) {
        os << j.dump();
        return os;
    }

    // Implicit conversions
    operator std::string() const { return getStr(); }
    operator int() const { return getInt(); }
    operator bool() const { return getBool(); }
    operator double() const { return getFloat(); }
};

} // namespace simple_json

using json = simple_json::Json;

#endif // SIMPLE_JSON_H
