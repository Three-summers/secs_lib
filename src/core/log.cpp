#include "secs/core/log.hpp"

#include "core/spdlog_logger.hpp"

#include <spdlog/spdlog.h>

#include <mutex>

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

namespace {

spdlog::logger *ensure_secs_logger_raw_() noexcept {
    static std::shared_ptr<spdlog::logger> logger;
    static std::once_flag once;

    try {
        std::call_once(once, []() noexcept {
            try {
                auto base = spdlog::default_logger();
                if (!base) {
                    return;
                }
                auto l = std::make_shared<spdlog::logger>("secs",
                                                          base->sinks().begin(),
                                                          base->sinks().end());
                l->set_level(base->level());
                logger = std::move(l);
            } catch (...) {
                // 兜底：创建失败时保持为空，调用方会降级到 default logger。
            }
        });
    } catch (...) {
    }

    return logger ? logger.get() : nullptr;
}

} // namespace

spdlog::logger *spdlog_logger_raw() noexcept {
    if (auto *logger = ensure_secs_logger_raw_()) {
        return logger;
    }
    return spdlog::default_logger_raw();
}

void set_log_level(LogLevel level) noexcept {
    // 仅调整库内命名 logger，避免污染宿主进程的全局日志策略。
    auto *logger = ensure_secs_logger_raw_();
    if (!logger) {
        return;
    }
    try {
        logger->set_level(to_spdlog_level(level));
    } catch (...) {
    }
}

LogLevel log_level() noexcept {
    auto *logger = ensure_secs_logger_raw_();
    if (!logger) {
        return LogLevel::off;
    }
    try {
        return from_spdlog_level(logger->level());
    } catch (...) {
        return LogLevel::off;
    }
}

} // namespace secs::core
