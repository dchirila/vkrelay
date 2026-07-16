// Structured-ish process logging for vkrelay2.
//
// Emits one line per record to stderr:
//   2026-06-17T18:30:00.123Z INFO supervisor: message key=value key2=value2
//
// Thread-safe. The default minimum level and process component name can be
// configured once at startup. Session launchers can route the records into per-session bundles.
#ifndef VKRELAY2_COMMON_LOGGING_LOGGING_HPP
#define VKRELAY2_COMMON_LOGGING_LOGGING_HPP

#include <sstream>
#include <string>

namespace vkr::log {

enum class Level { Trace, Debug, Info, Warn, Error };

void set_min_level(Level level);
Level min_level();
void set_component(std::string component);
const std::string& component();
bool enabled(Level level);

// One log line, flushed when the Record is destroyed.
class Record {
  public:
    Record(Level level, std::string component);
    ~Record();

    Record(const Record&) = delete;
    Record& operator=(const Record&) = delete;

    template <typename T> Record& operator<<(const T& value) {
        if (active_) {
            message_ << value;
        }
        return *this;
    }

    // Appends a structured " key=value" pair after the free-text message.
    Record& kv(const char* key, const std::string& value);
    Record& kv(const char* key, long long value);

  private:
    bool active_;
    Level level_;
    std::string component_;
    std::ostringstream message_;
    std::ostringstream fields_;
};

} // namespace vkr::log

#define VKR_LOG(level, component) ::vkr::log::Record((level), (component))
#define VKR_TRACE(component) VKR_LOG(::vkr::log::Level::Trace, (component))
#define VKR_DEBUG(component) VKR_LOG(::vkr::log::Level::Debug, (component))
#define VKR_INFO(component) VKR_LOG(::vkr::log::Level::Info, (component))
#define VKR_WARN(component) VKR_LOG(::vkr::log::Level::Warn, (component))
#define VKR_ERROR(component) VKR_LOG(::vkr::log::Level::Error, (component))

#endif // VKRELAY2_COMMON_LOGGING_LOGGING_HPP
