/**
 * @file test_hsms_codec_fuzz.cpp
 * @brief HSMS 编解码：确定性 fuzz + 差分（encode/decode 对拍）
 */

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
using secs::hsms::SType;
using secs::hsms::decode_frame;
using secs::hsms::decode_payload;
using secs::hsms::encode_frame;
using secs::hsms::kHeaderSize;
using secs::hsms::kLengthFieldSize;
using secs::hsms::kPTypeSecs2;

struct Lcg final {
    std::uint32_t x{0x12345678u};

    [[nodiscard]] std::uint32_t next_u32() noexcept {
        x = x * 1664525u + 1013904223u;
        return x;
    }

    [[nodiscard]] byte next_byte() noexcept {
        return static_cast<byte>(next_u32() & 0xFFu);
    }
};

[[nodiscard]] SType random_stype(Lcg &rng) noexcept {
    // 覆盖常见控制消息 + data。
    switch (rng.next_u32() % 9u) {
    case 0:
        return SType::data;
    case 1:
        return SType::select_req;
    case 2:
        return SType::select_rsp;
    case 3:
        return SType::deselect_req;
    case 4:
        return SType::deselect_rsp;
    case 5:
        return SType::linktest_req;
    case 6:
        return SType::linktest_rsp;
    case 7:
        return SType::reject_req;
    default:
        return SType::separate_req;
    }
}

[[nodiscard]] Message random_message(Lcg &rng) {
    Message msg{};
    msg.header.session_id = static_cast<std::uint16_t>(rng.next_u32() & 0xFFFFu);
    msg.header.header_byte2 = static_cast<std::uint8_t>(rng.next_u32() & 0xFFu);
    msg.header.header_byte3 = static_cast<std::uint8_t>(rng.next_u32() & 0xFFu);
    msg.header.p_type = kPTypeSecs2;
    msg.header.s_type = random_stype(rng);
    msg.header.system_bytes = rng.next_u32();

    const std::size_t n = static_cast<std::size_t>(rng.next_u32() % 512u);
    msg.body.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        msg.body[i] = rng.next_byte();
    }
    return msg;
}

void test_encode_decode_roundtrip_deterministic() {
    Lcg rng{};

    for (std::size_t i = 0; i < 5000u; ++i) {
        const auto msg = random_message(rng);

        std::vector<byte> frame;
        const auto enc = encode_frame(msg, frame);
        TEST_EXPECT_OK(enc);
        TEST_EXPECT(frame.size() >= kLengthFieldSize + kHeaderSize);

        Message decoded{};
        std::size_t consumed = 123;
        const auto dec =
            decode_frame(bytes_view{frame.data(), frame.size()}, decoded, consumed);
        TEST_EXPECT_OK(dec);
        TEST_EXPECT_EQ(consumed, frame.size());

        TEST_EXPECT_EQ(decoded.header.session_id, msg.header.session_id);
        TEST_EXPECT_EQ(decoded.header.header_byte2, msg.header.header_byte2);
        TEST_EXPECT_EQ(decoded.header.header_byte3, msg.header.header_byte3);
        TEST_EXPECT_EQ(decoded.header.p_type, msg.header.p_type);
        TEST_EXPECT_EQ(static_cast<std::uint8_t>(decoded.header.s_type),
                       static_cast<std::uint8_t>(msg.header.s_type));
        TEST_EXPECT_EQ(decoded.header.system_bytes, msg.header.system_bytes);
        TEST_EXPECT_EQ(decoded.body, msg.body);

        // 差分：decode_frame(frame) 与 decode_payload(frame[4:]) 必须一致。
        Message decoded_payload{};
        const auto payload = bytes_view{frame.data() + kLengthFieldSize,
                                        frame.size() - kLengthFieldSize};
        const auto dec2 = decode_payload(payload, decoded_payload);
        TEST_EXPECT_OK(dec2);
        TEST_EXPECT_EQ(decoded_payload.header.session_id, decoded.header.session_id);
        TEST_EXPECT_EQ(decoded_payload.header.header_byte2, decoded.header.header_byte2);
        TEST_EXPECT_EQ(decoded_payload.header.header_byte3, decoded.header.header_byte3);
        TEST_EXPECT_EQ(decoded_payload.header.p_type, decoded.header.p_type);
        TEST_EXPECT_EQ(static_cast<std::uint8_t>(decoded_payload.header.s_type),
                       static_cast<std::uint8_t>(decoded.header.s_type));
        TEST_EXPECT_EQ(decoded_payload.header.system_bytes, decoded.header.system_bytes);
        TEST_EXPECT_EQ(decoded_payload.body, decoded.body);

        // 差分：decode 后 re-encode 必须字节级一致（同一实现路径的对拍）。
        std::vector<byte> frame2;
        const auto enc2 = encode_frame(decoded, frame2);
        TEST_EXPECT_OK(enc2);
        TEST_EXPECT_EQ(frame2, frame);
    }
}

void test_decode_payload_deterministic_fuzz_does_not_crash() {
    Lcg rng{};
    for (std::size_t i = 0; i < 20000u; ++i) {
        const std::size_t n = static_cast<std::size_t>(rng.next_u32() % 1024u);
        std::vector<byte> buf(n);
        for (std::size_t j = 0; j < n; ++j) {
            buf[j] = rng.next_byte();
        }

        Message out{};
        const auto ec = decode_payload(bytes_view{buf.data(), buf.size()}, out);
        if (!ec) {
            TEST_EXPECT(buf.size() >= kHeaderSize);
            TEST_EXPECT_EQ(out.body.size(), buf.size() - kHeaderSize);
        } else {
            // decode_payload 无 consumed 参数；这里只要求“不崩溃”。
        }
    }
}

void test_decode_frame_deterministic_fuzz_consumed_contract() {
    Lcg rng{};
    for (std::size_t i = 0; i < 20000u; ++i) {
        const std::size_t n = static_cast<std::size_t>(rng.next_u32() % 1024u);
        std::vector<byte> buf(n);
        for (std::size_t j = 0; j < n; ++j) {
            buf[j] = rng.next_byte();
        }

        Message out{};
        std::size_t consumed = 123;
        const auto ec = decode_frame(bytes_view{buf.data(), buf.size()}, out, consumed);
        if (ec) {
            TEST_EXPECT_EQ(consumed, 0u);
        } else {
            TEST_EXPECT(consumed > 0u);
            TEST_EXPECT(consumed <= buf.size());
        }
    }
}

} // namespace

int main() {
    test_encode_decode_roundtrip_deterministic();
    test_decode_payload_deterministic_fuzz_does_not_crash();
    test_decode_frame_deterministic_fuzz_consumed_contract();
    return ::secs::tests::run_and_report();
}

