#include "secs/secs1/block.hpp"
#include "secs/secs1/link.hpp"
#include "secs/secs1/state_machine.hpp"

#include "secs/core/error.hpp"

#include "test_main.hpp"

#include <asio/as_tuple.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/steady_timer.hpp>
#include <asio/use_awaitable.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <string_view>
#include <vector>

namespace {

using secs::core::byte;
using secs::core::bytes_view;
using secs::core::errc;
using secs::core::make_error_code;
using secs::secs1::DecodedBlock;
using secs::secs1::Header;
using secs::secs1::MemoryLink;
using secs::secs1::Reassembler;
using secs::secs1::StateMachine;
using secs::secs1::Timeouts;
using secs::secs1::Timer;

using namespace std::chrono_literals;

class ScriptedLink final : public secs::secs1::Link {
public:
    explicit ScriptedLink(asio::any_io_executor ex) : ex_(std::move(ex)) {}

    [[nodiscard]] asio::any_io_executor executor() const noexcept override {
        return ex_;
    }

    void push_read_ok(byte b) {
        reads_.push_back(ReadResult{std::error_code{}, b});
    }
    void push_read_error(std::error_code ec) {
        reads_.push_back(ReadResult{ec, byte{0}});
    }

    void push_write_error(std::error_code ec) { write_errors_.push_back(ec); }

    [[nodiscard]] const std::vector<std::vector<byte>> &
    writes() const noexcept {
        return writes_;
    }

    asio::awaitable<std::error_code> async_write(bytes_view data) override {
        writes_.emplace_back(data.begin(), data.end());
        if (!write_errors_.empty()) {
            const auto ec = write_errors_.front();
            write_errors_.pop_front();
            co_return ec;
        }
        co_return std::error_code{};
    }

    asio::awaitable<std::pair<std::error_code, byte>>
    async_read_byte(std::optional<secs::core::duration> timeout) override {
        (void)timeout;
        if (reads_.empty()) {
            co_return std::pair{make_error_code(errc::timeout), byte{0}};
        }
        const auto next = reads_.front();
        reads_.pop_front();
        co_return std::pair{next.ec, next.b};
    }

private:
    struct ReadResult final {
        std::error_code ec{};
        byte b{0};
    };

    asio::any_io_executor ex_{};
    std::deque<ReadResult> reads_{};
    std::deque<std::error_code> write_errors_{};
    std::vector<std::vector<byte>> writes_{};
};

bytes_view as_bytes(std::string_view s) {
    return bytes_view{reinterpret_cast<const byte *>(s.data()), s.size()};
}

std::vector<byte> make_payload(std::size_t n) {
    std::vector<byte> out;
    out.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        out.push_back(static_cast<byte>(i & 0xFF));
    }
    return out;
}

asio::awaitable<std::error_code> write_byte(secs::secs1::Link &link, byte b) {
    byte tmp = b;
    co_return co_await link.async_write(bytes_view{&tmp, 1});
}

asio::awaitable<std::pair<std::error_code, std::vector<byte>>>
read_frame(secs::secs1::Link &link,
           secs::core::duration first_timeout,
           secs::core::duration per_byte_timeout) {
    auto [ec_len, len_b] = co_await link.async_read_byte(first_timeout);
    if (ec_len) {
        co_return std::pair{ec_len, std::vector<byte>{}};
    }

    const auto length = static_cast<std::size_t>(len_b);
    std::vector<byte> frame;
    frame.reserve(1 + length + 2);
    frame.push_back(len_b);

    for (std::size_t i = 0; i < length + 2; ++i) {
        auto [ec, b] = co_await link.async_read_byte(per_byte_timeout);
        if (ec) {
            co_return std::pair{ec, std::vector<byte>{}};
        }
        frame.push_back(b);
    }

    co_return std::pair{std::error_code{}, std::move(frame)};
}

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

void test_checksum_known() {
    std::array<byte, 3> bytes{1, 2, 3};
    TEST_EXPECT_EQ(
        secs::secs1::checksum(bytes_view{bytes.data(), bytes.size()}), 6);
}

void test_secs1_error_category_and_messages() {
    TEST_EXPECT_EQ(std::string_view(secs::secs1::error_category().name()),
                   "secs.secs1");

    TEST_EXPECT_EQ(
        secs::secs1::make_error_code(secs::secs1::errc::invalid_block)
            .message(),
        "invalid block");
    TEST_EXPECT_EQ(
        secs::secs1::make_error_code(secs::secs1::errc::checksum_mismatch)
            .message(),
        "checksum mismatch");
    TEST_EXPECT_EQ(
        secs::secs1::make_error_code(secs::secs1::errc::device_id_mismatch)
            .message(),
        "device id mismatch");
    TEST_EXPECT_EQ(
        secs::secs1::make_error_code(secs::secs1::errc::protocol_error)
            .message(),
        "protocol error");
    TEST_EXPECT_EQ(
        secs::secs1::make_error_code(secs::secs1::errc::too_many_retries)
            .message(),
        "too many retries");
    TEST_EXPECT_EQ(
        secs::secs1::make_error_code(secs::secs1::errc::block_sequence_error)
            .message(),
        "block sequence error");

    std::error_code unknown(9999, secs::secs1::error_category());
    TEST_EXPECT_EQ(unknown.message(), "unknown secs.secs1 error");
}

void test_encode_decode_roundtrip_single_block() {
    auto h = sample_header();

    std::vector<byte> frame;
    TEST_EXPECT_OK(secs::secs1::encode_block(h, as_bytes("hello"), frame));

    DecodedBlock decoded{};
    TEST_EXPECT_OK(secs::secs1::decode_block(
        bytes_view{frame.data(), frame.size()}, decoded));

    TEST_EXPECT_EQ(decoded.header.device_id, h.device_id);
    TEST_EXPECT_EQ(decoded.header.reverse_bit, h.reverse_bit);
    TEST_EXPECT_EQ(decoded.header.wait_bit, h.wait_bit);
    TEST_EXPECT_EQ(decoded.header.stream, h.stream);
    TEST_EXPECT_EQ(decoded.header.function, h.function);
    TEST_EXPECT_EQ(decoded.header.end_bit, h.end_bit);
    TEST_EXPECT_EQ(decoded.header.block_number, h.block_number);
    TEST_EXPECT_EQ(decoded.header.system_bytes, h.system_bytes);

    TEST_EXPECT_EQ(
        std::string_view(reinterpret_cast<const char *>(decoded.data.data()),
                         decoded.data.size()),
        "hello");
}

void test_encode_invalid_device_id() {
    auto h = sample_header();
    h.device_id = 0xFFFF; // 超出 15 位有效范围

    std::vector<byte> frame;
    auto ec = secs::secs1::encode_block(h, as_bytes("x"), frame);
    TEST_EXPECT_EQ(ec, make_error_code(errc::invalid_argument));
}

void test_encode_device_id_boundary_values() {
    auto h = sample_header();

    // 最大合法值：0x7FFF
    h.device_id = 0x7FFF;
    std::vector<byte> ok_frame;
    TEST_EXPECT_OK(secs::secs1::encode_block(h, as_bytes("x"), ok_frame));

    // 最高位与 reverse_bit 冲突：0x8000 必须被拒绝（避免产生歧义字节流）
    h.device_id = 0x8000;
    std::vector<byte> bad_frame;
    auto ec = secs::secs1::encode_block(h, as_bytes("x"), bad_frame);
    TEST_EXPECT_EQ(ec, make_error_code(errc::invalid_argument));
}

void test_encode_invalid_block_number() {
    auto h = sample_header();
    h.block_number = 0x0100; // 超出 8 位有效范围（对齐 c_dump）

    std::vector<byte> frame;
    auto ec = secs::secs1::encode_block(h, as_bytes("x"), frame);
    TEST_EXPECT_EQ(ec, make_error_code(errc::invalid_argument));
}

void test_encode_data_too_large() {
    auto h = sample_header();

    auto payload = make_payload(secs::secs1::kMaxBlockDataSize + 1);
    std::vector<byte> frame;
    auto ec = secs::secs1::encode_block(
        h, bytes_view{payload.data(), payload.size()}, frame);
    TEST_EXPECT_EQ(ec, make_error_code(errc::invalid_argument));
}

