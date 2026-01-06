#pragma once

#include <cstdint>

namespace secs::core {

/**
 * @brief 日志级别（用于库内 spdlog 日志的统一控制）。
 *
 * 说明：
 * - 本库内部日志使用 spdlog，但不把 spdlog 类型暴露到 public headers；
 * - 业务侧可通过 set_log_level 调整全局日志级别。
 */
enum class LogLevel : std::uint8_t {
    trace = 0,
    debug = 1,
    info = 2,
    warn = 3,
    error = 4,
    critical = 5,
    off = 6,
};

void set_log_level(LogLevel level) noexcept;
[[nodiscard]] LogLevel log_level() noexcept;

} // namespace secs::core

