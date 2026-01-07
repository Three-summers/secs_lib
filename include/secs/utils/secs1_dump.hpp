#pragma once

#include "secs/core/common.hpp"
#include "secs/ii/codec.hpp"
#include "secs/secs1/block.hpp"
#include "secs/utils/hex.hpp"
#include "secs/utils/item_dump.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace secs::utils {

/**
 * @brief SECS-I block/message 的解析与可视化输出（调试/抓包分析用途）。
 */
struct Secs1DumpOptions final {
    bool include_hex{true};
    HexDumpOptions hex{};

    // 是否输出 ANSI 颜色控制码（终端更易读；写入日志/文件时建议关闭）。
    bool enable_color{false};

    // 是否在“完整消息”级别尝试解码 SECS-II Item（开关）。
    // - 单 block 且 end_bit=1：可直接解码；
    // - 多 block：建议使用 Secs1MessageReassembler 组包后再解码。
    bool enable_secs2_decode{false};
    secs::ii::DecodeLimits secs2_limits{};
    ItemDumpOptions item{};
};

/**
 * @brief 解析并输出一个完整 SECS-I block frame（Length + Header + Data + Checksum）。
 *
 * 如果解析失败（含 checksum 错误），返回字符串中会包含错误信息。
 */
[[nodiscard]] std::string dump_secs1_block_frame(secs::core::bytes_view frame,
                                                 Secs1DumpOptions options = {});

/**
 * @brief SECS-I 多 block 消息重组器（并可选解码 SECS-II）。
 *
 * 用法：
 * - 对每个收到的 block frame 调用 accept_frame；
 * - 当 message_ready=true 时，通过 message_header()/message_body() 读取重组结果；
 * - 本类会在 message_ready 时内部自动 reset，以便继续处理下一条消息。
 */
class Secs1MessageReassembler final {
public:
    explicit Secs1MessageReassembler(
        std::optional<std::uint16_t> expected_device_id = std::nullopt);

    void reset() noexcept;

    /**
     * @brief 接收一个 block frame。
     *
     * @param frame 完整 block frame（Length + payload + checksum）
     * @param message_ready 输出：当前帧处理后是否完成一条消息
     */
    std::error_code accept_frame(secs::core::bytes_view frame,
                                 bool &message_ready);

    [[nodiscard]] const secs::secs1::Header &message_header() const noexcept {
        return message_header_;
    }
    [[nodiscard]] secs::core::bytes_view message_body() const noexcept {
        return secs::core::bytes_view{message_body_.data(), message_body_.size()};
    }

    /**
     * @brief 将当前完成的消息（header+body）格式化为字符串。
     *
     * 仅在最近一次 accept_frame 令 message_ready=true 时有效。
     */
    [[nodiscard]] std::string dump_message(Secs1DumpOptions options = {}) const;

    /**
     * @brief （可选）将 message_body 解码为 SECS-II Item。
     *
     * 仅在最近一次 accept_frame 令 message_ready=true 时有效。
     * 失败时返回错误码，并保持 out 未定义（调用方可自行 reset/out）。
     */
    std::error_code decode_message_body_as_secs2(secs::ii::Item &out,
                                                 std::size_t &consumed,
                                                 const secs::ii::DecodeLimits
                                                     &limits) const noexcept;

private:
    secs::secs1::Reassembler reassembler_;
    secs::secs1::Header message_header_{};
    std::vector<secs::core::byte> message_body_{};
};

} // namespace secs::utils
