#pragma once

#include "secs/core/common.hpp"
#include "secs/hsms/message.hpp"
#include "secs/ii/codec.hpp"
#include "secs/utils/hex.hpp"
#include "secs/utils/item_dump.hpp"

#include <cstddef>
#include <string>

namespace secs::utils {

/**
 * @brief HSMS 帧解析与可视化输出（调试/抓包分析用途）。
 */
struct HsmsDumpOptions final {
    // 是否包含原始帧 hexdump。
    bool include_hex{true};

    // hexdump 选项（include_hex=true 时生效）。
    HexDumpOptions hex{};

    // 是否输出 ANSI 颜色控制码（终端更易读；写入日志/文件时建议关闭）。
    bool enable_color{false};

    // 是否尝试将 data message 的 body 解码为 SECS-II Item。
    // 这是一个“开关”：关闭时只展示 body 长度与原始 bytes（如果 include_hex 开启）。
    bool enable_secs2_decode{false};

    // SECS-II 解码资源限制（enable_secs2_decode=true 时使用）。
    secs::ii::DecodeLimits secs2_limits{};

    // SECS-II Item 的输出格式（enable_secs2_decode=true 时使用）。
    ItemDumpOptions item{};
};

/**
 * @brief 解析并输出完整 HSMS TCP 帧（含 4B Length 字段）。
 *
 * 如果解析失败，返回字符串中会包含错误信息。
 */
[[nodiscard]] std::string dump_hsms_frame(secs::core::bytes_view frame,
                                          HsmsDumpOptions options = {});

/**
 * @brief 解析并输出 HSMS 负载（10B Header + body，不含 4B Length）。
 *
 * 如果解析失败，返回字符串中会包含错误信息。
 */
[[nodiscard]] std::string dump_hsms_payload(secs::core::bytes_view payload,
                                            HsmsDumpOptions options = {});

} // namespace secs::utils
