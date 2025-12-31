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
// HSMS payload（Header + Body）长度来自网络输入；需要上限避免恶意 length 触发巨量分配。
inline constexpr std::uint32_t kMaxPayloadSize = 16u * 1024u * 1024u;  // 16MB

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
  std::uint16_t session_id{0};
  std::uint8_t header_byte2{0};
  std::uint8_t header_byte3{0};
  std::uint8_t p_type{kPTypeSecs2};
  SType s_type{SType::data};
  std::uint32_t system_bytes{0};
};

struct Message final {
  Header header{};
  std::vector<core::byte> body{};

  [[nodiscard]] bool is_data() const noexcept { return header.s_type == SType::data; }
  [[nodiscard]] bool is_control() const noexcept { return header.s_type != SType::data; }

  // 数据消息语义：header_byte2 的 bit7 为 W-bit，其余 7bit 为 Stream。
  [[nodiscard]] bool w_bit() const noexcept { return (header.header_byte2 & 0x80U) != 0; }
  [[nodiscard]] std::uint8_t stream() const noexcept { return static_cast<std::uint8_t>(header.header_byte2 & 0x7FU); }
  [[nodiscard]] std::uint8_t function() const noexcept { return header.header_byte3; }
};

// 控制消息构造助手：control message 默认 body 为空，header_byte2/3 为 0。
[[nodiscard]] Message make_select_req(std::uint16_t session_id, std::uint32_t system_bytes);
[[nodiscard]] Message make_select_rsp(std::uint16_t status, std::uint32_t system_bytes);
[[nodiscard]] Message make_deselect_req(std::uint16_t session_id, std::uint32_t system_bytes);
[[nodiscard]] Message make_deselect_rsp(std::uint16_t status, std::uint32_t system_bytes);
[[nodiscard]] Message make_linktest_req(std::uint16_t session_id, std::uint32_t system_bytes);
[[nodiscard]] Message make_linktest_rsp(std::uint16_t status, std::uint32_t system_bytes);
[[nodiscard]] Message make_separate_req(std::uint16_t session_id, std::uint32_t system_bytes);

// 数据消息构造助手：body 为原始 SECS-II payload（由上层编解码）。
[[nodiscard]] Message make_data_message(
  std::uint16_t session_id,
  std::uint8_t stream,
  std::uint8_t function,
  bool w_bit,
  std::uint32_t system_bytes,
  core::bytes_view body);

// 编码：输出完整 TCP frame（Length(4B) + Header(10B) + body）。
[[nodiscard]] std::vector<core::byte> encode_frame(const Message& msg);

// 解码：输入完整 TCP frame（含 4B length），若成功 consumed 为该 frame 总长度。
std::error_code decode_frame(core::bytes_view frame, Message& out, std::size_t& consumed) noexcept;

// 解码：仅解析 payload（Header(10B)+body），用于连接层读到 length 后的解析。
std::error_code decode_payload(core::bytes_view payload, Message& out) noexcept;

}  // namespace secs::hsms
