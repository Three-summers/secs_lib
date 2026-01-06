#include "secs/hsms/message.hpp"

#include <array>
#include <cstring>
#include <new>
#include <stdexcept>

namespace secs::hsms {
namespace {

/*
 * HSMS 字节级编解码（SEMI E37）：
 *
 * - TCP 帧格式：
 *   [Message Length: 4B big-endian] [Header: 10B] [Message Text: 0..N]
 *
 * - Header(10B) 字段布局：
 *   - session_id     : 2B
 *   - header_byte2   : 1B
 *   - header_byte3   : 1B
 *   - p_type         : 1B（本库仅支持 0x00=SECS-II）
 *   - s_type         : 1B（0=data，其它为控制消息）
 *   - system_bytes   : 4B（请求-响应关联）
 *
 * - 字段语义的层次划分：
 *   - 本文件只做“字节级”封装/解析（含长度上限校验），不关心会话状态机；
 *   - 例如：HSMS-SS 控制消息 SessionID=0xFFFF 等约束在 Session 层完成。
 *
 * - header_byte2 的复用约定（与 include/secs/hsms/message.hpp 保持一致）：
 *   - data message：bit7=W-bit，低 7 位为 Stream
 *   - Select/Deselect.rsp：header_byte2 = status
 *   - Reject.req：header_byte2 = reason code；body 回显被拒绝消息的 10B header
 */

std::uint16_t read_u16_be(const core::byte *p) noexcept {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(p[0]) << 8U) |
                                      static_cast<std::uint16_t>(p[1]));
}

std::uint32_t read_u32_be(const core::byte *p) noexcept {
    return (static_cast<std::uint32_t>(p[0]) << 24U) |
           (static_cast<std::uint32_t>(p[1]) << 16U) |
           (static_cast<std::uint32_t>(p[2]) << 8U) |
           static_cast<std::uint32_t>(p[3]);
}

void write_u16_be(core::byte *p, std::uint16_t v) noexcept {
    p[0] = static_cast<core::byte>((v >> 8U) & 0xFFU);
    p[1] = static_cast<core::byte>(v & 0xFFU);
}

void write_u32_be(core::byte *p, std::uint32_t v) noexcept {
    p[0] = static_cast<core::byte>((v >> 24U) & 0xFFU);
    p[1] = static_cast<core::byte>((v >> 16U) & 0xFFU);
    p[2] = static_cast<core::byte>((v >> 8U) & 0xFFU);
    p[3] = static_cast<core::byte>(v & 0xFFU);
}

std::vector<core::byte> encode_header_bytes(const Header &h) {
    std::vector<core::byte> out;
    out.resize(kHeaderSize);
    write_u16_be(out.data() + 0, h.session_id);
    out[2] = h.header_byte2;
    out[3] = h.header_byte3;
    out[4] = h.p_type;
    out[5] = static_cast<core::byte>(h.s_type);
    write_u32_be(out.data() + 6, h.system_bytes);
    return out;
}

Message make_control_base(SType stype,
                          std::uint16_t session_id,
                          std::uint32_t system_bytes) {
    Message m;
    m.header.session_id = session_id;
    m.header.header_byte2 = 0;
    m.header.header_byte3 = 0;
    m.header.p_type = kPTypeSecs2;
    m.header.s_type = stype;
    m.header.system_bytes = system_bytes;
    return m;
}

} // namespace

Message make_select_req(std::uint16_t session_id, std::uint32_t system_bytes) {
    return make_control_base(SType::select_req, session_id, system_bytes);
}

Message make_select_rsp(std::uint16_t session_id,
                        std::uint8_t status,
                        std::uint32_t system_bytes) {
    auto m = make_control_base(SType::select_rsp, session_id, system_bytes);
    m.header.header_byte2 = status;
    return m;
}

Message make_deselect_req(std::uint16_t session_id,
                          std::uint32_t system_bytes) {
    return make_control_base(SType::deselect_req, session_id, system_bytes);
}

Message make_deselect_rsp(std::uint16_t session_id,
                          std::uint8_t status,
                          std::uint32_t system_bytes) {
    auto m = make_control_base(SType::deselect_rsp, session_id, system_bytes);
    m.header.header_byte2 = status;
    return m;
}

Message make_linktest_req(std::uint16_t session_id,
                          std::uint32_t system_bytes) {
    return make_control_base(SType::linktest_req, session_id, system_bytes);
}

Message make_linktest_rsp(std::uint16_t session_id, std::uint32_t system_bytes) {
    return make_control_base(SType::linktest_rsp, session_id, system_bytes);
}

Message make_reject_req(std::uint8_t reason_code,
                        const Header &rejected_header) {
    // Reject.req：消息体携带被拒绝消息的 10B header（字节级回显）。
    auto m = make_control_base(SType::reject_req,
                               rejected_header.session_id,
                               rejected_header.system_bytes);
    m.header.header_byte2 = reason_code;
    m.body = encode_header_bytes(rejected_header);
    return m;
}

