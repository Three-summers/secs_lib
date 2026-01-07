#pragma once

#include "secs/core/common.hpp"
#include "secs/core/error.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace secs::utils {

/**
 * @brief 16 进制解析/格式化工具。
 *
 * 典型使用场景：
 * - 从抓包/日志里复制一段 “00 00 00 0A ...” 的字符串，解析为 bytes；
 * - 将 bytes 以 hexdump 形式输出，便于人工排查字段。
 */

struct HexDumpOptions final {
    // 每行字节数（典型 16/32）。
    std::size_t bytes_per_line{16};

    // 输出的最大字节数（0 表示不限制）。超出部分会打印截断提示。
    std::size_t max_bytes{256};

    // 是否输出行首偏移（0000:）。
    bool show_offset{true};

    // 是否输出 ASCII 侧栏（仅展示可打印字符，其余用 '.'）。
    bool show_ascii{false};

    // 是否输出 ANSI 颜色控制码（终端更易读；写入日志/文件时建议关闭）。
    bool enable_color{false};
};

/**
 * @brief 将 bytes 以 hexdump 形式格式化为字符串。
 */
[[nodiscard]] std::string hex_dump(secs::core::bytes_view bytes,
                                   HexDumpOptions options = {});

/**
 * @brief 解析 16 进制字符串为 bytes。
 *
 * 支持：
 * - 大小写 hex；
 * - 分隔符：空白、逗号、冒号、连字符、下划线、方括号等；
 * - 可选的 0x/0X 前缀（会被忽略）。
 *
 * 失败返回 core::errc::invalid_argument。
 */
std::error_code parse_hex(std::string_view text,
                          std::vector<secs::core::byte> &out) noexcept;

} // namespace secs::utils
