#pragma once

#include "secs/core/common.hpp"
#include "secs/core/error.hpp"

#include <cstddef>
#include <cstdint>
#include <system_error>
#include <vector>

namespace secs::hsms {

inline constexpr std::size_t kLengthFieldSize = 4;
inline constexpr std::size_t kHeaderSize = 10;
inline constexpr std::uint8_t kPTypeSecs2 = 0x00;
// HSMS 负载（头部 +
// 消息体）长度来自网络输入；需要上限避免恶意长度字段触发巨量分配。
inline constexpr std::uint32_t kMaxPayloadSize =
    16u * 1024u * 1024u; // 16MB 上限

enum class SType : std::uint8_t {
    data = 0x00,
    select_req = 0x01,
    select_rsp = 0x02,
    deselect_req = 0x03,
    deselect_rsp = 0x04,
    linktest_req = 0x05,
    linktest_rsp = 0x06,
    reject_req = 0x07,
    separate_req = 0x09,
};

struct Header final {
    /**
     * @brief HSMS SessionID（2B）。
     *
     * 语义提示（HSMS-SS 常见约定）：
     * - 控制消息（SELECT/LINKTEST/SEPARATE 等）：通常固定为 0xFFFF；
     * - 数据消息（SType=0）：通常低 15 位为 DeviceID（高位为 0）。
     *
     * 注意：本库对“控制消息必须为 0xFFFF”的强校验在 Session 层完成，
     * Header/Message 只负责承载字节级字段。
     */
    std::uint16_t session_id{0};

    /**
     * @brief Header Byte 2（1B）。
     *
     * - 数据消息：bit7=W-bit，其余 7 bit 为 Stream（0..127）
     * - 控制消息：E37 对该字段通常定义为保留/状态码等；本库约定：
     *   - Select.rsp / Deselect.rsp：header_byte2 = status（0=OK）
     *   - Reject.req：header_byte2 = reason code
     */
    std::uint8_t header_byte2{0};
    std::uint8_t header_byte3{0};
    std::uint8_t p_type{kPTypeSecs2};
    SType s_type{SType::data};
    std::uint32_t system_bytes{0};
};

struct Message final {
    Header header{};
    std::vector<core::byte> body{};

    [[nodiscard]] bool is_data() const noexcept {
        return header.s_type == SType::data;
    }
    [[nodiscard]] bool is_control() const noexcept {
        return header.s_type != SType::data;
    }

    // 数据消息语义：header_byte2 的第 7 位为 W 位，其余 7 位为 Stream。
    [[nodiscard]] bool w_bit() const noexcept {
        return (header.header_byte2 & 0x80U) != 0;
    }
    [[nodiscard]] std::uint8_t stream() const noexcept {
        return static_cast<std::uint8_t>(header.header_byte2 & 0x7FU);
    }
    [[nodiscard]] std::uint8_t function() const noexcept {
        return header.header_byte3;
    }
};

// 控制消息构造助手：控制消息默认消息体为空，header_byte2/3 为 0。
[[nodiscard]] Message make_select_req(std::uint16_t session_id,
                                      std::uint32_t system_bytes);
// Select.rsp：header_byte2 承载 1B 状态码（0=接受，其它为拒绝原因）。
[[nodiscard]] Message make_select_rsp(std::uint16_t session_id,
                                      std::uint8_t status,
                                      std::uint32_t system_bytes);
[[nodiscard]] Message make_deselect_req(std::uint16_t session_id,
                                        std::uint32_t system_bytes);
// Deselect.rsp：header_byte2 承载 1B 状态码（0=接受，其它为拒绝原因）。
[[nodiscard]] Message make_deselect_rsp(std::uint16_t session_id,
                                        std::uint8_t status,
                                        std::uint32_t system_bytes);
[[nodiscard]] Message make_linktest_req(std::uint16_t session_id,
                                        std::uint32_t system_bytes);
[[nodiscard]] Message make_linktest_rsp(std::uint16_t session_id,
                                        std::uint32_t system_bytes);
// Reject.req：header_byte2 承载 1B reason code；body 为被拒绝消息的 10B header。
[[nodiscard]] Message make_reject_req(std::uint8_t reason_code,
                                      const Header &rejected_header);
[[nodiscard]] Message make_separate_req(std::uint16_t session_id,
                                        std::uint32_t system_bytes);

// 数据消息构造助手：消息体为原始 SECS-II 负载（由上层编解码）。
[[nodiscard]] Message make_data_message(std::uint16_t session_id,
                                        std::uint8_t stream,
                                        std::uint8_t function,
                                        bool w_bit,
                                        std::uint32_t system_bytes,
                                        core::bytes_view body);

// 编码：输出完整 TCP 帧（长度字段 4B + 头部 10B + 消息体）。
// 注意：
// - 该函数会校验 payload 上限（kMaxPayloadSize）与 PType（仅支持 0x00=SECS-II）。
// - 如需拿到错误码，请优先使用带 out 参数的重载。
std::error_code encode_frame(const Message &msg,
                             std::vector<core::byte> &out) noexcept;
[[nodiscard]] std::vector<core::byte> encode_frame(const Message &msg);

// 解码：输入完整 TCP 帧（含 4B 长度字段），若成功 consumed 为该帧总长度。
std::error_code decode_frame(core::bytes_view frame,
                             Message &out,
                             std::size_t &consumed) noexcept;

// 解码：仅解析负载（头部 10B + 消息体），用于连接层读到长度字段后的解析。
std::error_code decode_payload(core::bytes_view payload, Message &out) noexcept;

} // namespace secs::hsms
