#include "secs/core/error.hpp"
#include "secs/hsms/message.hpp"

#include "test_main.hpp"

#include <cstdint>
#include <vector>

namespace {

using secs::core::byte;
using secs::core::bytes_view;
using secs::core::errc;
using secs::core::make_error_code;
using secs::hsms::Header;
using secs::hsms::Message;
using secs::hsms::decode_frame;
using secs::hsms::decode_payload;
using secs::hsms::encode_frame;
using secs::hsms::kHeaderSize;
using secs::hsms::kLengthFieldSize;
using secs::hsms::kMaxPayloadSize;
using secs::hsms::kPTypeSecs2;
using secs::hsms::make_data_message;
using secs::hsms::make_deselect_req;
using secs::hsms::make_deselect_rsp;
using secs::hsms::make_linktest_req;
using secs::hsms::make_linktest_rsp;
using secs::hsms::make_reject_req;
using secs::hsms::make_separate_req;
using secs::hsms::make_select_req;
using secs::hsms::make_select_rsp;
using secs::hsms::SType;

static void write_u32_be(byte *p, std::uint32_t v) {
    p[0] = static_cast<byte>((v >> 24U) & 0xFFU);
    p[1] = static_cast<byte>((v >> 16U) & 0xFFU);
    p[2] = static_cast<byte>((v >> 8U) & 0xFFU);
    p[3] = static_cast<byte>(v & 0xFFU);
}

static std::uint16_t read_u16_be(const byte *p) noexcept {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(p[0]) << 8U) |
                                      static_cast<std::uint16_t>(p[1]));
}

static std::uint32_t read_u32_be(const byte *p) noexcept {
    return (static_cast<std::uint32_t>(p[0]) << 24U) |
           (static_cast<std::uint32_t>(p[1]) << 16U) |
           (static_cast<std::uint32_t>(p[2]) << 8U) |
           static_cast<std::uint32_t>(p[3]);
}

static void test_message_inline_accessors() {
    const std::vector<byte> body = {static_cast<byte>(0xAA),
                                    static_cast<byte>(0xBB)};
    const auto msg = make_data_message(/*session_id=*/0x0001,
                                       /*stream=*/12,
                                       /*function=*/34,
                                       /*w_bit=*/true,
                                       /*system_bytes=*/0x11223344,
                                       bytes_view{body.data(), body.size()});

    TEST_EXPECT(msg.is_data());
    TEST_EXPECT(!msg.is_control());
    TEST_EXPECT(msg.w_bit());
    TEST_EXPECT_EQ(msg.stream(), 12U);
    TEST_EXPECT_EQ(msg.function(), 34U);

    const auto ctrl = make_linktest_req(/*session_id=*/0xFFFF, 0x01020304);
    TEST_EXPECT(!ctrl.is_data());
    TEST_EXPECT(ctrl.is_control());
    TEST_EXPECT(!ctrl.w_bit());
}

