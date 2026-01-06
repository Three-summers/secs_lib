#include "secs/core/log.hpp"

#include <spdlog/spdlog.h>

namespace secs::core {
namespace {

[[nodiscard]] spdlog::level::level_enum to_spdlog_level(LogLevel level) noexcept {
    switch (level) {
    case LogLevel::trace:
        return spdlog::level::trace;
    case LogLevel::debug:
        return spdlog::level::debug;
    case LogLevel::info:
        return spdlog::level::info;
    case LogLevel::warn:
        return spdlog::level::warn;
    case LogLevel::error:
        return spdlog::level::err;
    case LogLevel::critical:
        return spdlog::level::critical;
    case LogLevel::off:
        return spdlog::level::off;
    }
    return spdlog::level::off;
}

[[nodiscard]] LogLevel from_spdlog_level(spdlog::level::level_enum level) noexcept {
    switch (level) {
    case spdlog::level::trace:
        return LogLevel::trace;
    case spdlog::level::debug:
        return LogLevel::debug;
    case spdlog::level::info:
        return LogLevel::info;
    case spdlog::level::warn:
        return LogLevel::warn;
    case spdlog::level::err:
        return LogLevel::error;
    case spdlog::level::critical:
        return LogLevel::critical;
    case spdlog::level::off:
        return LogLevel::off;
    default:
        return LogLevel::off;
    }
}

} // namespace

void set_log_level(LogLevel level) noexcept {
    // spdlog 可能被业务侧额外配置；这里仅做最小的全局级别设置。
    spdlog::set_level(to_spdlog_level(level));
}

LogLevel log_level() noexcept { return from_spdlog_level(spdlog::get_level()); }

} // namespace secs::core