void test_encode_decode_variant_bits_and_block_number_max() {
    Header h{};
    h.reverse_bit = false;
    h.device_id = 0x0001;
    h.wait_bit = true;
    h.stream = 0x7F;
    h.function = 0xFF;
    h.end_bit = false;
    h.block_number = 0x00FF; // 覆盖 block_number 最大值路径（8 位）
    h.system_bytes = 0xAABBCCDD;

    std::vector<byte> frame;
    TEST_EXPECT_OK(secs::secs1::encode_block(h, bytes_view{}, frame));

    DecodedBlock decoded{};
    TEST_EXPECT_OK(secs::secs1::decode_block(
        bytes_view{frame.data(), frame.size()}, decoded));
    TEST_EXPECT_EQ(decoded.header.reverse_bit, h.reverse_bit);
    TEST_EXPECT_EQ(decoded.header.wait_bit, h.wait_bit);
    TEST_EXPECT_EQ(decoded.header.end_bit, h.end_bit);
    TEST_EXPECT_EQ(decoded.header.block_number, h.block_number);
    TEST_EXPECT_EQ(decoded.header.system_bytes, h.system_bytes);
    TEST_EXPECT(decoded.data.empty());
}

void test_decode_checksum_mismatch() {
    auto h = sample_header();

    std::vector<byte> frame;
    TEST_EXPECT_OK(secs::secs1::encode_block(h, as_bytes("hello"), frame));

    // 翻转负载的一个字节，制造校验失败
    TEST_EXPECT(frame.size() > 1 + secs::secs1::kHeaderSize);
    frame[1 + secs::secs1::kHeaderSize] ^= 0xFF;

    DecodedBlock decoded{};
    auto ec = secs::secs1::decode_block(bytes_view{frame.data(), frame.size()},
                                        decoded);
    TEST_EXPECT_EQ(
        ec, secs::secs1::make_error_code(secs::secs1::errc::checksum_mismatch));
}

void test_decode_invalid_too_small() {
    std::vector<byte> frame(1 + secs::secs1::kHeaderSize + 1, 0);
    DecodedBlock decoded{};
    auto ec = secs::secs1::decode_block(bytes_view{frame.data(), frame.size()},
                                        decoded);
    TEST_EXPECT_EQ(
        ec, secs::secs1::make_error_code(secs::secs1::errc::invalid_block));
}

void test_decode_invalid_length_out_of_range() {
    std::vector<byte> frame(1 + secs::secs1::kHeaderSize + 2, 0);
    frame[0] = 254; // 超过 kMaxBlockLength
    DecodedBlock decoded{};
    auto ec = secs::secs1::decode_block(bytes_view{frame.data(), frame.size()},
                                        decoded);
    TEST_EXPECT_EQ(
        ec, secs::secs1::make_error_code(secs::secs1::errc::invalid_block));
}

void test_decode_invalid_size_mismatch() {
    std::vector<byte> frame(1 + secs::secs1::kHeaderSize + 3, 0);
    frame[0] = static_cast<byte>(secs::secs1::kHeaderSize);
    DecodedBlock decoded{};
    auto ec = secs::secs1::decode_block(bytes_view{frame.data(), frame.size()},
                                        decoded);
    TEST_EXPECT_EQ(
        ec, secs::secs1::make_error_code(secs::secs1::errc::invalid_block));
}

void test_decode_block_frame_too_large() {
    // 恶意输入：超长块帧（长度 > 协议允许的最大值）
    std::vector<byte> frame(secs::secs1::kMaxBlockFrameSize * 2, 0);
    frame[0] = static_cast<byte>(secs::secs1::kHeaderSize);

    DecodedBlock decoded{};
    auto ec = secs::secs1::decode_block(bytes_view{frame.data(), frame.size()},
                                        decoded);
    TEST_EXPECT_EQ(
        ec, secs::secs1::make_error_code(secs::secs1::errc::invalid_block));
}

void test_decode_block_frame_size_256_ok() {
    auto h = sample_header();
    auto payload = make_payload(secs::secs1::kMaxBlockDataSize);

    std::vector<byte> frame;
    TEST_EXPECT_OK(secs::secs1::encode_block(
        h, bytes_view{payload.data(), payload.size()}, frame));
    TEST_EXPECT_EQ(frame.size(), secs::secs1::kMaxBlockFrameSize);

    DecodedBlock decoded{};
    TEST_EXPECT_OK(secs::secs1::decode_block(
        bytes_view{frame.data(), frame.size()}, decoded));
    TEST_EXPECT_EQ(decoded.data.size(), payload.size());
}

void test_decode_block_frame_size_255_ok() {
    auto h = sample_header();
    auto payload = make_payload(secs::secs1::kMaxBlockDataSize - 1);

    std::vector<byte> frame;
    TEST_EXPECT_OK(secs::secs1::encode_block(
        h, bytes_view{payload.data(), payload.size()}, frame));
    TEST_EXPECT_EQ(frame.size(), secs::secs1::kMaxBlockFrameSize - 1);

    DecodedBlock decoded{};
    TEST_EXPECT_OK(secs::secs1::decode_block(
        bytes_view{frame.data(), frame.size()}, decoded));
    TEST_EXPECT_EQ(decoded.data.size(), payload.size());
}

void test_fragment_message_splits_and_decodes() {
    auto h = sample_header();
    auto payload = make_payload(secs::secs1::kMaxBlockDataSize * 2 + 10);

    auto frames = secs::secs1::fragment_message(
        h, bytes_view{payload.data(), payload.size()});
    TEST_EXPECT_EQ(frames.size(), 3u);

    std::size_t total = 0;
    std::uint16_t expected_block = 1;

    for (std::size_t i = 0; i < frames.size(); ++i) {
        DecodedBlock decoded{};
        TEST_EXPECT_OK(secs::secs1::decode_block(
            bytes_view{frames[i].data(), frames[i].size()}, decoded));
        TEST_EXPECT_EQ(decoded.header.block_number, expected_block);
        TEST_EXPECT_EQ(decoded.header.system_bytes, h.system_bytes);
        TEST_EXPECT_EQ(decoded.header.end_bit, i + 1 == frames.size());

        total += decoded.data.size();
        ++expected_block;
    }

    TEST_EXPECT_EQ(total, payload.size());
}

void test_fragment_message_empty_payload() {
    auto h = sample_header();
    auto frames = secs::secs1::fragment_message(h, bytes_view{});
    TEST_EXPECT_EQ(frames.size(), 1u);

    DecodedBlock decoded{};
    TEST_EXPECT_OK(secs::secs1::decode_block(
        bytes_view{frames[0].data(), frames[0].size()}, decoded));
    TEST_EXPECT(decoded.data.empty());
    TEST_EXPECT(decoded.header.end_bit);
    TEST_EXPECT_EQ(decoded.header.block_number, 1);
}

void test_reassembler_happy_path() {
    auto h = sample_header();
    auto payload = make_payload(secs::secs1::kMaxBlockDataSize + 1);

    auto frames = secs::secs1::fragment_message(
        h, bytes_view{payload.data(), payload.size()});
    Reassembler re(h.device_id);

    for (const auto &frame : frames) {
        DecodedBlock decoded{};
        TEST_EXPECT_OK(secs::secs1::decode_block(
            bytes_view{frame.data(), frame.size()}, decoded));
        TEST_EXPECT_OK(re.accept(decoded));
    }

    TEST_EXPECT(re.has_message());
    auto out = re.message_body();
    TEST_EXPECT_EQ(out.size(), payload.size());
    TEST_EXPECT(
        std::equal(out.begin(), out.end(), payload.begin(), payload.end()));
}

void test_reassembler_device_id_mismatch() {
    auto h = sample_header();
    h.device_id = 0x0002;

    std::vector<byte> frame;
    TEST_EXPECT_OK(secs::secs1::encode_block(h, as_bytes("x"), frame));

    DecodedBlock decoded{};
    TEST_EXPECT_OK(secs::secs1::decode_block(
        bytes_view{frame.data(), frame.size()}, decoded));

    Reassembler re(0x0001);
    auto ec = re.accept(decoded);
    TEST_EXPECT_EQ(
        ec,
        secs::secs1::make_error_code(secs::secs1::errc::device_id_mismatch));
}

void test_reassembler_sequence_error() {
    auto h = sample_header();
    h.block_number = 2;

    std::vector<byte> frame;
    TEST_EXPECT_OK(secs::secs1::encode_block(h, as_bytes("x"), frame));

    DecodedBlock decoded{};
    TEST_EXPECT_OK(secs::secs1::decode_block(
        bytes_view{frame.data(), frame.size()}, decoded));

    Reassembler re(h.device_id);
    auto ec = re.accept(decoded);
    TEST_EXPECT_EQ(
        ec,
        secs::secs1::make_error_code(secs::secs1::errc::block_sequence_error));
}