Message make_separate_req(std::uint16_t session_id,
                          std::uint32_t system_bytes) {
    return make_control_base(SType::separate_req, session_id, system_bytes);
}

Message make_data_message(std::uint16_t session_id,
                          std::uint8_t stream,
                          std::uint8_t function,
                          bool w_bit,
                          std::uint32_t system_bytes,
                          core::bytes_view body) {
    Message m;
    m.header.session_id = session_id;
    m.header.header_byte2 =
        static_cast<std::uint8_t>((w_bit ? 0x80U : 0x00U) | (stream & 0x7FU));
    m.header.header_byte3 = function;
    m.header.p_type = kPTypeSecs2;
    m.header.s_type = SType::data;
    m.header.system_bytes = system_bytes;
    m.body.assign(body.begin(), body.end());
    return m;
}

std::vector<core::byte> encode_frame(const Message &msg) {
    std::vector<core::byte> out;
    auto ec = encode_frame(msg, out);
    if (ec) {
        return {};
    }
    return out;
}

std::error_code encode_frame(const Message &msg,
                             std::vector<core::byte> &out) noexcept {
    out.clear();

    if (msg.header.p_type != kPTypeSecs2) {
        return core::make_error_code(core::errc::invalid_argument);
    }

    const auto header_size = static_cast<std::size_t>(kHeaderSize);
    const auto max_payload_size = static_cast<std::size_t>(kMaxPayloadSize);
    if (max_payload_size < header_size) {
        return core::make_error_code(core::errc::invalid_argument);
    }

    const std::size_t max_body_size = max_payload_size - header_size;
    if (msg.body.size() > max_body_size) {
        return core::make_error_code(core::errc::buffer_overflow);
    }

    const auto payload_len = static_cast<std::uint32_t>(header_size +
                                                        msg.body.size());
    try {
        out.resize(static_cast<std::size_t>(kLengthFieldSize) + payload_len);
    } catch (const std::bad_alloc &) {
        return core::make_error_code(core::errc::out_of_memory);
    } catch (const std::length_error &) {
        return core::make_error_code(core::errc::buffer_overflow);
    } catch (...) {
        return core::make_error_code(core::errc::invalid_argument);
    }

    write_u32_be(out.data(), payload_len);

    auto *h = out.data() + kLengthFieldSize;
    write_u16_be(h + 0, msg.header.session_id);
    h[2] = msg.header.header_byte2;
    h[3] = msg.header.header_byte3;
    h[4] = msg.header.p_type;
    h[5] = static_cast<core::byte>(msg.header.s_type);
    write_u32_be(h + 6, msg.header.system_bytes);

    if (!msg.body.empty()) {
        std::memcpy(h + kHeaderSize, msg.body.data(), msg.body.size());
    }

    return {};
}

std::error_code decode_payload(core::bytes_view payload,
                               Message &out) noexcept {
    if (payload.size() < kHeaderSize) {
        return core::make_error_code(core::errc::invalid_argument);
    }
    if (payload.size() > static_cast<std::size_t>(kMaxPayloadSize)) {
        return core::make_error_code(core::errc::buffer_overflow);
    }

    const auto *p = payload.data();
    Header h;
    h.session_id = read_u16_be(p + 0);
    h.header_byte2 = p[2];
    h.header_byte3 = p[3];
    h.p_type = p[4];
    h.s_type = static_cast<SType>(p[5]);
    h.system_bytes = read_u32_be(p + 6);

    if (h.p_type != kPTypeSecs2) {
        return core::make_error_code(core::errc::invalid_argument);
    }

    out.header = h;
    try {
        out.body.assign(
            payload.begin() + static_cast<std::ptrdiff_t>(kHeaderSize),
            payload.end());
    } catch (const std::bad_alloc &) {
        return core::make_error_code(core::errc::out_of_memory);
    } catch (const std::length_error &) {
        return core::make_error_code(core::errc::buffer_overflow);
    } catch (...) {
        return core::make_error_code(core::errc::invalid_argument);
    }
    return {};
}

std::error_code decode_frame(core::bytes_view frame,
                             Message &out,
                             std::size_t &consumed) noexcept {
    consumed = 0;
    if (frame.size() < kLengthFieldSize) {
        return core::make_error_code(core::errc::invalid_argument);
    }

    const std::uint32_t payload_len = read_u32_be(frame.data());
    if (payload_len < kHeaderSize) {
        return core::make_error_code(core::errc::invalid_argument);
    }
    if (payload_len > kMaxPayloadSize) {
        return core::make_error_code(core::errc::buffer_overflow);
    }

    const std::size_t total_len =
        static_cast<std::size_t>(kLengthFieldSize) + payload_len;
    if (frame.size() < total_len) {
        return core::make_error_code(core::errc::invalid_argument);
    }

    auto ec = decode_payload(frame.subspan(kLengthFieldSize, payload_len), out);
    if (ec) {
        return ec;
    }

    consumed = total_len;
    return {};
}

} // namespace secs::hsms
