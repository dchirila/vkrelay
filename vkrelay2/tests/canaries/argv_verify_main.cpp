// argv-verify: shell-agnostic checker for the quoting smoke tests
// for the launcher's adversarial argument cases.
//
// Reads the JSON emitted by argv-echo and compares it against expected argv
// (NUL-separated file), expected env (KEY=VALUE flags/file), and expected cwd.
// All quoting-sensitive comparison happens here in C++ so the Bash and
// PowerShell wrappers never have to parse JSON or re-encode bytes. Returns 0
// when everything matches, 1 otherwise.
#include "common/util/json.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::vector<std::string> split_nul(const std::string& data) {
    std::vector<std::string> out;
    std::string current;
    for (char c : data) {
        if (c == '\0') {
            out.push_back(current);
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    if (!current.empty()) {
        out.push_back(current);
    }
    return out;
}

int g_failures = 0;
void fail(const std::string& msg) {
    ++g_failures;
    std::cerr << "argv-verify FAIL: " << msg << "\n";
}

} // namespace

int main(int argc, char** argv) {
    std::vector<std::string> expect_argv;
    std::vector<std::pair<std::string, std::string>> expect_env;
    std::string expect_cwd;
    bool have_cwd = false;
    std::string actual_file;
    bool have_expect_argv = false;

    auto value = [&](int& i, const char* opt) -> std::string {
        if (i + 1 >= argc) {
            std::cerr << opt << " requires a value\n";
            std::exit(2);
        }
        return argv[++i];
    };

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--expect-argv-file") {
            expect_argv = split_nul(read_file(value(i, "--expect-argv-file")));
            have_expect_argv = true;
        } else if (arg == "--expect-env") {
            const std::string kv = value(i, "--expect-env");
            const std::size_t eq = kv.find('=');
            expect_env.emplace_back(kv.substr(0, eq),
                                    eq == std::string::npos ? "" : kv.substr(eq + 1));
        } else if (arg == "--expect-env-file") {
            for (const std::string& entry : split_nul(read_file(value(i, "--expect-env-file")))) {
                const std::size_t eq = entry.find('=');
                expect_env.emplace_back(entry.substr(0, eq),
                                        eq == std::string::npos ? "" : entry.substr(eq + 1));
            }
        } else if (arg == "--cwd") {
            expect_cwd = value(i, "--cwd");
            have_cwd = true;
        } else if (arg == "--cwd-file") {
            expect_cwd = read_file(value(i, "--cwd-file"));
            while (!expect_cwd.empty() &&
                   (expect_cwd.back() == '\n' || expect_cwd.back() == '\r')) {
                expect_cwd.pop_back();
            }
            have_cwd = true;
        } else if (arg == "--actual-file") {
            actual_file = value(i, "--actual-file");
        } else {
            std::cerr << "unknown argument: " << arg << "\n";
            return 2;
        }
    }

    std::string actual_text;
    if (actual_file.empty()) {
        std::ostringstream ss;
        ss << std::cin.rdbuf();
        actual_text = ss.str();
    } else {
        actual_text = read_file(actual_file);
    }

    vkr::json::Value actual;
    std::string err;
    if (!vkr::json::Value::try_parse(actual_text, actual, err)) {
        fail("actual output is not JSON: " + err);
        return 1;
    }

    if (have_expect_argv) {
        const vkr::json::Value* echoed = actual.find("argv");
        if (echoed == nullptr || !echoed->is_array()) {
            fail("actual has no argv array");
        } else {
            const auto& arr = echoed->as_array();
            if (arr.size() != expect_argv.size() + 1) {
                fail("argv count mismatch: actual=" + std::to_string(arr.size()) +
                     " expected=" + std::to_string(expect_argv.size() + 1) + " (incl program)");
            }
            for (std::size_t i = 0; i < expect_argv.size() && i + 1 < arr.size(); ++i) {
                if (arr[i + 1].as_string() != expect_argv[i]) {
                    fail("argv[" + std::to_string(i + 1) + "] mismatch: actual=[" +
                         arr[i + 1].as_string() + "] expected=[" + expect_argv[i] + "]");
                }
            }
        }
    }

    const vkr::json::Value* env = actual.find("env");
    for (const auto& kv : expect_env) {
        const vkr::json::Value* v = (env != nullptr) ? env->find(kv.first) : nullptr;
        if (v == nullptr) {
            fail("missing env key: " + kv.first);
        } else if (v->as_string() != kv.second) {
            fail("env[" + kv.first + "] mismatch: actual=[" + v->as_string() + "] expected=[" +
                 kv.second + "]");
        }
    }

    if (have_cwd) {
        const vkr::json::Value* cwd = actual.find("cwd");
        if (cwd == nullptr) {
            fail("actual has no cwd");
        } else {
            std::error_code ec;
            if (!std::filesystem::equivalent(std::filesystem::path(cwd->as_string()),
                                             std::filesystem::path(expect_cwd), ec) ||
                ec) {
                fail("cwd mismatch: actual=[" + cwd->as_string() + "] expected=[" + expect_cwd +
                     "]");
            }
        }
    }

    if (g_failures == 0) {
        std::cerr << "argv-verify PASS\n";
        return 0;
    }
    return 1;
}