void test_reassembler_mismatch_after_first_block() {
    auto base = sample_header();
    base.end_bit = false;
    base.block_number = 1;

    std::vector<byte> frame1;
    TEST_EXPECT_OK(secs::secs1::encode_block(base, as_bytes("a"), frame1));

    DecodedBlock block1{};
    TEST_EXPECT_OK(secs::secs1::decode_block(
        bytes_view{frame1.data(), frame1.size()}, block1));

    // system_bytes 不一致
    {
        Reassembler re(std::nullopt);
        TEST_EXPECT_OK(re.accept(block1));

        auto h2 = base;
        h2.block_number = 2;
        h2.end_bit = true;
        ++h2.system_bytes;

        std::vector<byte> frame2;
        TEST_EXPECT_OK(secs::secs1::encode_block(h2, as_bytes("b"), frame2));
        DecodedBlock block2{};
        TEST_EXPECT_OK(secs::secs1::decode_block(
            bytes_view{frame2.data(), frame2.size()}, block2));

        auto ec = re.accept(block2);
        TEST_EXPECT_EQ(
            ec,
            secs::secs1::make_error_code(secs::secs1::errc::protocol_error));
    }

    // stream/function 不一致
    {
        Reassembler re(std::nullopt);
        TEST_EXPECT_OK(re.accept(block1));

        auto h2 = base;
        h2.block_number = 2;
        h2.end_bit = true;
        ++h2.stream;

        std::vector<byte> frame2;
        TEST_EXPECT_OK(secs::secs1::encode_block(h2, as_bytes("b"), frame2));
        DecodedBlock block2{};
        TEST_EXPECT_OK(secs::secs1::decode_block(
            bytes_view{frame2.data(), frame2.size()}, block2));

        auto ec = re.accept(block2);
        TEST_EXPECT_EQ(
            ec,
            secs::secs1::make_error_code(secs::secs1::errc::protocol_error));
    }

    // reverse_bit/wait_bit 不一致
    {
        Reassembler re(std::nullopt);
        TEST_EXPECT_OK(re.accept(block1));

        auto h2 = base;
        h2.block_number = 2;
        h2.end_bit = true;
        h2.reverse_bit = !h2.reverse_bit;

        std::vector<byte> frame2;
        TEST_EXPECT_OK(secs::secs1::encode_block(h2, as_bytes("b"), frame2));
        DecodedBlock block2{};
        TEST_EXPECT_OK(secs::secs1::decode_block(
            bytes_view{frame2.data(), frame2.size()}, block2));

        auto ec = re.accept(block2);
        TEST_EXPECT_EQ(
            ec,
            secs::secs1::make_error_code(secs::secs1::errc::protocol_error));
    }
    {
        Reassembler re(std::nullopt);
        TEST_EXPECT_OK(re.accept(block1));

        auto h2 = base;
        h2.block_number = 2;
        h2.end_bit = true;
        h2.wait_bit = !h2.wait_bit;

        std::vector<byte> frame2;
        TEST_EXPECT_OK(secs::secs1::encode_block(h2, as_bytes("b"), frame2));
        DecodedBlock block2{};
        TEST_EXPECT_OK(secs::secs1::decode_block(
            bytes_view{frame2.data(), frame2.size()}, block2));

        auto ec = re.accept(block2);
        TEST_EXPECT_EQ(
            ec,
            secs::secs1::make_error_code(secs::secs1::errc::protocol_error));
    }

    // device_id 不一致（不设置 expected_device_id_，走 header_ 内部一致性检查）
    {
        Reassembler re(std::nullopt);
        TEST_EXPECT_OK(re.accept(block1));

        auto h2 = base;
        h2.block_number = 2;
        h2.end_bit = true;
        ++h2.device_id;

        std::vector<byte> frame2;
        TEST_EXPECT_OK(secs::secs1::encode_block(h2, as_bytes("b"), frame2));
        DecodedBlock block2{};
        TEST_EXPECT_OK(secs::secs1::decode_block(
            bytes_view{frame2.data(), frame2.size()}, block2));

        auto ec = re.accept(block2);
        TEST_EXPECT_EQ(ec,
                       secs::secs1::make_error_code(
                           secs::secs1::errc::device_id_mismatch));
    }

    // device_id 不一致（设置 expected_device_id_，走 expected_device_id_ 检查）
    {
        Reassembler re(base.device_id);
        TEST_EXPECT_OK(re.accept(block1));

        auto h2 = base;
        h2.block_number = 2;
        h2.end_bit = true;
        ++h2.device_id;

        std::vector<byte> frame2;
        TEST_EXPECT_OK(secs::secs1::encode_block(h2, as_bytes("b"), frame2));
        DecodedBlock block2{};
        TEST_EXPECT_OK(secs::secs1::decode_block(
            bytes_view{frame2.data(), frame2.size()}, block2));

        auto ec = re.accept(block2);
        TEST_EXPECT_EQ(ec,
                       secs::secs1::make_error_code(
                           secs::secs1::errc::device_id_mismatch));
    }

    // block_number 不连续
    {
        Reassembler re(std::nullopt);
        TEST_EXPECT_OK(re.accept(block1));

        auto h2 = base;
        h2.block_number = 3;
        h2.end_bit = true;

        std::vector<byte> frame2;
        TEST_EXPECT_OK(secs::secs1::encode_block(h2, as_bytes("b"), frame2));
        DecodedBlock block2{};
        TEST_EXPECT_OK(secs::secs1::decode_block(
            bytes_view{frame2.data(), frame2.size()}, block2));

        auto ec = re.accept(block2);
        TEST_EXPECT_EQ(ec,
                       secs::secs1::make_error_code(
                           secs::secs1::errc::block_sequence_error));
    }
}

void test_memory_link_drop_next() {
    asio::io_context ioc;
    auto [a, b] = MemoryLink::create(ioc.get_executor());

    a.drop_next(1);

    std::atomic<bool> done{false};
    asio::steady_timer watchdog(ioc);
    watchdog.expires_after(1s);
    watchdog.async_wait([&](const std::error_code &) {
        TEST_FAIL("watchdog fired");
        ioc.stop();
    });

    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            std::array<byte, 2> out{0x11, 0x22};
            TEST_EXPECT_OK(
                co_await a.async_write(bytes_view{out.data(), out.size()}));

            auto [ec, got] = co_await b.async_read_byte(50ms);
            TEST_EXPECT_OK(ec);
            TEST_EXPECT_EQ(got, 0x22);

            done = true;
            watchdog.cancel();
            ioc.stop();
            co_return;
        },
        asio::detached);

    ioc.run();
    TEST_EXPECT(done.load());
}

void test_memory_link_fixed_delay() {
    asio::io_context ioc;
    auto [a, b] = MemoryLink::create(ioc.get_executor());

    a.set_fixed_delay(2ms);

    std::atomic<bool> done{false};
    asio::steady_timer watchdog(ioc);
    watchdog.expires_after(1s);
    watchdog.async_wait([&](const std::error_code &) {
        TEST_FAIL("watchdog fired");
        ioc.stop();
    });

    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            TEST_EXPECT_OK(co_await write_byte(a, 0x33));
            auto [ec, got] = co_await b.async_read_byte(50ms);
            TEST_EXPECT_OK(ec);
            TEST_EXPECT_EQ(got, 0x33);

            done = true;
            watchdog.cancel();
            ioc.stop();
            co_return;
        },
        asio::detached);

    ioc.run();
    TEST_EXPECT(done.load());
}

void test_memory_link_delay_zero_branch() {
    asio::io_context ioc;
    auto [a, b] = MemoryLink::create(ioc.get_executor());

    a.set_fixed_delay(0ms);

    std::atomic<bool> done{false};
    asio::steady_timer watchdog(ioc);
    watchdog.expires_after(1s);
    watchdog.async_wait([&](const std::error_code &) {
        TEST_FAIL("watchdog fired");
        ioc.stop();
    });

    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            TEST_EXPECT_OK(co_await write_byte(a, 0x44));
            auto [ec, got] = co_await b.async_read_byte(50ms);
            TEST_EXPECT_OK(ec);
            TEST_EXPECT_EQ(got, 0x44);

            done = true;
            watchdog.cancel();
            ioc.stop();
            co_return;
        },
        asio::detached);

    ioc.run();
    TEST_EXPECT(done.load());
}

void test_memory_link_default_endpoint_invalid_argument() {
    asio::io_context ioc;
    MemoryLink::Endpoint ep;
    ep.drop_next(1);
    ep.set_fixed_delay(1ms);

    std::atomic<bool> done{false};
    asio::steady_timer watchdog(ioc);
    watchdog.expires_after(1s);
    watchdog.async_wait([&](const std::error_code &) {
        TEST_FAIL("watchdog fired");
        ioc.stop();
    });

    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            std::array<byte, 1> out{0x11};
            auto ec =
                co_await ep.async_write(bytes_view{out.data(), out.size()});
            TEST_EXPECT_EQ(ec, make_error_code(errc::invalid_argument));

            auto [rec_ec, _] = co_await ep.async_read_byte(10ms);
            TEST_EXPECT_EQ(rec_ec, make_error_code(errc::invalid_argument));

            done = true;
            watchdog.cancel();
            ioc.stop();
            co_return;
        },
        asio::detached);

    ioc.run();
    TEST_EXPECT(done.load());
}

