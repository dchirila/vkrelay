#include "common/util/json.hpp"

#include <array>
#include <cmath>
#include <cstdio>

namespace vkr::json {

namespace {

void encode_utf8(std::uint32_t cp, std::string& out) {
    if (cp <= 0x7F) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

class Parser {
  public:
    explicit Parser(const std::string& text) : s_(text) {}

    Value parse_document() {
        skip_ws();
        Value v = parse_value();
        skip_ws();
        if (pos_ != s_.size()) {
            fail("trailing characters after JSON value");
        }
        return v;
    }

  private:
    const std::string& s_;
    std::size_t pos_ = 0;

    [[noreturn]] void fail(const std::string& msg) const {
        throw ParseError("json parse error at offset " + std::to_string(pos_) + ": " + msg);
    }

    char peek() const { return pos_ < s_.size() ? s_[pos_] : '\0'; }

    char next() {
        if (pos_ >= s_.size()) {
            fail("unexpected end of input");
        }
        return s_[pos_++];
    }

    void skip_ws() {
        while (pos_ < s_.size()) {
            char c = s_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++pos_;
            } else {
                break;
            }
        }
    }

    void expect(char c) {
        if (next() != c) {
            fail(std::string("expected '") + c + "'");
        }
    }

    Value parse_value() {
        skip_ws();
        char c = peek();
        switch (c) {
        case '{':
            return parse_object();
        case '[':
            return parse_array();
        case '"':
            return Value(parse_string());
        case 't':
        case 'f':
            return parse_bool();
        case 'n':
            return parse_null();
        default:
            if (c == '-' || (c >= '0' && c <= '9')) {
                return parse_number();
            }
            fail("unexpected character");
        }
    }

    Value parse_object() {
        expect('{');
        Object obj;
        skip_ws();
        if (peek() == '}') {
            ++pos_;
            return Value(std::move(obj));
        }
        while (true) {
            skip_ws();
            if (peek() != '"') {
                fail("expected object key string");
            }
            std::string key = parse_string();
            skip_ws();
            expect(':');
            Value v = parse_value();
            obj.emplace_back(std::move(key), std::move(v));
            skip_ws();
            char c = next();
            if (c == ',') {
                continue;
            }
            if (c == '}') {
                break;
            }
            fail("expected ',' or '}' in object");
        }
        return Value(std::move(obj));
    }

    Value parse_array() {
        expect('[');
        Array arr;
        skip_ws();
        if (peek() == ']') {
            ++pos_;
            return Value(std::move(arr));
        }
        while (true) {
            arr.push_back(parse_value());
            skip_ws();
            char c = next();
            if (c == ',') {
                continue;
            }
            if (c == ']') {
                break;
            }
            fail("expected ',' or ']' in array");
        }
        return Value(std::move(arr));
    }

    std::uint32_t parse_hex4() {
        std::uint32_t value = 0;
        for (int i = 0; i < 4; ++i) {
            char c = next();
            value <<= 4;
            if (c >= '0' && c <= '9') {
                value |= static_cast<std::uint32_t>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                value |= static_cast<std::uint32_t>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                value |= static_cast<std::uint32_t>(c - 'A' + 10);
            } else {
                fail("invalid \\u hex digit");
            }
        }
        return value;
    }

    std::string parse_string() {
        expect('"');
        std::string out;
        while (true) {
            char c = next();
            if (c == '"') {
                break;
            }
            if (c == '\\') {
                char e = next();
                switch (e) {
                case '"':
                    out.push_back('"');
                    break;
                case '\\':
                    out.push_back('\\');
                    break;
                case '/':
                    out.push_back('/');
                    break;
                case 'b':
                    out.push_back('\b');
                    break;
                case 'f':
                    out.push_back('\f');
                    break;
                case 'n':
                    out.push_back('\n');
                    break;
                case 'r':
                    out.push_back('\r');
                    break;
                case 't':
                    out.push_back('\t');
                    break;
                case 'u': {
                    std::uint32_t cp = parse_hex4();
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        // High surrogate; expect a following low surrogate.
                        if (next() != '\\' || next() != 'u') {
                            fail("expected low surrogate escape");
                        }
                        std::uint32_t low = parse_hex4();
                        if (low < 0xDC00 || low > 0xDFFF) {
                            fail("invalid low surrogate");
                        }
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                    }
                    encode_utf8(cp, out);
                    break;
                }
                default:
                    fail("invalid escape sequence");
                }
            } else {
                out.push_back(c);
            }
        }
        return out;
    }

    Value parse_bool() {
        if (s_.compare(pos_, 4, "true") == 0) {
            pos_ += 4;
            return Value(true);
        }
        if (s_.compare(pos_, 5, "false") == 0) {
            pos_ += 5;
            return Value(false);
        }
        fail("invalid literal");
    }

    Value parse_null() {
        if (s_.compare(pos_, 4, "null") == 0) {
            pos_ += 4;
            return Value(nullptr);
        }
        fail("invalid literal");
    }