static void test_make_control_messages() {
    {
        const auto m = make_select_req(0xFFFF, 0x01020304);
        TEST_EXPECT_EQ(m.header.session_id, 0xFFFF);
        TEST_EXPECT_EQ(m.header.s_type, SType::select_req);
        TEST_EXPECT_EQ(m.header.system_bytes, 0x01020304U);
        TEST_EXPECT(m.body.empty());
    }
    {
        const auto m = make_select_rsp(0xFFFF, /*status=*/7, 0xAABBCCDD);
        TEST_EXPECT_EQ(m.header.s_type, SType::select_rsp);
        TEST_EXPECT_EQ(m.header.header_byte2, 7U);
        TEST_EXPECT_EQ(m.header.system_bytes, 0xAABBCCDDU);
        TEST_EXPECT(m.body.empty());
    }
    {
        const auto m = make_deselect_req(0xFFFF, 0x01020304);
        TEST_EXPECT_EQ(m.header.s_type, SType::deselect_req);
        TEST_EXPECT(m.body.empty());
    }
    {
        const auto m = make_deselect_rsp(0xFFFF, /*status=*/9, 0x01020304);
        TEST_EXPECT_EQ(m.header.s_type, SType::deselect_rsp);
        TEST_EXPECT_EQ(m.header.header_byte2, 9U);
        TEST_EXPECT(m.body.empty());
    }
    {
        const auto m = make_linktest_req(0xFFFF, 0x01020304);
        TEST_EXPECT_EQ(m.header.s_type, SType::linktest_req);
        TEST_EXPECT(m.body.empty());
    }
    {
        const auto m = make_linktest_rsp(0xFFFF, 0x01020304);
        TEST_EXPECT_EQ(m.header.s_type, SType::linktest_rsp);
        TEST_EXPECT(m.body.empty());
    }
    {
        const auto m = make_separate_req(0xFFFF, 0x01020304);
        TEST_EXPECT_EQ(m.header.s_type, SType::separate_req);
        TEST_EXPECT(m.body.empty());
    }
    {
        Header rejected{};
        rejected.session_id = 0x1234;
        rejected.header_byte2 = 0x81; // W=1, Stream=1
        rejected.header_byte3 = 0x02;
        rejected.p_type = kPTypeSecs2;
        rejected.s_type = SType::data;
        rejected.system_bytes = 0x11223344;

        const auto m = make_reject_req(/*reason_code=*/0x55, rejected);
        TEST_EXPECT_EQ(m.header.s_type, SType::reject_req);
        TEST_EXPECT_EQ(m.header.session_id, rejected.session_id);
        TEST_EXPECT_EQ(m.header.header_byte2, 0x55U);
        TEST_EXPECT_EQ(m.header.system_bytes, rejected.system_bytes);
        TEST_EXPECT_EQ(m.body.size(), static_cast<std::size_t>(kHeaderSize));

        // body 为被拒绝消息的 10B header 回显
        TEST_EXPECT_EQ(read_u16_be(m.body.data() + 0), rejected.session_id);
        TEST_EXPECT_EQ(static_cast<std::uint8_t>(m.body[2]),
                       rejected.header_byte2);
        TEST_EXPECT_EQ(static_cast<std::uint8_t>(m.body[3]),
                       rejected.header_byte3);
        TEST_EXPECT_EQ(static_cast<std::uint8_t>(m.body[4]), rejected.p_type);
        TEST_EXPECT_EQ(static_cast<std::uint8_t>(m.body[5]),
                       static_cast<std::uint8_t>(rejected.s_type));
        TEST_EXPECT_EQ(read_u32_be(m.body.data() + 6), rejected.system_bytes);
    }
}

static void test_encode_frame_rejects_invalid_p_type() {
    Message msg = make_select_req(/*session_id=*/0xFFFF, /*system_bytes=*/0x01020304);
    msg.header.p_type = static_cast<byte>(0xFF);
    std::vector<byte> frame;
    const auto ec = encode_frame(msg, frame);
    TEST_EXPECT_EQ(ec, make_error_code(errc::invalid_argument));
}

static void test_encode_frame_rejects_oversized_body() {
    const std::size_t max_body =
        static_cast<std::size_t>(kMaxPayloadSize) - static_cast<std::size_t>(kHeaderSize);
    std::vector<byte> body(max_body + 1U, static_cast<byte>(0xAB));
    const auto msg = make_data_message(/*session_id=*/0x0001,
                                       /*stream=*/1,
                                       /*function=*/1,
                                       /*w_bit=*/false,
                                       /*system_bytes=*/0x11223344,
                                       bytes_view{body.data(), body.size()});
    std::vector<byte> frame;
    const auto ec = encode_frame(msg, frame);
    TEST_EXPECT_EQ(ec, make_error_code(errc::buffer_overflow));
}

static void test_decode_payload_rejects_too_small() {
    std::vector<byte> payload(static_cast<std::size_t>(kHeaderSize) - 1U,
                              static_cast<byte>(0));
    Message out{};
    const auto ec = decode_payload(bytes_view{payload.data(), payload.size()}, out);
    TEST_EXPECT_EQ(ec, make_error_code(errc::invalid_argument));
}

static void test_decode_payload_rejects_invalid_p_type() {
    const auto msg = make_data_message(/*session_id=*/0x0001,
                                       /*stream=*/1,
                                       /*function=*/1,
                                       /*w_bit=*/false,
                                       /*system_bytes=*/0x11223344,
                                       bytes_view{});
    std::vector<byte> frame;
    TEST_EXPECT_OK(encode_frame(msg, frame));

    // payload = header + body（跳过前 4B 长度字段）
    std::vector<byte> payload(frame.begin() + static_cast<std::ptrdiff_t>(kLengthFieldSize),
                              frame.end());
    payload[4] = static_cast<byte>(0xFF); // p_type byte

    Message out{};
    const auto ec = decode_payload(bytes_view{payload.data(), payload.size()}, out);
    TEST_EXPECT_EQ(ec, make_error_code(errc::invalid_argument));
}

