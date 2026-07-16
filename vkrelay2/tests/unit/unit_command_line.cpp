#include "common/argv/argv_split.hpp"
#include "common/argv/command_line.hpp"
#include "tests/test_assert.hpp"

#include <string>

using vkr::argv::build_command_line;
using vkr::argv::quote_argument;
using vkr::argv::split_on_double_dash;

namespace {

void test_quote_argument() {
    VKR_CHECK_EQ(quote_argument("simple"), std::string("simple"));
    VKR_CHECK_EQ(quote_argument(""), std::string("\"\""));
    VKR_CHECK_EQ(quote_argument("a b"), std::string("\"a b\""));
    VKR_CHECK_EQ(quote_argument("tab\there"), std::string("\"tab\there\""));
    // Backslashes with no whitespace/quote stay literal and unquoted.
    VKR_CHECK_EQ(quote_argument("c:\\path\\file"), std::string("c:\\path\\file"));
    // a"b  ->  "a\"b"
    VKR_CHECK_EQ(quote_argument("a\"b"), std::string("\"a\\\"b\""));
    // a\"  ->  "a\\\""  (one backslash doubled before the escaped quote)
    VKR_CHECK_EQ(quote_argument("a\\\""), std::string("\"a\\\\\\\"\""));
}

void test_build_command_line() {
    VKR_CHECK_EQ(build_command_line({"a", "b c"}), std::string("a \"b c\""));
    VKR_CHECK_EQ(build_command_line({"prog", "arg1", "with space"}),
                 std::string("prog arg1 \"with space\""));
    VKR_CHECK_EQ(build_command_line({"only"}), std::string("only"));
}

void test_split() {
    auto a = split_on_double_dash({"--gpu", "auto", "--", "app", "arg"});
    VKR_CHECK(a.has_separator);
    VKR_CHECK_EQ(a.options.size(), static_cast<std::size_t>(2));
    VKR_CHECK_EQ(a.app_argv.size(), static_cast<std::size_t>(2));
    VKR_CHECK_EQ(a.app_argv[0], std::string("app"));

    auto b = split_on_double_dash({"--list-gpus"});
    VKR_CHECK(!b.has_separator);
    VKR_CHECK_EQ(b.options.size(), static_cast<std::size_t>(1));
    VKR_CHECK(b.app_argv.empty());

    // Only the first "--" splits; later ones are preserved as app args.
    auto c = split_on_double_dash({"--", "a", "--", "b"});
    VKR_CHECK(c.has_separator);
    VKR_CHECK(c.options.empty());
    VKR_CHECK_EQ(c.app_argv.size(), static_cast<std::size_t>(3));
    VKR_CHECK_EQ(c.app_argv[1], std::string("--"));
}

} // namespace

int main() {
    test_quote_argument();
    test_build_command_line();
    test_split();
    return vkr::test::finish("unit_command_line");
}
