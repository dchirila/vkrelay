#include "common/logging/logging.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <iostream>
#include <mutex>

namespace vkr::log {
namespace {

std::atomic<Level> g_min_level{Level::Info};
std::mutex g_sink_mutex;
std::string g_component = "vkrelay2"; // guarded by g_sink_mutex for writes

const char* level_name(Level level) {
    switch (level) {
    case Level::Trace:
        return "TRACE";
    case Level::Debug:
        return "DEBUG";
    case Level::Info:
        return "INFO";
    case Level::Warn:
        return "WARN";
    case Level::Error:
        return "ERROR";
    }
    return "INFO";
}

std::string timestamp_utc() {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto secs = time_point_cast<seconds>(now);
    const auto millis = duration_cast<milliseconds>(now - secs).count();
    const std::time_t t = system_clock::to_time_t(secs);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    // 64 bytes: -O2's -Wformat-truncation reasons about the FULL int ranges (a theoretical
    // 11-digit year etc.), so size for the worst case rather than the real 24-char output.
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", tm.tm_year + 1900,
                  tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec,
                  static_cast<int>(millis));
    return buf;
}

} // namespace

void set_min_level(Level level) {
    g_min_level.store(level, std::memory_order_relaxed);
}
Level min_level() {
    return g_min_level.load(std::memory_order_relaxed);
}

void set_component(std::string component) {
    std::lock_guard<std::mutex> lock(g_sink_mutex);
    g_component = std::move(component);
}

const std::string& component() {
    return g_component;
}

bool enabled(Level level) {
    return static_cast<int>(level) >= static_cast<int>(min_level());
}

Record::Record(Level level, std::string component)
    : active_(enabled(level)), level_(level), component_(std::move(component)) {}

Record& Record::kv(const char* key, const std::string& value) {
    if (active_) {
        fields_ << ' ' << key << '=' << value;
    }
    return *this;
}

Record& Record::kv(const char* key, long long value) {
    if (active_) {
        fields_ << ' ' << key << '=' << value;
    }
    return *this;
}

Record::~Record() {
    if (!active_) {
        return;
    }
    const std::string line = timestamp_utc() + " " + level_name(level_) + " " + component_ + ": " +
                             message_.str() + fields_.str() + "\n";
    std::lock_guard<std::mutex> lock(g_sink_mutex);
    std::fputs(line.c_str(), stderr);
    std::fflush(stderr);
}

} // namespace vkr::log