void test_memory_link_all_bytes_dropped_no_event() {
    asio::io_context ioc;
    auto [a, b] = MemoryLink::create(ioc.get_executor());

    a.drop_next(1);

    std::atomic<bool> done{false};
    asio::steady_timer watchdog(ioc);
    watchdog.expires_after(1s);
    watchdog.async_wait([&](const std::error_code &) {
        TEST_FAIL("watchdog fired");
        ioc.stop();
    });

    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            TEST_EXPECT_OK(co_await write_byte(a, 0x55));
            auto [ec, _] = co_await b.async_read_byte(5ms);
            TEST_EXPECT_EQ(ec, make_error_code(errc::timeout));

            done = true;
            watchdog.cancel();
            ioc.stop();
            co_return;
        },
        asio::detached);

    ioc.run();
    TEST_EXPECT(done.load());
}

void test_memory_link_read_timeout_when_no_data() {
    asio::io_context ioc;
    auto [a, b] = MemoryLink::create(ioc.get_executor());
    (void)a;

    std::atomic<bool> done{false};
    asio::steady_timer watchdog(ioc);
    watchdog.expires_after(1s);
    watchdog.async_wait([&](const std::error_code &) {
        TEST_FAIL("watchdog fired");
        ioc.stop();
    });

    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            auto [ec, _] = co_await b.async_read_byte(5ms);
            TEST_EXPECT_EQ(ec, make_error_code(errc::timeout));

            done = true;
            watchdog.cancel();
            ioc.stop();
            co_return;
        },
        asio::detached);

    ioc.run();
    TEST_EXPECT(done.load());
}

void test_memory_link_read_two_bytes_paths() {
    asio::io_context ioc;
    auto [a, b] = MemoryLink::create(ioc.get_executor());

    std::atomic<bool> done{false};
    asio::steady_timer watchdog(ioc);
    watchdog.expires_after(1s);
    watchdog.async_wait([&](const std::error_code &) {
        TEST_FAIL("watchdog fired");
        ioc.stop();
    });

    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            std::array<byte, 2> out{0x66, 0x77};
            TEST_EXPECT_OK(
                co_await a.async_write(bytes_view{out.data(), out.size()}));

            auto [ec1, b1] = co_await b.async_read_byte(50ms);
            TEST_EXPECT_OK(ec1);
            TEST_EXPECT_EQ(b1, 0x66);

            auto [ec2, b2] = co_await b.async_read_byte(50ms);
            TEST_EXPECT_OK(ec2);
            TEST_EXPECT_EQ(b2, 0x77);

            done = true;
            watchdog.cancel();
            ioc.stop();
            co_return;
        },
        asio::detached);

    ioc.run();
    TEST_EXPECT(done.load());
}

void test_timer_cancelled() {
    asio::io_context ioc;
    Timer t(ioc.get_executor());

    std::atomic<bool> done{false};
    asio::steady_timer watchdog(ioc);
    watchdog.expires_after(1s);
    watchdog.async_wait([&](const std::error_code &) {
        TEST_FAIL("watchdog fired");
        ioc.stop();
    });

    asio::steady_timer canceller(ioc);
    canceller.expires_after(2ms);
    canceller.async_wait([&](const std::error_code &) { t.cancel(); });

    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            auto ec = co_await t.async_sleep(200ms);
            TEST_EXPECT_EQ(ec, make_error_code(errc::cancelled));
            done = true;
            watchdog.cancel();
            ioc.stop();
            co_return;
        },
        asio::detached);

    ioc.run();
    TEST_EXPECT(done.load());
}

void test_state_machine_send_receive_single_block() {
    asio::io_context ioc;
    auto [a, b] = MemoryLink::create(ioc.get_executor());

    Timeouts timeouts{};
    timeouts.t1_intercharacter = 20ms;
    timeouts.t2_protocol = 50ms;
    timeouts.t3_reply = 100ms;
    timeouts.t4_interblock = 50ms;

    StateMachine sender(a, 0x1234, timeouts);
    StateMachine receiver(b, 0x1234, timeouts);

    auto h = sample_header();
    auto payload = make_payload(10);

    std::atomic<int> done{0};
    asio::steady_timer watchdog(ioc);
    watchdog.expires_after(1s);
    watchdog.async_wait([&](const std::error_code &) {
        TEST_FAIL("watchdog fired");
        ioc.stop();
    });

    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            auto ec = co_await sender.async_send(
                h, bytes_view{payload.data(), payload.size()});
            TEST_EXPECT_OK(ec);
            if (++done == 2) {
                watchdog.cancel();
                ioc.stop();
            }
            co_return;
        },
        asio::detached);

    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            auto [ec, msg] = co_await receiver.async_receive(200ms);
            TEST_EXPECT_OK(ec);
            TEST_EXPECT_EQ(msg.header.device_id, h.device_id);
            TEST_EXPECT_EQ(msg.body.size(), payload.size());
            TEST_EXPECT(std::equal(msg.body.begin(),
                                   msg.body.end(),
                                   payload.begin(),
                                   payload.end()));
            if (++done == 2) {
                watchdog.cancel();
                ioc.stop();
            }
            co_return;
        },
        asio::detached);

    ioc.run();
    TEST_EXPECT_EQ(done.load(), 2);
}

void test_state_machine_send_unexpected_handshake_byte_protocol_error_scripted() {
    asio::io_context ioc;
    ScriptedLink link(ioc.get_executor());

    link.push_read_ok(0x00); // 非 EOT/ACK/NAK

    StateMachine sm(link, std::nullopt, Timeouts{}, 1);
    auto h = sample_header();
    auto payload = make_payload(1);

    std::error_code result;
    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            result = co_await sm.async_send(
                h, bytes_view{payload.data(), payload.size()});
            co_return;
        },
        asio::detached);

    ioc.run();
    TEST_EXPECT_EQ(
        result,
        secs::secs1::make_error_code(secs::secs1::errc::protocol_error));
}

void test_state_machine_send_propagates_handshake_read_error_scripted() {
    asio::io_context ioc;
    ScriptedLink link(ioc.get_executor());
    link.push_read_error(make_error_code(errc::invalid_argument));

    StateMachine sm(link, std::nullopt, Timeouts{}, 1);
    auto h = sample_header();

    std::error_code result;
    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            result = co_await sm.async_send(h, as_bytes("x"));
            co_return;
        },
        asio::detached);

    ioc.run();
    TEST_EXPECT_EQ(result, make_error_code(errc::invalid_argument));
}

void test_state_machine_send_propagates_handshake_write_error_scripted() {
    asio::io_context ioc;
    ScriptedLink link(ioc.get_executor());
    link.push_write_error(make_error_code(errc::invalid_argument));

    StateMachine sm(link, std::nullopt, Timeouts{}, 1);
    auto h = sample_header();

    std::error_code result;
    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            result = co_await sm.async_send(h, as_bytes("x"));
            co_return;
        },
        asio::detached);

    ioc.run();
    TEST_EXPECT_EQ(result, make_error_code(errc::invalid_argument));
}

void test_state_machine_send_accepts_ack_as_handshake_response_scripted() {
    asio::io_context ioc;
    ScriptedLink link(ioc.get_executor());

    link.push_read_ok(secs::secs1::kAck); // ENQ 的响应：ACK
    link.push_read_ok(secs::secs1::kAck); // 块帧的响应：ACK

    StateMachine sm(link, std::nullopt, Timeouts{}, 1);
    auto h = sample_header();

    std::error_code result;
    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            result = co_await sm.async_send(h, as_bytes("x"));
            co_return;
        },
        asio::detached);

    ioc.run();
    TEST_EXPECT_OK(result);
    TEST_EXPECT_EQ(link.writes().size(), 2u); // ENQ + 块帧
    TEST_EXPECT_EQ(link.writes()[0].size(), 1u);
    TEST_EXPECT_EQ(link.writes()[0][0], secs::secs1::kEnq);
}

