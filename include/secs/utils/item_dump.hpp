#pragma once

#include "secs/ii/codec.hpp"
#include "secs/ii/item.hpp"

#include <cstddef>
#include <string>

namespace secs::utils {

/**
 * @brief SECS-II Item 的可读化输出（调试/日志用途）。
 *
 * 说明：
 * - 该输出不是严格的 SML 语法，但尽量贴近 SECS 领域习惯；
 * - 默认会对超长内容做截断，避免日志被巨量 payload 淹没；
 * - 如需与 on-wire 字节对齐的解析，请先用 `secs::ii::decode_one()` 解码为 Item。
 */
struct ItemDumpOptions final {
    // 递归最大深度（0 表示只输出根节点）。
    std::size_t max_depth{16};

    // List 最大输出元素数（0 表示不限制）。
    std::size_t max_list_items{128};

    // ASCII/Binary 最大输出字节数（0 表示不限制）。
    std::size_t max_payload_bytes{256};

    // 数值数组最大输出元素数（0 表示不限制）。
    std::size_t max_array_items{64};

    // List 是否使用多行缩进格式。
    bool multiline{true};

    // 每层缩进空格数（multiline=true 时生效）。
    std::size_t indent_spaces{2};

    // 是否输出 ANSI 颜色控制码（终端更易读；写入日志/文件时建议关闭）。
    bool enable_color{false};
};

/**
 * @brief 将 Item 格式化为字符串。
 */
[[nodiscard]] std::string dump_item(const secs::ii::Item &item,
                                    ItemDumpOptions options = {});

} // namespace secs::utils
