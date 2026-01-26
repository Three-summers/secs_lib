#include "secs/secs1/block.hpp"

#include "secs/core/error.hpp"

#include "test_main.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

namespace {

using secs::core::byte;
using secs::core::bytes_view;
using secs::core::errc;
using secs::core::make_error_code;
using secs::secs1::DecodedBlock;
using secs::secs1::Header;

Header sample_header() {
    Header h{};
    h.reverse_bit = true;
    h.device_id = 0x1234;
    h.wait_bit = false;
    h.stream = 1;
    h.function = 2;
    h.end_bit = true;
    h.block_number = 1;
    h.system_bytes = 0x01020304;
    return h;
}

void rewrite_checksum(std::vector<byte> &frame) {
    TEST_EXPECT(frame.size() >= 1 + secs::secs1::kHeaderSize + 2);

    const auto length = static_cast<std::size_t>(frame[0]);
    TEST_EXPECT(length >= secs::secs1::kHeaderSize);
    TEST_EXPECT(frame.size() == 1 + length + 2);

    const auto cs =
        secs::secs1::checksum(bytes_view{frame.data() + 1, length});
    frame[1 + length] = static_cast<byte>((cs >> 8) & 0xFF);
    frame[1 + length + 1] = static_cast<byte>(cs & 0xFF);
}

void test_checksum_wraps_mod_65536() {
    // 255 * 300 = 76500 = 0x12AD4 -> mod 0x10000 == 0x2AD4
    std::array<byte, 300> buf{};
    buf.fill(static_cast<byte>(0xFF));
    TEST_EXPECT_EQ(secs::secs1::checksum(bytes_view{buf.data(), buf.size()}),
                   static_cast<std::uint16_t>(0x2AD4));
}

void test_encode_rejects_block_number_zero() {
    auto h = sample_header();
    h.block_number = 0;

    std::vector<byte> out;
    const auto ec = secs::secs1::encode_block(h, bytes_view{}, out);
    TEST_EXPECT_EQ(ec, make_error_code(errc::invalid_argument));
}

void test_decode_rejects_block_number_zero() {
    auto h = sample_header();
    h.block_number = 1;

    std::vector<byte> frame;
    TEST_EXPECT_OK(secs::secs1::encode_block(h, bytes_view{}, frame));

    // 构造 block_number=0（Byte5/6），并更新 checksum。
    TEST_EXPECT(frame.size() >= 1 + secs::secs1::kHeaderSize + 2);
    frame[1 + 4] &= static_cast<byte>(0x80); // 只保留 end_bit
    frame[1 + 5] = 0x00;
    rewrite_checksum(frame);

    DecodedBlock decoded{};
    const auto ec = secs::secs1::decode_block(
        bytes_view{frame.data(), frame.size()}, decoded);
    TEST_EXPECT_EQ(ec,
                   secs::secs1::make_error_code(secs::secs1::errc::invalid_block));
}

void test_fragment_message_max_blocks_boundary_sampling() {
    // 边界：payload_size = 244 * 0x7FFF（32767 blocks）
    const std::size_t max_blocks = 0x7FFFu;
    const std::size_t ok_size = secs::secs1::kMaxBlockDataSize * max_blocks;

    // 复用同一份 payload：最后 +1 用于“超过最大块数”的用例。
    std::vector<byte> payload(ok_size + 1);
    for (std::size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<byte>(i & 0xFF);
    }

    auto h = sample_header();
    auto frames =
        secs::secs1::fragment_message(h, bytes_view{payload.data(), ok_size});
    TEST_EXPECT_EQ(frames.size(), max_blocks);

    const std::array<std::size_t, 3> sample_indices = {
        0u,
        12345u,
        max_blocks - 1,
    };

    for (const auto idx : sample_indices) {
        DecodedBlock decoded{};
        TEST_EXPECT_OK(secs::secs1::decode_block(
            bytes_view{frames[idx].data(), frames[idx].size()}, decoded));

        TEST_EXPECT_EQ(decoded.header.device_id, h.device_id);
        TEST_EXPECT_EQ(decoded.header.stream, h.stream);
        TEST_EXPECT_EQ(decoded.header.function, h.function);
        TEST_EXPECT_EQ(decoded.header.system_bytes, h.system_bytes);
        TEST_EXPECT_EQ(decoded.header.block_number,
                       static_cast<std::uint16_t>(idx + 1));
        TEST_EXPECT_EQ(decoded.header.end_bit, idx + 1 == max_blocks);
        TEST_EXPECT_EQ(decoded.data.size(), secs::secs1::kMaxBlockDataSize);

        const std::size_t offset = idx * secs::secs1::kMaxBlockDataSize;
        TEST_EXPECT(std::equal(decoded.data.begin(),
                               decoded.data.end(),
                               payload.begin() + static_cast<std::ptrdiff_t>(offset)));
    }

    // 超过最大块数：payload_size = 244 * 0x7FFF + 1 -> blocks=0x8000，应返回空。
    auto too_many =
        secs::secs1::fragment_message(h, bytes_view{payload.data(), payload.size()});
    TEST_EXPECT(too_many.empty());
}

void test_reassembler_reset_allows_reuse() {
    auto h = sample_header();
    h.end_bit = false;

    std::array<byte, secs::secs1::kMaxBlockDataSize + 10> payload{};
    for (std::size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<byte>(i & 0xFF);
    }

    auto frames =
        secs::secs1::fragment_message(h, bytes_view{payload.data(), payload.size()});
    TEST_EXPECT_EQ(frames.size(), 2u);

    secs::secs1::Reassembler re(h.device_id);
    for (const auto &f : frames) {
        DecodedBlock decoded{};
        TEST_EXPECT_OK(secs::secs1::decode_block(
            bytes_view{f.data(), f.size()}, decoded));
        TEST_EXPECT_OK(re.accept(decoded));
    }
    TEST_EXPECT(re.has_message());
    TEST_EXPECT_EQ(re.message_body().size(), payload.size());

    // reset 后允许重用。
    re.reset();
    auto h2 = h;
    h2.system_bytes = static_cast<std::uint32_t>(h.system_bytes + 1);
    h2.end_bit = true;
    h2.block_number = 1;

    std::vector<byte> f2;
    TEST_EXPECT_OK(secs::secs1::encode_block(h2, bytes_view{}, f2));
    DecodedBlock d2{};
    TEST_EXPECT_OK(
        secs::secs1::decode_block(bytes_view{f2.data(), f2.size()}, d2));
    TEST_EXPECT_OK(re.accept(d2));
    TEST_EXPECT(re.has_message());
    TEST_EXPECT_EQ(re.message_header().system_bytes, h2.system_bytes);
    TEST_EXPECT(re.message_body().empty());
}

void test_reassembler_requires_reset_between_messages() {
    auto h = sample_header();
    h.end_bit = true;
    h.block_number = 1;

    std::vector<byte> f1;
    TEST_EXPECT_OK(secs::secs1::encode_block(h, bytes_view{}, f1));
    DecodedBlock d1{};
    TEST_EXPECT_OK(
        secs::secs1::decode_block(bytes_view{f1.data(), f1.size()}, d1));

    secs::secs1::Reassembler re(h.device_id);
    TEST_EXPECT_OK(re.accept(d1));
    TEST_EXPECT(re.has_message());

    // 未 reset 的情况下，下一条消息的 block_number=1 会被视为序列错误。
    auto h2 = h;
    h2.system_bytes = static_cast<std::uint32_t>(h.system_bytes + 1);
    std::vector<byte> f2;
    TEST_EXPECT_OK(secs::secs1::encode_block(h2, bytes_view{}, f2));
    DecodedBlock d2{};
    TEST_EXPECT_OK(
        secs::secs1::decode_block(bytes_view{f2.data(), f2.size()}, d2));

    const auto ec = re.accept(d2);
    // 新消息的 system_bytes 不同：在未 reset 的情况下会触发“同一条消息内部字段不一致”。
    TEST_EXPECT_EQ(
        ec,
        secs::secs1::make_error_code(secs::secs1::errc::protocol_error));
}

void test_decode_block_random_frames_invariants() {
    // 伪随机坏输入：验证 decode 不崩溃，且在成功时满足基本不变量。
    std::mt19937 rng(0x5EC51u);
    std::uniform_int_distribution<int> size_dist(
        0, static_cast<int>(secs::secs1::kMaxBlockFrameSize * 2));
    std::uniform_int_distribution<int> byte_dist(0, 255);

    for (int i = 0; i < 5000; ++i) {
        const int n = size_dist(rng);
        std::vector<byte> frame(static_cast<std::size_t>(n));
        for (auto &b : frame) {
            b = static_cast<byte>(byte_dist(rng));
        }

        DecodedBlock decoded{};
        const auto ec = secs::secs1::decode_block(
            bytes_view{frame.data(), frame.size()}, decoded);
        if (ec) {
            continue;
        }

        TEST_EXPECT(frame.size() <= secs::secs1::kMaxBlockFrameSize);
        TEST_EXPECT(frame.size() >= 1 + secs::secs1::kHeaderSize + 2);

        const auto length = static_cast<std::size_t>(frame[0]);
        TEST_EXPECT(length >= secs::secs1::kHeaderSize);
        TEST_EXPECT(length <= secs::secs1::kMaxBlockLength);
        TEST_EXPECT_EQ(frame.size(), 1 + length + 2);

        TEST_EXPECT(decoded.header.block_number >= 1);
        TEST_EXPECT(decoded.header.block_number <= 0x7FFF);
        TEST_EXPECT_EQ(decoded.data.size(), length - secs::secs1::kHeaderSize);
    }
}

} // namespace

int main() {
    test_checksum_wraps_mod_65536();
    test_encode_rejects_block_number_zero();
    test_decode_rejects_block_number_zero();
    test_fragment_message_max_blocks_boundary_sampling();
    test_reassembler_reset_allows_reuse();
    test_reassembler_requires_reset_between_messages();
    test_decode_block_random_frames_invariants();
    return ::secs::tests::run_and_report();
}