void test_state_machine_send_unexpected_ack_response_protocol_error_scripted() {
    asio::io_context ioc;
    ScriptedLink link(ioc.get_executor());

    link.push_read_ok(secs::secs1::kEot); // 握手完成
    link.push_read_ok(secs::secs1::kEot); // 非 ACK/NAK

    StateMachine sm(link, std::nullopt, Timeouts{}, 1);
    auto h = sample_header();

    std::error_code result;
    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            result = co_await sm.async_send(h, as_bytes("x"));
            co_return;
        },
        asio::detached);

    ioc.run();
    TEST_EXPECT_EQ(
        result,
        secs::secs1::make_error_code(secs::secs1::errc::protocol_error));
}

void test_state_machine_send_propagates_ack_read_error_scripted() {
    asio::io_context ioc;
    ScriptedLink link(ioc.get_executor());

    link.push_read_ok(secs::secs1::kEot); // 握手完成
    link.push_read_error(make_error_code(errc::cancelled));

    StateMachine sm(link, std::nullopt, Timeouts{}, 1);
    auto h = sample_header();

    std::error_code result;
    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            result = co_await sm.async_send(h, as_bytes("x"));
            co_return;
        },
        asio::detached);

    ioc.run();
    TEST_EXPECT_EQ(result, make_error_code(errc::cancelled));
}

void test_state_machine_send_block_timeout_retries_to_too_many_retries_scripted() {
    asio::io_context ioc;
    ScriptedLink link(ioc.get_executor());

    link.push_read_ok(secs::secs1::kEot); // 握手完成
    // 后续 ACK 读不到 -> ScriptedLink 默认返回 errc::timeout，触发重试

    StateMachine sm(link, std::nullopt, Timeouts{}, 2);
    auto h = sample_header();

    std::error_code result;
    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            result = co_await sm.async_send(h, as_bytes("x"));
            co_return;
        },
        asio::detached);

    ioc.run();
    TEST_EXPECT_EQ(
        result,
        secs::secs1::make_error_code(secs::secs1::errc::too_many_retries));
    TEST_EXPECT_EQ(link.writes().size(),
                   3u); // ENQ + block(第1次) + block(第2次)
}

void test_state_machine_send_when_not_idle_returns_invalid_argument() {
    asio::io_context ioc;
    auto [a, b] = MemoryLink::create(ioc.get_executor());

    // 延迟接收端回包，确保发送端在 wait_eot 状态停留一段时间
    b.set_fixed_delay(20ms);

    Timeouts timeouts{};
    timeouts.t1_intercharacter = 50ms;
    timeouts.t2_protocol = 200ms;
    timeouts.t4_interblock = 50ms;

    StateMachine sender(a, 0x1234, timeouts, 3);
    StateMachine receiver(b, 0x1234, timeouts, 3);

    auto h = sample_header();
    auto payload = make_payload(1);

    std::atomic<int> done{0};
    asio::steady_timer watchdog(ioc);
    watchdog.expires_after(1s);
    watchdog.async_wait([&](const std::error_code &) {
        TEST_FAIL("watchdog fired");
        ioc.stop();
    });

    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            auto ec = co_await sender.async_send(
                h, bytes_view{payload.data(), payload.size()});
            TEST_EXPECT_OK(ec);
            if (++done == 3) {
                watchdog.cancel();
                ioc.stop();
            }
            co_return;
        },
        asio::detached);

    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            asio::steady_timer t(ioc);
            t.expires_after(1ms);
            (void)co_await t.async_wait(asio::as_tuple(asio::use_awaitable));

            auto ec = co_await sender.async_send(
                h, bytes_view{payload.data(), payload.size()});
            TEST_EXPECT_EQ(ec, make_error_code(errc::invalid_argument));
            if (++done == 3) {
                watchdog.cancel();
                ioc.stop();
            }
            co_return;
        },
        asio::detached);

    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            auto [ec, _] = co_await receiver.async_receive(500ms);
            TEST_EXPECT_OK(ec);
            if (++done == 3) {
                watchdog.cancel();
                ioc.stop();
            }
            co_return;
        },
        asio::detached);

    ioc.run();
    TEST_EXPECT_EQ(done.load(), 3);
}

void test_state_machine_receive_when_not_idle_returns_invalid_argument() {
    asio::io_context ioc;
    auto [a, b] = MemoryLink::create(ioc.get_executor());

    Timeouts timeouts{};
    timeouts.t1_intercharacter = 50ms;
    timeouts.t2_protocol = 20ms;

    StateMachine receiver(b, 0x1234, timeouts, 3);

    std::atomic<int> done{0};
    asio::steady_timer watchdog(ioc);
    watchdog.expires_after(1s);
    watchdog.async_wait([&](const std::error_code &) {
        TEST_FAIL("watchdog fired");
        ioc.stop();
    });

    // 触发第 1 次接收进入 wait_block 状态
    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            TEST_EXPECT_OK(co_await write_byte(a, secs::secs1::kEnq));
            auto [ec, ch] = co_await a.async_read_byte(200ms);
            TEST_EXPECT_OK(ec);
            TEST_EXPECT_EQ(ch, secs::secs1::kEot);
            co_return;
        },
        asio::detached);

    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            auto [ec, _] = co_await receiver.async_receive(500ms);
            TEST_EXPECT_EQ(ec,
                           make_error_code(errc::timeout)); // 等不到长度字段
            if (++done == 2) {
                watchdog.cancel();
                ioc.stop();
            }
            co_return;
        },
        asio::detached);

    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            asio::steady_timer t(ioc);
            t.expires_after(2ms);
            (void)co_await t.async_wait(asio::as_tuple(asio::use_awaitable));

            auto [ec, _] = co_await receiver.async_receive(500ms);
            TEST_EXPECT_EQ(ec, make_error_code(errc::invalid_argument));
            if (++done == 2) {
                watchdog.cancel();
                ioc.stop();
            }
            co_return;
        },
        asio::detached);

    ioc.run();
    TEST_EXPECT_EQ(done.load(), 2);
}

void test_state_machine_receive_invalid_length_returns_invalid_block() {
    asio::io_context ioc;
    auto [a, b] = MemoryLink::create(ioc.get_executor());

    Timeouts timeouts{};
    timeouts.t2_protocol = 50ms;
    timeouts.t1_intercharacter = 50ms;

    StateMachine receiver(b, 0x1234, timeouts, 3);

    std::atomic<int> done{0};
    asio::steady_timer watchdog(ioc);
    watchdog.expires_after(1s);
    watchdog.async_wait([&](const std::error_code &) {
        TEST_FAIL("watchdog fired");
        ioc.stop();
    });

    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            TEST_EXPECT_OK(co_await write_byte(a, secs::secs1::kEnq));
            auto [ec0, ch0] = co_await a.async_read_byte(200ms);
            TEST_EXPECT_OK(ec0);
            TEST_EXPECT_EQ(ch0, secs::secs1::kEot);

            // 非法长度（<10）
            TEST_EXPECT_OK(co_await write_byte(a, 0x09));
            auto [ec1, resp] = co_await a.async_read_byte(200ms);
            TEST_EXPECT_OK(ec1);
            TEST_EXPECT_EQ(resp, secs::secs1::kNak);

            if (++done == 2) {
                watchdog.cancel();
                ioc.stop();
            }
            co_return;
        },
        asio::detached);

    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            auto [ec, _] = co_await receiver.async_receive(200ms);
            TEST_EXPECT_EQ(
                ec,
                secs::secs1::make_error_code(secs::secs1::errc::invalid_block));
            if (++done == 2) {
                watchdog.cancel();
                ioc.stop();
            }
            co_return;
        },
        asio::detached);

    ioc.run();
    TEST_EXPECT_EQ(done.load(), 2);
}

void test_state_machine_receive_too_many_bad_blocks() {
    asio::io_context ioc;
    auto [a, b] = MemoryLink::create(ioc.get_executor());

    Timeouts timeouts{};
    timeouts.t1_intercharacter = 50ms;
    timeouts.t2_protocol = 50ms;

    StateMachine receiver(b, 0x1234, timeouts, 2);

    auto h = sample_header();
    std::vector<byte> good;
    TEST_EXPECT_OK(secs::secs1::encode_block(h, as_bytes("hello"), good));

    auto bad = good;
    bad[1 + secs::secs1::kHeaderSize] ^= 0xFF;

    std::atomic<int> done{0};
    asio::steady_timer watchdog(ioc);
    watchdog.expires_after(1s);
    watchdog.async_wait([&](const std::error_code &) {
        TEST_FAIL("watchdog fired");
        ioc.stop();
    });

    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            TEST_EXPECT_OK(co_await write_byte(a, secs::secs1::kEnq));
            auto [ec0, ch0] = co_await a.async_read_byte(200ms);
            TEST_EXPECT_OK(ec0);
            TEST_EXPECT_EQ(ch0, secs::secs1::kEot);

            // 连续两次发送错误块帧，触发 nack_count >= retry_limit
            for (int i = 0; i < 2; ++i) {
                TEST_EXPECT_OK(
                    co_await a.async_write(bytes_view{bad.data(), bad.size()}));
                auto [ec1, resp] = co_await a.async_read_byte(200ms);
                TEST_EXPECT_OK(ec1);
                TEST_EXPECT_EQ(resp, secs::secs1::kNak);
            }

            if (++done == 2) {
                watchdog.cancel();
                ioc.stop();
            }
            co_return;
        },
        asio::detached);

    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            auto [ec, _] = co_await receiver.async_receive(500ms);
            TEST_EXPECT_EQ(ec,
                           secs::secs1::make_error_code(
                               secs::secs1::errc::too_many_retries));
            if (++done == 2) {
                watchdog.cancel();
                ioc.stop();
            }
            co_return;
        },
        asio::detached);

    ioc.run();
    TEST_EXPECT_EQ(done.load(), 2);
}

