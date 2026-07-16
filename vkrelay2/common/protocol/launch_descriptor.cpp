#include "common/protocol/launch_descriptor.hpp"

#include "common/util/json.hpp"

namespace vkr::protocol {
namespace {

bool contains_nul(const std::string& s) {
    return s.find('\0') != std::string::npos;
}

std::string read_string_field(const json::Value& obj, const std::string& key,
                              const std::string& fallback) {
    const json::Value* v = obj.find(key);
    if (v == nullptr || v->is_null()) {
        return fallback;
    }
    if (!v->is_string()) {
        throw DescriptorError("field '" + key + "' must be a string");
    }
    return v->as_string();
}

} // namespace

bool is_clean_utf8(const std::string& s) {
    std::size_t i = 0;
    const std::size_t n = s.size();
    while (i < n) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == 0) {
            return false; // embedded NUL
        }
        std::size_t extra = 0;
        std::uint32_t cp = 0;
        if (c < 0x80) {
            extra = 0;
            cp = c;
        } else if ((c & 0xE0) == 0xC0) {
            extra = 1;
            cp = c & 0x1F;
        } else if ((c & 0xF0) == 0xE0) {
            extra = 2;
            cp = c & 0x0F;
        } else if ((c & 0xF8) == 0xF0) {
            extra = 3;
            cp = c & 0x07;
        } else {
            return false;
        }
        if (i + extra >= n) {
            return false;
        }
        for (std::size_t k = 1; k <= extra; ++k) {
            const unsigned char cc = static_cast<unsigned char>(s[i + k]);
            if ((cc & 0xC0) != 0x80) {
                return false;
            }
            cp = (cp << 6) | (cc & 0x3F);
        }
        // Reject overlong encodings and out-of-range / surrogate code points.
        if (extra == 1 && cp < 0x80) {
            return false;
        }
        if (extra == 2 && cp < 0x800) {
            return false;
        }
        if (extra == 3 && cp < 0x10000) {
            return false;
        }
        if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
            return false;
        }
        i += extra + 1;
    }
    return true;
}

void LaunchDescriptor::validate() const {
    if (version != kLaunchDescriptorVersion) {
        throw DescriptorError("unsupported descriptor version " + std::to_string(version));
    }
    if (argv.empty()) {
        throw DescriptorError("descriptor must specify a non-empty argv");
    }
    for (const auto& arg : argv) {
        if (contains_nul(arg) || !is_clean_utf8(arg)) {
            throw DescriptorError("argv entry is not clean UTF-8");
        }
    }
    if (!cwd.empty() && (contains_nul(cwd) || !is_clean_utf8(cwd))) {
        throw DescriptorError("cwd is not clean UTF-8");
    }
    for (const auto& kv : env) {
        if (kv.first.empty()) {
            throw DescriptorError("environment variable name must not be empty");
        }
        if (kv.first.find('=') != std::string::npos) {
            throw DescriptorError("environment variable name must not contain '='");
        }
        if (!is_clean_utf8(kv.first) || !is_clean_utf8(kv.second)) {
            throw DescriptorError("environment entry is not clean UTF-8");
        }
    }
    if (!is_clean_utf8(session.gpu_selector) || !is_clean_utf8(session.display_backend) ||
        !is_clean_utf8(session.graphics_frontend)) {
        throw DescriptorError("session option is not clean UTF-8");
    }
}

std::string LaunchDescriptor::to_json(bool pretty) const {
    json::Value root = json::Value::make_object();
    root.set("version", json::Value(version));

    json::Value opts = json::Value::make_object();
    opts.set("gpu_selector", json::Value(session.gpu_selector));
    opts.set("display_backend", json::Value(session.display_backend));
    opts.set("graphics_frontend", json::Value(session.graphics_frontend));
    root.set("session_options", std::move(opts));

    json::Value process = json::Value::make_object();
    process.set("cwd", json::Value(cwd));
    json::Array argv_arr;
    for (const auto& arg : argv) {
        argv_arr.emplace_back(arg);
    }
    process.set("argv", json::Value(std::move(argv_arr)));
    json::Value env_obj = json::Value::make_object();
    for (const auto& kv : env) {
        env_obj.set(kv.first, json::Value(kv.second));
    }
    process.set("env", std::move(env_obj));
    root.set("process", std::move(process));

    return root.dump(pretty ? 2 : 0);
}

LaunchDescriptor LaunchDescriptor::from_json(const std::string& text) {
    if (text.size() > kLaunchDescriptorMaxBytes) {
        throw DescriptorError("descriptor payload exceeds size bound");
    }

    json::Value root;
    std::string err;
    if (!json::Value::try_parse(text, root, err)) {
        throw DescriptorError("descriptor is not valid JSON: " + err);
    }
    if (!root.is_object()) {
        throw DescriptorError("descriptor root must be an object");
    }

    LaunchDescriptor d;
    const json::Value* version = root.find("version");
    if (version == nullptr || !version->is_integer()) {
        // is_integer() rejects fractional/exponent/NaN/Inf so a version like
        // 1.4 is not silently rounded to 1.
        throw DescriptorError("descriptor 'version' must be an integer");
    }
    const std::int64_t version_value = version->as_int();
    if (version_value < 0 || version_value > 1000000) {
        throw DescriptorError("descriptor 'version' is out of range");
    }
    d.version = static_cast<int>(version_value);

    if (const json::Value* opts = root.find("session_options")) {
        if (!opts->is_object()) {
            throw DescriptorError("'session_options' must be an object");
        }
        d.session.gpu_selector = read_string_field(*opts, "gpu_selector", "auto");
        d.session.display_backend = read_string_field(*opts, "display_backend", "auto");
        d.session.graphics_frontend = read_string_field(*opts, "graphics_frontend", "auto");
    }

    const json::Value* process = root.find("process");
    if (process == nullptr || !process->is_object()) {
        throw DescriptorError("descriptor missing 'process' object");
    }
    d.cwd = read_string_field(*process, "cwd", "");

    const json::Value* argv = process->find("argv");
    if (argv == nullptr || !argv->is_array()) {
        throw DescriptorError("descriptor missing 'process.argv' array");
    }
    for (const auto& entry : argv->as_array()) {
        if (!entry.is_string()) {
            throw DescriptorError("argv entries must be strings");
        }
        d.argv.push_back(entry.as_string());
    }

    if (const json::Value* env = process->find("env")) {
        if (!env->is_object()) {
            throw DescriptorError("'process.env' must be an object");
        }
        for (const auto& kv : env->as_object()) {
            if (!kv.second.is_string()) {
                throw DescriptorError("environment values must be strings");
            }
            d.env.emplace_back(kv.first, kv.second.as_string());
        }
    }

    d.validate();
    return d;
}

} // namespace vkr::protocol