    Value parse_number() {
        std::size_t start = pos_;
        if (peek() == '-') {
            ++pos_;
        }
        while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') {
            ++pos_;
        }
        if (peek() == '.') {
            ++pos_;
            while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') {
                ++pos_;
            }
        }
        if (peek() == 'e' || peek() == 'E') {
            ++pos_;
            if (peek() == '+' || peek() == '-') {
                ++pos_;
            }
            while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') {
                ++pos_;
            }
        }
        std::string token = s_.substr(start, pos_ - start);
        if (token.empty() || token == "-") {
            fail("invalid number");
        }
        return Value(std::stod(token));
    }
};

} // namespace

Value::Value(double n) : type_(Type::Number), num_(n) {
    integral_ = std::isfinite(n) && n == std::floor(n);
}

bool Value::as_bool() const {
    if (type_ != Type::Bool) {
        throw ParseError("value is not a bool");
    }
    return bool_;
}

double Value::as_number() const {
    if (type_ != Type::Number) {
        throw ParseError("value is not a number");
    }
    return num_;
}

std::int64_t Value::as_int() const {
    return static_cast<std::int64_t>(std::llround(as_number()));
}

const std::string& Value::as_string() const {
    if (type_ != Type::String) {
        throw ParseError("value is not a string");
    }
    return str_;
}

const Array& Value::as_array() const {
    if (type_ != Type::Array) {
        throw ParseError("value is not an array");
    }
    return arr_;
}

const Object& Value::as_object() const {
    if (type_ != Type::Object) {
        throw ParseError("value is not an object");
    }
    return obj_;
}

Array& Value::as_array() {
    if (type_ != Type::Array) {
        throw ParseError("value is not an array");
    }
    return arr_;
}

Object& Value::as_object() {
    if (type_ != Type::Object) {
        throw ParseError("value is not an object");
    }
    return obj_;
}

const Value* Value::find(const std::string& key) const {
    if (type_ != Type::Object) {
        return nullptr;
    }
    for (const auto& kv : obj_) {
        if (kv.first == key) {
            return &kv.second;
        }
    }
    return nullptr;
}

Value& Value::set(const std::string& key, Value v) {
    if (type_ != Type::Object) {
        *this = Value(Object{});
    }
    for (auto& kv : obj_) {
        if (kv.first == key) {
            kv.second = std::move(v);
            return kv.second;
        }
    }
    obj_.emplace_back(key, std::move(v));
    return obj_.back().second;
}

std::string escape_string(const std::string& s) {
    std::string out;
    out.push_back('"');
    for (unsigned char c : s) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (c < 0x20) {
                std::array<char, 8> buf{};
                std::snprintf(buf.data(), buf.size(), "\\u%04x", c);
                out += buf.data();
            } else {
                // Pass through UTF-8 (and all other bytes >= 0x20) verbatim.
                out.push_back(static_cast<char>(c));
            }
        }
    }
    out.push_back('"');
    return out;
}

void Value::dump_to(std::string& out, int indent, int depth) const {
    const bool pretty = indent > 0;
    const std::string pad =
        pretty ? std::string(static_cast<std::size_t>(indent * (depth + 1)), ' ') : std::string();
    const std::string pad_close =
        pretty ? std::string(static_cast<std::size_t>(indent * depth), ' ') : std::string();
    const char* nl = pretty ? "\n" : "";

    switch (type_) {
    case Type::Null:
        out += "null";
        break;
    case Type::Bool:
        out += bool_ ? "true" : "false";
        break;
    case Type::Number: {
        if (num_ == std::floor(num_) && std::abs(num_) < 1e15) {
            out += std::to_string(static_cast<std::int64_t>(num_));
        } else {
            std::array<char, 32> buf{};
            std::snprintf(buf.data(), buf.size(), "%.17g", num_);
            out += buf.data();
        }
        break;
    }
    case Type::String:
        out += escape_string(str_);
        break;
    case Type::Array:
        if (arr_.empty()) {
            out += "[]";
            break;
        }
        out += '[';
        out += nl;
        for (std::size_t i = 0; i < arr_.size(); ++i) {
            out += pad;
            arr_[i].dump_to(out, indent, depth + 1);
            if (i + 1 < arr_.size()) {
                out += ',';
            }
            out += nl;
        }
        out += pad_close;
        out += ']';
        break;
    case Type::Object:
        if (obj_.empty()) {
            out += "{}";
            break;
        }
        out += '{';
        out += nl;
        for (std::size_t i = 0; i < obj_.size(); ++i) {
            out += pad;
            out += escape_string(obj_[i].first);
            out += pretty ? ": " : ":";
            obj_[i].second.dump_to(out, indent, depth + 1);
            if (i + 1 < obj_.size()) {
                out += ',';
            }
            out += nl;
        }
        out += pad_close;
        out += '}';
        break;
    }
}

std::string Value::dump(int indent) const {
    std::string out;
    dump_to(out, indent, 0);
    return out;
}

Value Value::parse(const std::string& text) {
    Parser parser(text);
    return parser.parse_document();
}

bool Value::try_parse(const std::string& text, Value& out, std::string& error) {
    try {
        out = parse(text);
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

} // namespace vkr::json