void test_state_machine_receive_device_id_mismatch() {
    asio::io_context ioc;
    auto [a, b] = MemoryLink::create(ioc.get_executor());

    Timeouts timeouts{};
    timeouts.t1_intercharacter = 50ms;
    timeouts.t2_protocol = 50ms;

    StateMachine receiver(b, 0x1234, timeouts, 3);

    auto h = sample_header();
    ++h.device_id;

    std::vector<byte> frame;
    TEST_EXPECT_OK(secs::secs1::encode_block(h, as_bytes("x"), frame));

    std::atomic<int> done{0};
    asio::steady_timer watchdog(ioc);
    watchdog.expires_after(1s);
    watchdog.async_wait([&](const std::error_code &) {
        TEST_FAIL("watchdog fired");
        ioc.stop();
    });

    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            TEST_EXPECT_OK(co_await write_byte(a, secs::secs1::kEnq));
            auto [ec0, ch0] = co_await a.async_read_byte(200ms);
            TEST_EXPECT_OK(ec0);
            TEST_EXPECT_EQ(ch0, secs::secs1::kEot);

            TEST_EXPECT_OK(
                co_await a.async_write(bytes_view{frame.data(), frame.size()}));
            auto [ec1, resp] = co_await a.async_read_byte(200ms);
            TEST_EXPECT_OK(ec1);
            TEST_EXPECT_EQ(resp, secs::secs1::kNak);

            if (++done == 2) {
                watchdog.cancel();
                ioc.stop();
            }
            co_return;
        },
        asio::detached);

    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            auto [ec, _] = co_await receiver.async_receive(500ms);
            TEST_EXPECT_EQ(ec,
                           secs::secs1::make_error_code(
                               secs::secs1::errc::device_id_mismatch));
            if (++done == 2) {
                watchdog.cancel();
                ioc.stop();
            }
            co_return;
        },
        asio::detached);

    ioc.run();
    TEST_EXPECT_EQ(done.load(), 2);
}

void test_state_machine_receive_block_sequence_error() {
    asio::io_context ioc;
    auto [a, b] = MemoryLink::create(ioc.get_executor());

    Timeouts timeouts{};
    timeouts.t1_intercharacter = 50ms;
    timeouts.t2_protocol = 50ms;
    timeouts.t4_interblock = 50ms;

    StateMachine receiver(b, 0x1234, timeouts, 3);

    auto h1 = sample_header();
    h1.end_bit = false;
    h1.block_number = 1;
    std::vector<byte> frame1;
    TEST_EXPECT_OK(secs::secs1::encode_block(h1, as_bytes("a"), frame1));

    auto h2 = h1;
    h2.end_bit = true;
    h2.block_number = 3; // 跳过 2，触发 block_sequence_error
    std::vector<byte> frame2;
    TEST_EXPECT_OK(secs::secs1::encode_block(h2, as_bytes("b"), frame2));

    std::atomic<int> done{0};
    asio::steady_timer watchdog(ioc);
    watchdog.expires_after(1s);
    watchdog.async_wait([&](const std::error_code &) {
        TEST_FAIL("watchdog fired");
        ioc.stop();
    });

    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            TEST_EXPECT_OK(co_await write_byte(a, secs::secs1::kEnq));
            auto [ec0, ch0] = co_await a.async_read_byte(200ms);
            TEST_EXPECT_OK(ec0);
            TEST_EXPECT_EQ(ch0, secs::secs1::kEot);

            // 第 1 块应被 ACK
            TEST_EXPECT_OK(
                co_await a.async_write(bytes_view{frame1.data(), frame1.size()}));
            auto [ec1, resp1] = co_await a.async_read_byte(200ms);
            TEST_EXPECT_OK(ec1);
            TEST_EXPECT_EQ(resp1, secs::secs1::kAck);

            // 第 2 块序号不连续，应被 NAK
            TEST_EXPECT_OK(
                co_await a.async_write(bytes_view{frame2.data(), frame2.size()}));
            auto [ec2, resp2] = co_await a.async_read_byte(200ms);
            TEST_EXPECT_OK(ec2);
            TEST_EXPECT_EQ(resp2, secs::secs1::kNak);

            if (++done == 2) {
                watchdog.cancel();
                ioc.stop();
            }
            co_return;
        },
        asio::detached);

    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            auto [ec, _] = co_await receiver.async_receive(500ms);
            TEST_EXPECT_EQ(
                ec,
                secs::secs1::make_error_code(secs::secs1::errc::block_sequence_error));
            if (++done == 2) {
                watchdog.cancel();
                ioc.stop();
            }
            co_return;
        },
        asio::detached);

    ioc.run();
    TEST_EXPECT_EQ(done.load(), 2);
}

void test_state_machine_receive_ignores_noise_before_enq() {
    asio::io_context ioc;
    auto [a, b] = MemoryLink::create(ioc.get_executor());

    Timeouts timeouts{};
    timeouts.t1_intercharacter = 50ms;
    timeouts.t2_protocol = 50ms;

    StateMachine receiver(b, 0x1234, timeouts, 3);
    auto h = sample_header();
    auto payload = make_payload(3);

    std::atomic<int> done{0};
    asio::steady_timer watchdog(ioc);
    watchdog.expires_after(1s);
    watchdog.async_wait([&](const std::error_code &) {
        TEST_FAIL("watchdog fired");
        ioc.stop();
    });

    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            std::array<byte, 2> noise{0x00, 0x01};
            TEST_EXPECT_OK(
                co_await a.async_write(bytes_view{noise.data(), noise.size()}));

            TEST_EXPECT_OK(co_await write_byte(a, secs::secs1::kEnq));
            auto [ec0, ch0] = co_await a.async_read_byte(200ms);
            TEST_EXPECT_OK(ec0);
            TEST_EXPECT_EQ(ch0, secs::secs1::kEot);

            auto frames = secs::secs1::fragment_message(
                h, bytes_view{payload.data(), payload.size()});
            TEST_EXPECT_EQ(frames.size(), 1u);
            TEST_EXPECT_OK(co_await a.async_write(
                bytes_view{frames[0].data(), frames[0].size()}));
            auto [ec1, resp] = co_await a.async_read_byte(200ms);
            TEST_EXPECT_OK(ec1);
            TEST_EXPECT_EQ(resp, secs::secs1::kAck);

            if (++done == 2) {
                watchdog.cancel();
                ioc.stop();
            }
            co_return;
        },
        asio::detached);

    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            auto [ec, msg] = co_await receiver.async_receive(500ms);
            TEST_EXPECT_OK(ec);
            TEST_EXPECT_EQ(msg.body.size(), payload.size());
            if (++done == 2) {
                watchdog.cancel();
                ioc.stop();
            }
            co_return;
        },
        asio::detached);

    ioc.run();
    TEST_EXPECT_EQ(done.load(), 2);
}