static void test_decode_frame_length_errors() {
    // frame 太短（<4）
    {
        const std::vector<byte> frame = {static_cast<byte>(0x00),
                                         static_cast<byte>(0x00),
                                         static_cast<byte>(0x00)};
        Message out{};
        std::size_t consumed = 123;
        const auto ec = decode_frame(bytes_view{frame.data(), frame.size()}, out, consumed);
        TEST_EXPECT_EQ(ec, make_error_code(errc::invalid_argument));
        TEST_EXPECT_EQ(consumed, 0U);
    }

    // payload_len < header_size
    {
        std::vector<byte> frame(static_cast<std::size_t>(kLengthFieldSize),
                                static_cast<byte>(0));
        write_u32_be(frame.data(), 0U);
        Message out{};
        std::size_t consumed = 123;
        const auto ec = decode_frame(bytes_view{frame.data(), frame.size()}, out, consumed);
        TEST_EXPECT_EQ(ec, make_error_code(errc::invalid_argument));
        TEST_EXPECT_EQ(consumed, 0U);
    }

    // payload_len > max
    {
        std::vector<byte> frame(static_cast<std::size_t>(kLengthFieldSize),
                                static_cast<byte>(0));
        write_u32_be(frame.data(), static_cast<std::uint32_t>(kMaxPayloadSize) + 1U);
        Message out{};
        std::size_t consumed = 123;
        const auto ec = decode_frame(bytes_view{frame.data(), frame.size()}, out, consumed);
        TEST_EXPECT_EQ(ec, make_error_code(errc::buffer_overflow));
        TEST_EXPECT_EQ(consumed, 0U);
    }
}

static void test_decode_frame_truncated_and_consumed() {
    const auto msg = make_data_message(/*session_id=*/0x0001,
                                       /*stream=*/1,
                                       /*function=*/2,
                                       /*w_bit=*/false,
                                       /*system_bytes=*/0x11223344,
                                       bytes_view{});
    std::vector<byte> frame;
    TEST_EXPECT_OK(encode_frame(msg, frame));

    // 截断：保留 length 字段，但丢掉尾部，必须报错。
    {
        std::vector<byte> truncated = frame;
        truncated.pop_back();
        Message out{};
        std::size_t consumed = 0;
        const auto ec =
            decode_frame(bytes_view{truncated.data(), truncated.size()}, out, consumed);
        TEST_EXPECT_EQ(ec, make_error_code(errc::invalid_argument));
        TEST_EXPECT_EQ(consumed, 0U);
    }

    // 多帧缓冲：末尾追加垃圾字节，consumed 应只吃掉第一帧长度。
    {
        std::vector<byte> buf = frame;
        buf.push_back(static_cast<byte>(0xAA));
        buf.push_back(static_cast<byte>(0xBB));

        Message out{};
        std::size_t consumed = 0;
        const auto ec = decode_frame(bytes_view{buf.data(), buf.size()}, out, consumed);
        TEST_EXPECT_OK(ec);
        TEST_EXPECT(consumed > 0U);
        TEST_EXPECT(consumed < buf.size());
        TEST_EXPECT_EQ(out.header.p_type, kPTypeSecs2);
        TEST_EXPECT_EQ(out.header.s_type, SType::data);
        TEST_EXPECT_EQ(out.header.system_bytes, 0x11223344U);
        TEST_EXPECT_EQ(out.stream(), 1U);
        TEST_EXPECT_EQ(out.function(), 2U);
    }
}

} // namespace

int main() {
    test_message_inline_accessors();
    test_make_control_messages();
    test_encode_frame_rejects_invalid_p_type();
    test_encode_frame_rejects_oversized_body();
    test_decode_payload_rejects_too_small();
    test_decode_payload_rejects_invalid_p_type();
    test_decode_frame_length_errors();
    test_decode_frame_truncated_and_consumed();
    return ::secs::tests::run_and_report();
}
