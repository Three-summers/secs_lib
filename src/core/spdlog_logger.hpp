#pragma once

#include <spdlog/spdlog.h>

namespace secs::core {

// 返回库内部使用的命名 logger（默认名称：secs）。
//
// 说明：
// - 该接口仅供 src/ 下源文件使用，避免在 public headers 暴露 spdlog 类型；
// - 若创建/注册失败，会回退到 spdlog 的 default logger。
[[nodiscard]] spdlog::logger *spdlog_logger_raw() noexcept;

} // namespace secs::core