void test_sender_retries_handshake_on_nak_then_ok() {
    asio::io_context ioc;
    auto [a, b] = MemoryLink::create(ioc.get_executor());

    Timeouts timeouts{};
    timeouts.t2_protocol = 20ms;

    StateMachine sender(a, std::nullopt, timeouts, 3);

    auto h = sample_header();
    auto payload = make_payload(5);

    std::atomic<bool> done{false};
    std::atomic<int> enq_seen{0};

    asio::steady_timer watchdog(ioc);
    watchdog.expires_after(1s);
    watchdog.async_wait([&](const std::error_code &) {
        TEST_FAIL("watchdog fired");
        ioc.stop();
    });

    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            // 手工接收端：第一次 ENQ 回复 NAK，第二次回复 EOT，然后读块帧并 ACK
            for (;;) {
                auto [ec, ch] = co_await b.async_read_byte(200ms);
                TEST_EXPECT_OK(ec);
                if (ch == secs::secs1::kEnq) {
                    break;
                }
            }
            ++enq_seen;
            TEST_EXPECT_OK(co_await write_byte(b, secs::secs1::kNak));

            for (;;) {
                auto [ec, ch] = co_await b.async_read_byte(200ms);
                TEST_EXPECT_OK(ec);
                if (ch == secs::secs1::kEnq) {
                    break;
                }
            }
            ++enq_seen;
            TEST_EXPECT_OK(co_await write_byte(b, secs::secs1::kEot));

            auto [frame_ec, frame] = co_await read_frame(b, 200ms, 200ms);
            TEST_EXPECT_OK(frame_ec);

            DecodedBlock decoded{};
            TEST_EXPECT_OK(secs::secs1::decode_block(
                bytes_view{frame.data(), frame.size()}, decoded));
            TEST_EXPECT_OK(co_await write_byte(b, secs::secs1::kAck));

            co_return;
        },
        asio::detached);

    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            auto ec = co_await sender.async_send(
                h, bytes_view{payload.data(), payload.size()});
            TEST_EXPECT_OK(ec);
            done = true;
            watchdog.cancel();
            ioc.stop();
            co_return;
        },
        asio::detached);

    ioc.run();
    TEST_EXPECT(done.load());
    TEST_EXPECT_EQ(enq_seen.load(), 2);
}

void test_sender_retries_block_on_nak() {
    asio::io_context ioc;
    auto [a, b] = MemoryLink::create(ioc.get_executor());

    Timeouts timeouts{};
    timeouts.t2_protocol = 50ms;

    StateMachine sender(a, std::nullopt, timeouts, 3);
    auto h = sample_header();
    auto payload = make_payload(10);

    std::vector<byte> first;
    std::vector<byte> second;

    std::atomic<bool> done{false};
    asio::steady_timer watchdog(ioc);
    watchdog.expires_after(1s);
    watchdog.async_wait([&](const std::error_code &) {
        TEST_FAIL("watchdog fired");
        ioc.stop();
    });

    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            // 握手：ENQ -> EOT
            for (;;) {
                auto [ec, ch] = co_await b.async_read_byte(200ms);
                TEST_EXPECT_OK(ec);
                if (ch == secs::secs1::kEnq) {
                    break;
                }
            }
            TEST_EXPECT_OK(co_await write_byte(b, secs::secs1::kEot));

            // 第一次块帧：回复 NAK
            auto [ec1, frame1] = co_await read_frame(b, 200ms, 200ms);
            TEST_EXPECT_OK(ec1);
            first = frame1;
            TEST_EXPECT_OK(co_await write_byte(b, secs::secs1::kNak));

            // 第二次块帧：回复 ACK
            auto [ec2, frame2] = co_await read_frame(b, 200ms, 200ms);
            TEST_EXPECT_OK(ec2);
            second = frame2;
            TEST_EXPECT_OK(co_await write_byte(b, secs::secs1::kAck));
            co_return;
        },
        asio::detached);

    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            auto ec = co_await sender.async_send(
                h, bytes_view{payload.data(), payload.size()});
            TEST_EXPECT_OK(ec);
            done = true;
            watchdog.cancel();
            ioc.stop();
            co_return;
        },
        asio::detached);

    ioc.run();
    TEST_EXPECT(done.load());
    TEST_EXPECT(!first.empty());
    TEST_EXPECT_EQ(first, second);
}

void test_receiver_nak_then_accept_on_checksum_error() {
    asio::io_context ioc;
    auto [a, b] = MemoryLink::create(ioc.get_executor());

    Timeouts timeouts{};
    timeouts.t1_intercharacter = 50ms;
    timeouts.t2_protocol = 100ms;

    StateMachine receiver(b, 0x1234, timeouts, 3);
    auto h = sample_header();
    auto payload = make_payload(20);

    std::vector<byte> bad;
    std::vector<byte> good;
    TEST_EXPECT_OK(secs::secs1::encode_block(
        h, bytes_view{payload.data(), payload.size()}, good));
    bad = good;
    bad[1 + secs::secs1::kHeaderSize] ^= 0xFF;

    std::atomic<bool> done{false};
    asio::steady_timer watchdog(ioc);
    watchdog.expires_after(1s);
    watchdog.async_wait([&](const std::error_code &) {
        TEST_FAIL("watchdog fired");
        ioc.stop();
    });

    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            // 手工发送端：ENQ -> 等待 EOT -> 发坏块帧 -> 等待 NAK -> 发好块帧
            // -> 等待 ACK
            TEST_EXPECT_OK(co_await write_byte(a, secs::secs1::kEnq));
            auto [ec0, ch0] = co_await a.async_read_byte(200ms);
            TEST_EXPECT_OK(ec0);
            TEST_EXPECT_EQ(ch0, secs::secs1::kEot);

            TEST_EXPECT_OK(
                co_await a.async_write(bytes_view{bad.data(), bad.size()}));
            auto [ec1, resp1] = co_await a.async_read_byte(200ms);
            TEST_EXPECT_OK(ec1);
            TEST_EXPECT_EQ(resp1, secs::secs1::kNak);

            TEST_EXPECT_OK(
                co_await a.async_write(bytes_view{good.data(), good.size()}));
            auto [ec2, resp2] = co_await a.async_read_byte(200ms);
            TEST_EXPECT_OK(ec2);
            TEST_EXPECT_EQ(resp2, secs::secs1::kAck);

            (void)co_await write_byte(a, secs::secs1::kEot);
            co_return;
        },
        asio::detached);

    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            auto [ec, msg] = co_await receiver.async_receive(500ms);
            TEST_EXPECT_OK(ec);
            TEST_EXPECT_EQ(msg.body.size(), payload.size());
            TEST_EXPECT(std::equal(msg.body.begin(),
                                   msg.body.end(),
                                   payload.begin(),
                                   payload.end()));
            done = true;
            watchdog.cancel();
            ioc.stop();
            co_return;
        },
        asio::detached);

    ioc.run();
    TEST_EXPECT(done.load());
}

void test_t1_intercharacter_timeout() {
    asio::io_context ioc;
    auto [a, b] = MemoryLink::create(ioc.get_executor());

    Timeouts timeouts{};
    timeouts.t1_intercharacter = 5ms;
    timeouts.t2_protocol = 100ms;

    StateMachine receiver(b, 0x1234, timeouts, 3);
    auto h = sample_header();

    std::vector<byte> frame;
    TEST_EXPECT_OK(secs::secs1::encode_block(h, as_bytes("hello"), frame));

    std::atomic<bool> done{false};
    asio::steady_timer watchdog(ioc);
    watchdog.expires_after(1s);
    watchdog.async_wait([&](const std::error_code &) {
        TEST_FAIL("watchdog fired");
        ioc.stop();
    });

    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            // 握手：ENQ -> EOT
            TEST_EXPECT_OK(co_await write_byte(a, secs::secs1::kEnq));
            auto [ec0, ch0] = co_await a.async_read_byte(200ms);
            TEST_EXPECT_OK(ec0);
            TEST_EXPECT_EQ(ch0, secs::secs1::kEot);

            // 只发长度字段 + 1 字节，然后等待超过 T1
            TEST_EXPECT_OK(co_await a.async_write(bytes_view{frame.data(), 2}));

            asio::steady_timer t(ioc);
            t.expires_after(20ms);
            (void)co_await t.async_wait(asio::as_tuple(asio::use_awaitable));

            // 发剩余字节（此时接收侧应已超时退出）
            if (frame.size() > 2) {
                (void)co_await a.async_write(
                    bytes_view{frame.data() + 2, frame.size() - 2});
            }
            co_return;
        },
        asio::detached);

    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            auto [ec, _] = co_await receiver.async_receive(500ms);
            TEST_EXPECT_EQ(ec, make_error_code(errc::timeout));
            done = true;
            watchdog.cancel();
            ioc.stop();
            co_return;
        },
        asio::detached);

    ioc.run();
    TEST_EXPECT(done.load());
}

void test_t2_handshake_timeout_to_too_many_retries() {
    asio::io_context ioc;
    auto [a, b] = MemoryLink::create(ioc.get_executor());
    (void)b;

    Timeouts timeouts{};
    timeouts.t2_protocol = 5ms;

    StateMachine sender(a, std::nullopt, timeouts, 2);
    auto h = sample_header();

    std::atomic<bool> done{false};
    asio::steady_timer watchdog(ioc);
    watchdog.expires_after(1s);
    watchdog.async_wait([&](const std::error_code &) {
        TEST_FAIL("watchdog fired");
        ioc.stop();
    });

    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            auto ec = co_await sender.async_send(h, as_bytes("hi"));
            TEST_EXPECT_EQ(ec,
                           secs::secs1::make_error_code(
                               secs::secs1::errc::too_many_retries));
            done = true;
            watchdog.cancel();
            ioc.stop();
            co_return;
        },
        asio::detached);

    ioc.run();
    TEST_EXPECT(done.load());
}

