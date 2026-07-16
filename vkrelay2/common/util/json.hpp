// Minimal, dependency-free JSON value, parser and serializer.
//
// Scope: enough to carry vkrelay2 launch descriptors and test fixtures
// Objects preserve insertion order so serialized
// output is deterministic. Strings are UTF-8; the parser decodes \uXXXX
// (including surrogate pairs) into UTF-8, and the serializer emits raw
// UTF-8 bytes (escaping only what JSON requires), so any byte string
// round-trips through dump() -> parse().
#ifndef VKRELAY2_COMMON_UTIL_JSON_HPP
#define VKRELAY2_COMMON_UTIL_JSON_HPP

#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace vkr::json {

class Value;
using Array = std::vector<Value>;
using Object = std::vector<std::pair<std::string, Value>>;

struct ParseError : std::runtime_error {
    explicit ParseError(const std::string& msg) : std::runtime_error(msg) {}
};

enum class Type { Null, Bool, Number, String, Array, Object };

class Value {
  public:
    Value() : type_(Type::Null) {}
    Value(std::nullptr_t) : type_(Type::Null) {}
    Value(bool b) : type_(Type::Bool), bool_(b) {}
    Value(double n);
    Value(int n) : type_(Type::Number), num_(static_cast<double>(n)), integral_(true) {}
    // long long (not int64_t): on LP64 int64_t is `long`, which would make
    // `Value(someLongLong)` ambiguous. long long is an exact match on MSVC and
    // GCC alike.
    Value(long long n) : type_(Type::Number), num_(static_cast<double>(n)), integral_(true) {}
    Value(const char* s) : type_(Type::String), str_(s) {}
    Value(std::string s) : type_(Type::String), str_(std::move(s)) {}
    Value(Array a) : type_(Type::Array), arr_(std::move(a)) {}
    Value(Object o) : type_(Type::Object), obj_(std::move(o)) {}

    Type type() const { return type_; }
    bool is_null() const { return type_ == Type::Null; }
    bool is_bool() const { return type_ == Type::Bool; }
    bool is_number() const { return type_ == Type::Number; }
    bool is_string() const { return type_ == Type::String; }
    bool is_array() const { return type_ == Type::Array; }
    bool is_object() const { return type_ == Type::Object; }

    bool as_bool() const;
    double as_number() const;
    std::int64_t as_int() const;
    // True only for a Number whose value is a finite, whole number (e.g. 1 or
    // 2.0, but not 1.4). Used to reject fractional protocol/descriptor versions
    // instead of silently rounding them.
    bool is_integer() const { return type_ == Type::Number && integral_; }
    const std::string& as_string() const;
    const Array& as_array() const;
    const Object& as_object() const;
    Array& as_array();
    Object& as_object();

    // Object helpers (linear scan; descriptors are small).
    const Value* find(const std::string& key) const;
    Value& set(const std::string& key, Value v);

    static Value make_array() { return Value(Array{}); }
    static Value make_object() { return Value(Object{}); }

    std::string dump(int indent = 0) const;

    static Value parse(const std::string& text);
    static bool try_parse(const std::string& text, Value& out, std::string& error);

  private:
    void dump_to(std::string& out, int indent, int depth) const;

    Type type_;
    bool bool_ = false;
    double num_ = 0.0;
    bool integral_ = false;
    std::string str_;
    Array arr_;
    Object obj_;
};

// Escapes a UTF-8 string as a JSON string literal (including the quotes).
std::string escape_string(const std::string& s);

} // namespace vkr::json

#endif // VKRELAY2_COMMON_UTIL_JSON_HPP
