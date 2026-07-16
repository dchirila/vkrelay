// Quoting regression test.
//
// Spawns the argv-echo canary through the real vkr::process spawn wrapper with
// adversarial inputs and asserts byte-for-byte argv/env/cwd equality
// at the child boundary. This is the same spawn path real apps use.
#include "common/process/process.hpp"
#include "common/util/json.hpp"
#include "tests/test_assert.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace {

const std::vector<std::string>& nasty_args() {
    static const std::vector<std::string> args = {
        "plain",      "a b c",      "\"double\"", "'single'",    "back\\slash",
        "trailing\\", "q\\\"mix",   "$PATH",      "%PATH%",      "100% sure",
        "a!b",        "caret^here", "amp&here",   "(parens)",    "",
        "--gpu",      "tab\there",  "nl\nhere",   "caf\xC3\xA9", "\xE6\x97\xA5\xE6\x9C\xAC",
    };
    return args;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: integration_argv_echo <argv-echo-path>\n");
        return 2;
    }
    const std::string echo_path = argv[1];

    namespace fs = std::filesystem;
    const fs::path cwd_dir = fs::temp_directory_path() / "vkr2 argv echo cwd";
    std::error_code ec;
    fs::create_directories(cwd_dir, ec);

    vkr::process::SpawnRequest req;
    req.argv.push_back(echo_path);
    for (const auto& a : nasty_args()) {
        req.argv.push_back(a);
    }
    req.env_overrides = {
        {"VKR_T_SPACE", "a b c"},      {"VKR_T_QUOTE", "he said \"hi\""},
        {"VKR_T_EQUALS", "k=v"},       {"VKR_T_SEMI", "a;b;c"},
        {"VKR_T_MIX", "x=1; y=\"2\""}, {"VKR_T_UNICODE", "caf\xC3\xA9"},
    };
    req.cwd = cwd_dir.string();
    req.capture_stdout = true;

    std::string output;
    try {
        vkr::process::Process child = vkr::process::Process::spawn(req);
        output = child.read_stdout_to_end();
        int code = 0;
        child.wait(-1, code);
        VKR_CHECK_EQ(code, 0);
    } catch (const std::exception& e) {
        ::vkr::test::report_fail(__FILE__, __LINE__, std::string("spawn failed: ") + e.what());
        return vkr::test::finish("integration_argv_echo");
    }

    vkr::json::Value parsed;
    std::string err;
    if (!vkr::json::Value::try_parse(output, parsed, err)) {
        ::vkr::test::report_fail(__FILE__, __LINE__,
                                 "echo output not JSON: " + err + " :: " + output);
        return vkr::test::finish("integration_argv_echo");
    }

    // argv[0] is the program; argv[1..] must match the nasty inputs exactly.
    const auto& echoed = parsed.find("argv")->as_array();
    VKR_CHECK_EQ(echoed.size(), nasty_args().size() + 1);
    for (std::size_t i = 0; i < nasty_args().size() && i + 1 < echoed.size(); ++i) {
        VKR_CHECK_EQ(echoed[i + 1].as_string(), nasty_args()[i]);
    }

    // Environment overrides must arrive verbatim.
    const vkr::json::Value* env = parsed.find("env");
    VKR_CHECK(env != nullptr && env->is_object());
    if (env != nullptr && env->is_object()) {
        for (const auto& kv : req.env_overrides) {
            const vkr::json::Value* v = env->find(kv.first);
            if (v == nullptr) {
                ::vkr::test::report_fail(__FILE__, __LINE__, "missing env key: " + kv.first);
                continue;
            }
            VKR_CHECK_EQ(v->as_string(), kv.second);
        }
    }

    // cwd must point at the directory we requested (compare by identity to
    // tolerate separator/case canonicalization).
    const std::string echoed_cwd = parsed.find("cwd")->as_string();
    const bool same = fs::equivalent(fs::path(echoed_cwd), cwd_dir, ec) && !ec;
    if (!same) {
        ::vkr::test::report_fail(__FILE__, __LINE__,
                                 "cwd mismatch: echoed=" + echoed_cwd +
                                     " expected=" + cwd_dir.string());
    }
    ++::vkr::test::counters().checks;

    return vkr::test::finish("integration_argv_echo");
}