void test_t4_interblock_timeout() {
    asio::io_context ioc;
    auto [a, b] = MemoryLink::create(ioc.get_executor());

    Timeouts timeouts{};
    timeouts.t1_intercharacter = 50ms;
    timeouts.t2_protocol = 50ms;
    timeouts.t4_interblock = 5ms;

    StateMachine receiver(b, 0x1234, timeouts, 3);
    auto h = sample_header();

    auto payload = make_payload(secs::secs1::kMaxBlockDataSize + 1);
    auto frames = secs::secs1::fragment_message(
        h, bytes_view{payload.data(), payload.size()});
    TEST_EXPECT(frames.size() >= 2);

    std::atomic<bool> done{false};
    asio::steady_timer watchdog(ioc);
    watchdog.expires_after(1s);
    watchdog.async_wait([&](const std::error_code &) {
        TEST_FAIL("watchdog fired");
        ioc.stop();
    });

    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            // 握手：ENQ -> EOT
            TEST_EXPECT_OK(co_await write_byte(a, secs::secs1::kEnq));
            auto [ec0, ch0] = co_await a.async_read_byte(200ms);
            TEST_EXPECT_OK(ec0);
            TEST_EXPECT_EQ(ch0, secs::secs1::kEot);

            // 只发送第一个块帧（end_bit=false，表示后面还有块）
            TEST_EXPECT_OK(co_await a.async_write(
                bytes_view{frames[0].data(), frames[0].size()}));
            (void)co_await a.async_read_byte(200ms); // ACK（确认）
            co_return;
        },
        asio::detached);

    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            auto [ec, _] = co_await receiver.async_receive(500ms);
            TEST_EXPECT_EQ(ec, make_error_code(errc::timeout));
            done = true;
            watchdog.cancel();
            ioc.stop();
            co_return;
        },
        asio::detached);

    ioc.run();
    TEST_EXPECT(done.load());
}

void test_t3_reply_timeout() {
    asio::io_context ioc;
    auto [a, b] = MemoryLink::create(ioc.get_executor());

    Timeouts timeouts_a{};
    timeouts_a.t1_intercharacter = 50ms;
    timeouts_a.t2_protocol = 50ms;
    timeouts_a.t3_reply = 10ms;
    timeouts_a.t4_interblock = 50ms;

    Timeouts timeouts_b = timeouts_a;
    timeouts_b.t3_reply = 200ms;

    StateMachine host(a, 0x1234, timeouts_a, 3);
    StateMachine equip(b, 0x1234, timeouts_b, 3);

    auto h = sample_header();
    h.wait_bit = true;
    auto payload = make_payload(10);

    std::atomic<int> done{0};
    asio::steady_timer watchdog(ioc);
    watchdog.expires_after(1s);
    watchdog.async_wait([&](const std::error_code &) {
        TEST_FAIL("watchdog fired");
        ioc.stop();
    });

    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            auto [ec, _] = co_await host.async_transact(
                h, bytes_view{payload.data(), payload.size()});
            TEST_EXPECT_EQ(ec, make_error_code(errc::timeout));
            if (++done == 2) {
                watchdog.cancel();
                ioc.stop();
            }
            co_return;
        },
        asio::detached);

    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            // 设备端只接收，不回复
            auto [ec, _] = co_await equip.async_receive(200ms);
            TEST_EXPECT_OK(ec);
            if (++done == 2) {
                watchdog.cancel();
                ioc.stop();
            }
            co_return;
        },
        asio::detached);

    ioc.run();
    TEST_EXPECT_EQ(done.load(), 2);
}

void test_t3_reply_success() {
    asio::io_context ioc;
    auto [a, b] = MemoryLink::create(ioc.get_executor());

    Timeouts timeouts{};
    timeouts.t1_intercharacter = 50ms;
    timeouts.t2_protocol = 50ms;
    timeouts.t3_reply = 200ms;
    timeouts.t4_interblock = 50ms;

    StateMachine host(a, 0x1234, timeouts, 3);
    StateMachine equip(b, 0x1234, timeouts, 3);

    auto req = sample_header();
    req.wait_bit = true;
    req.reverse_bit = false;
    auto req_body = make_payload(5);

    std::vector<byte> reply_body = {0xAA, 0xBB, 0xCC};

    std::atomic<int> done{0};
    asio::steady_timer watchdog(ioc);
    watchdog.expires_after(1s);
    watchdog.async_wait([&](const std::error_code &) {
        TEST_FAIL("watchdog fired");
        ioc.stop();
    });

    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            auto [ec, reply] = co_await host.async_transact(
                req, bytes_view{req_body.data(), req_body.size()});
            TEST_EXPECT_OK(ec);
            TEST_EXPECT_EQ(reply.header.system_bytes, req.system_bytes);
            TEST_EXPECT_EQ(reply.body, reply_body);
            if (++done == 2) {
                watchdog.cancel();
                ioc.stop();
            }
            co_return;
        },
        asio::detached);

    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            auto [ec, msg] = co_await equip.async_receive(200ms);
            TEST_EXPECT_OK(ec);
            TEST_EXPECT_EQ(msg.header.system_bytes, req.system_bytes);
            TEST_EXPECT_EQ(msg.body.size(), req_body.size());

            auto rep = msg.header;
            rep.reverse_bit = true;
            rep.wait_bit = false;
            rep.function = static_cast<std::uint8_t>(rep.function + 1);
            auto send_ec = co_await equip.async_send(
                rep, bytes_view{reply_body.data(), reply_body.size()});
            TEST_EXPECT_OK(send_ec);

            if (++done == 2) {
                watchdog.cancel();
                ioc.stop();
            }
            co_return;
        },
        asio::detached);

    ioc.run();
    TEST_EXPECT_EQ(done.load(), 2);
}

} // namespace

int main() {
    test_checksum_known();
    test_secs1_error_category_and_messages();
    test_encode_decode_roundtrip_single_block();
    test_encode_invalid_device_id();
    test_encode_device_id_boundary_values();
    test_encode_invalid_block_number();
    test_encode_data_too_large();
    test_encode_decode_variant_bits_and_block_number_max();
    test_decode_checksum_mismatch();
    test_decode_invalid_too_small();
    test_decode_invalid_length_out_of_range();
    test_decode_invalid_size_mismatch();
    test_decode_block_frame_too_large();
    test_decode_block_frame_size_256_ok();
    test_decode_block_frame_size_255_ok();
    test_fragment_message_splits_and_decodes();
    test_fragment_message_empty_payload();
    test_reassembler_happy_path();
    test_reassembler_device_id_mismatch();
    test_reassembler_sequence_error();
    test_reassembler_mismatch_after_first_block();
    test_memory_link_drop_next();
    test_memory_link_fixed_delay();
    test_memory_link_delay_zero_branch();
    test_memory_link_default_endpoint_invalid_argument();
    test_memory_link_all_bytes_dropped_no_event();
    test_memory_link_read_timeout_when_no_data();
    test_memory_link_read_two_bytes_paths();
    test_timer_cancelled();
    test_state_machine_send_receive_single_block();
    test_state_machine_send_unexpected_handshake_byte_protocol_error_scripted();
    test_state_machine_send_propagates_handshake_read_error_scripted();
    test_state_machine_send_propagates_handshake_write_error_scripted();
    test_state_machine_send_accepts_ack_as_handshake_response_scripted();
    test_state_machine_send_unexpected_ack_response_protocol_error_scripted();
    test_state_machine_send_propagates_ack_read_error_scripted();
    test_state_machine_send_block_timeout_retries_to_too_many_retries_scripted();
    test_state_machine_send_when_not_idle_returns_invalid_argument();
    test_state_machine_receive_when_not_idle_returns_invalid_argument();
    test_state_machine_receive_invalid_length_returns_invalid_block();
    test_state_machine_receive_too_many_bad_blocks();
    test_state_machine_receive_device_id_mismatch();
    test_state_machine_receive_block_sequence_error();
    test_state_machine_receive_ignores_noise_before_enq();
    test_sender_retries_handshake_on_nak_then_ok();
    test_sender_retries_block_on_nak();
    test_receiver_nak_then_accept_on_checksum_error();
    test_t1_intercharacter_timeout();
    test_t2_handshake_timeout_to_too_many_retries();
    test_t4_interblock_timeout();
    test_t3_reply_timeout();
    test_t3_reply_success();
    return ::secs::tests::run_and_report();
}
