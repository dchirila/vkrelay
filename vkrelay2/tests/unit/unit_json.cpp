#include "common/util/json.hpp"
#include "tests/test_assert.hpp"

#include <string>
#include <vector>

using vkr::json::Value;

namespace {

// dump(parse(x)) round-trips arbitrary byte strings carried as JSON strings.
void test_string_round_trip() {
    const std::vector<std::string> nasty = {
        "",
        "simple",
        "a b c",
        "\"double\"",
        "'single'",
        "back\\slash",
        "trailing\\",
        "path\\to\\file",
        "quote\\\"mix",
        "$PATH",
        "%PATH%",
        "bang! caret^ amp& (paren)",
        "tab\tnewline\nreturn\r",
        "caf\xC3\xA9",              // UTF-8 'café'
        "\xE6\x97\xA5\xE6\x9C\xAC", // UTF-8 '日本'
        "\xF0\x9F\x98\x80",         // UTF-8 emoji U+1F600
    };

    Value arr = Value::make_array();
    for (const auto& s : nasty) {
        arr.as_array().emplace_back(s);
    }
    Value root = Value::make_object();
    root.set("argv", std::move(arr));

    const std::string text = root.dump(0);
    Value parsed = Value::parse(text);

    const auto& out = parsed.find("argv")->as_array();
    VKR_CHECK_EQ(out.size(), nasty.size());
    for (std::size_t i = 0; i < nasty.size() && i < out.size(); ++i) {
        VKR_CHECK_EQ(out[i].as_string(), nasty[i]);
    }

    // Pretty form must parse back to the same data.
    Value parsed_pretty = Value::parse(root.dump(2));
    VKR_CHECK_EQ(parsed_pretty.find("argv")->as_array().size(), nasty.size());
}

void test_unicode_escapes() {
    VKR_CHECK_EQ(Value::parse("\"\\u0041\\u00e9\"").as_string(), std::string("A\xC3\xA9"));
    // Surrogate pair -> U+1F600 (9F 98 80).
    VKR_CHECK_EQ(Value::parse("\"\\ud83d\\ude00\"").as_string(), std::string("\xF0\x9F\x98\x80"));
    VKR_CHECK_EQ(Value::parse("\"tab\\tlf\\n\"").as_string(), std::string("tab\tlf\n"));
}

void test_scalars() {
    VKR_CHECK_EQ(Value::parse("123").as_int(), static_cast<long long>(123));
    VKR_CHECK_EQ(Value::parse("-7").as_int(), static_cast<long long>(-7));
    VKR_CHECK(Value::parse("3.5").as_number() == 3.5);
    VKR_CHECK_EQ(Value::parse("true").as_bool(), true);
    VKR_CHECK_EQ(Value::parse("false").as_bool(), false);
    VKR_CHECK(Value::parse("null").is_null());
    // Integers serialize without a decimal point.
    VKR_CHECK_EQ(Value(42).dump(0), std::string("42"));
}

void test_integer_classification() {
    // Whole-valued numbers are integers; fractional ones are not (so version
    // 1.4 is never silently rounded to 1).
    VKR_CHECK(Value::parse("1").is_integer());
    VKR_CHECK(Value::parse("-7").is_integer());
    VKR_CHECK(Value::parse("2.0").is_integer());
    VKR_CHECK(!Value::parse("1.4").is_integer());
    VKR_CHECK(!Value::parse("3.5").is_integer());
    VKR_CHECK(Value(42).is_integer());
    VKR_CHECK(Value(static_cast<long long>(99)).is_integer());
    VKR_CHECK(!Value(3.5).is_integer());
    // Non-numbers are never integers.
    VKR_CHECK(!Value::parse("\"7\"").is_integer());
    VKR_CHECK(!Value::parse("true").is_integer());
    VKR_CHECK(!Value::parse("null").is_integer());
}

void test_errors() {
    Value out;
    std::string err;
    VKR_CHECK(!Value::try_parse("{bad", out, err));
    VKR_CHECK(!Value::try_parse("", out, err));
    VKR_CHECK(!Value::try_parse("[1,2", out, err));
    VKR_CHECK(Value::try_parse("{\"a\":1}", out, err));
}

} // namespace

int main() {
    test_string_round_trip();
    test_unicode_escapes();
    test_scalars();
    test_integer_classification();
    test_errors();
    return vkr::test::finish("unit_json");
}
