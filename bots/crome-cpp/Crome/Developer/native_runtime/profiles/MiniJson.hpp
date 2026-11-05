#pragma once
#include <cctype>
#include <cstddef>
#include <cstdlib>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace crome::json {

struct Value;
using Object = std::map<std::string, Value>;
using Array = std::vector<Value>;

struct Value {
    using Data = std::variant<std::nullptr_t, bool, double, std::string, Object, Array>;
    Data data{nullptr};

    bool isObject() const { return std::holds_alternative<Object>(data); }
    bool isArray() const { return std::holds_alternative<Array>(data); }
    bool isString() const { return std::holds_alternative<std::string>(data); }
    bool isNumber() const { return std::holds_alternative<double>(data); }
    bool isBool() const { return std::holds_alternative<bool>(data); }
    bool isNull() const { return std::holds_alternative<std::nullptr_t>(data); }

    const Object& object() const { return std::get<Object>(data); }
    const Array& array() const { return std::get<Array>(data); }
    const std::string& string() const { return std::get<std::string>(data); }
    double number() const { return std::get<double>(data); }
    bool boolean() const { return std::get<bool>(data); }

    const Value* get(std::string_view key) const {
        if (!isObject()) return nullptr;
        auto it = object().find(std::string(key));
        return it == object().end() ? nullptr : &it->second;
    }
};

class Parser {
public:
    explicit Parser(std::string_view text) : text_(text) {}

    Value parse() {
        skipWs();
        Value v = parseValue();
        skipWs();
        if (pos_ != text_.size()) fail("trailing characters");
        return v;
    }

private:
    Value parseValue() {
        skipWs();
        if (pos_ >= text_.size()) fail("unexpected end");
        const char c = text_[pos_];
        if (c == '{') return Value{parseObject()};
        if (c == '[') return Value{parseArray()};
        if (c == '"') return Value{parseString()};
        if (c == 't') { consume("true"); return Value{true}; }
        if (c == 'f') { consume("false"); return Value{false}; }
        if (c == 'n') { consume("null"); return Value{nullptr}; }
        if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) return Value{parseNumber()};
        fail("invalid value");
        return {};
    }

    Object parseObject() {
        expect('{');
        Object out;
        skipWs();
        if (peek('}')) { ++pos_; return out; }
        while (true) {
            skipWs();
            if (!peek('"')) fail("object key must be a string");
            std::string key = parseString();
            skipWs(); expect(':');
            out.emplace(std::move(key), parseValue());
            skipWs();
            if (peek('}')) { ++pos_; break; }
            expect(',');
        }
        return out;
    }

    Array parseArray() {
        expect('[');
        Array out;
        skipWs();
        if (peek(']')) { ++pos_; return out; }
        while (true) {
            out.push_back(parseValue());
            skipWs();
            if (peek(']')) { ++pos_; break; }
            expect(',');
        }
        return out;
    }

    std::string parseString() {
        expect('"');
        std::string out;
        while (pos_ < text_.size()) {
            char c = text_[pos_++];
            if (c == '"') return out;
            if (c == '\\') {
                if (pos_ >= text_.size()) fail("bad escape");
                char e = text_[pos_++];
                switch (e) {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    default: fail("unsupported escape");
                }
            } else {
                out.push_back(c);
            }
        }
        fail("unterminated string");
        return {};
    }

    double parseNumber() {
        const std::size_t start = pos_;
        if (peek('-')) ++pos_;
        while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) ++pos_;
        if (peek('.')) {
            ++pos_;
            while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) ++pos_;
        }
        if (peek('e') || peek('E')) {
            ++pos_;
            if (peek('+') || peek('-')) ++pos_;
            while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) ++pos_;
        }
        std::string tmp(text_.substr(start, pos_ - start));
        char* end = nullptr;
        const double v = std::strtod(tmp.c_str(), &end);
        if (!end || *end != '\0') fail("invalid number");
        return v;
    }

    void consume(std::string_view word) {
        if (text_.substr(pos_, word.size()) != word) fail("invalid literal");
        pos_ += word.size();
    }

    void skipWs() {
        while (pos_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[pos_]))) ++pos_;
    }

    bool peek(char c) const { return pos_ < text_.size() && text_[pos_] == c; }

    void expect(char c) {
        skipWs();
        if (!peek(c)) fail(std::string("expected '") + c + "'");
        ++pos_;
    }

    [[noreturn]] void fail(const std::string& msg) const {
        throw std::runtime_error("JSON parse error at byte " + std::to_string(pos_) + ": " + msg);
    }

    std::string_view text_;
    std::size_t pos_{};
};

inline Value parse(std::string_view text) { return Parser(text).parse(); }

} // namespace crome::json
