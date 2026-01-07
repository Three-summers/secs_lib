#include "test_main.hpp"

#include <secs/hsms/message.hpp>
#include <secs/ii/codec.hpp>
#include <secs/secs1/block.hpp>
#include <secs/utils/hex.hpp>
#include <secs/utils/hsms_dump.hpp>
#include <secs/utils/item_dump.hpp>
#include <secs/utils/secs1_dump.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

using namespace secs;

int main() {
    // 1) hex 解析
    {
        std::vector<core::byte> bytes;
        const auto ec = utils::parse_hex("00 01 0x02,03", bytes);
        TEST_EXPECT_OK(ec);
        TEST_EXPECT_EQ(bytes.size(), static_cast<std::size_t>(4));
        TEST_EXPECT_EQ(bytes[0], static_cast<core::byte>(0x00));
        TEST_EXPECT_EQ(bytes[1], static_cast<core::byte>(0x01));
        TEST_EXPECT_EQ(bytes[2], static_cast<core::byte>(0x02));
        TEST_EXPECT_EQ(bytes[3], static_cast<core::byte>(0x03));
    }

    // 2) Item dump
    {
        ii::Item item = ii::Item::list({
            ii::Item::ascii("OK"),
            ii::Item::u4({100}),
        });
        const auto s = utils::dump_item(item);
        TEST_EXPECT(s.find("L[2]") != std::string::npos);
        TEST_EXPECT(s.find("A[2]") != std::string::npos);
        TEST_EXPECT(s.find("U4[1]") != std::string::npos);
    }

    // 3) HSMS frame dump（含 SECS-II 解码）
    std::vector<core::byte> secs2_body;
    {
        const auto ec = ii::encode(ii::Item::ascii("OK"), secs2_body);
        TEST_EXPECT_OK(ec);
    }
    {
        const auto msg = hsms::make_data_message(
            0x0001,
            1,
            2,
            false,
            0x11223344,
            core::bytes_view{secs2_body.data(), secs2_body.size()});

        std::vector<core::byte> frame;
        TEST_EXPECT_OK(hsms::encode_frame(msg, frame));

        utils::HsmsDumpOptions opt;
        opt.include_hex = false;
        opt.enable_secs2_decode = true;

        const auto out = utils::dump_hsms_frame(
            core::bytes_view{frame.data(), frame.size()},
            opt);
        TEST_EXPECT(out.find("S1F2") != std::string::npos);
        TEST_EXPECT(out.find("W=0") != std::string::npos);
        TEST_EXPECT(out.find("A[2]") != std::string::npos);
    }

    // 4) SECS-I 单 block dump（含 SECS-II 解码）
    {
        secs::secs1::Header hdr{};
        hdr.reverse_bit = false;
        hdr.device_id = 1;
        hdr.wait_bit = false;
        hdr.stream = 1;
        hdr.function = 2;
        hdr.end_bit = true;
        hdr.block_number = 1;
        hdr.system_bytes = 0x11223344;

        std::vector<core::byte> block_frame;
        TEST_EXPECT_OK(secs1::encode_block(
            hdr,
            core::bytes_view{secs2_body.data(), secs2_body.size()},
            block_frame));

        utils::Secs1DumpOptions opt;
        opt.include_hex = false;
        opt.enable_secs2_decode = true;

        const auto out = utils::dump_secs1_block_frame(
            core::bytes_view{block_frame.data(), block_frame.size()},
            opt);
        TEST_EXPECT(out.find("S1F2") != std::string::npos);
        TEST_EXPECT(out.find("A[2]") != std::string::npos);
    }

    // 5) SECS-I 多 block 重组
    {
        std::vector<core::byte> payload(700);
        for (std::size_t i = 0; i < payload.size(); ++i) {
            payload[i] = static_cast<core::byte>(i & 0xFFU);
        }

        secs::secs1::Header base{};
        base.reverse_bit = false;
        base.device_id = 1;
        base.wait_bit = true;
        base.stream = 7;
        base.function = 1;
        base.end_bit = false;
        base.block_number = 1;
        base.system_bytes = 0x01020304;

        const auto frames = secs1::fragment_message(
            base,
            core::bytes_view{payload.data(), payload.size()});
        TEST_EXPECT(frames.size() > 1);

        utils::Secs1MessageReassembler reasm{
            std::optional<std::uint16_t>{1}};

        bool ready = false;
        for (std::size_t i = 0; i < frames.size(); ++i) {
            const auto ec = reasm.accept_frame(
                core::bytes_view{frames[i].data(), frames[i].size()},
                ready);
            TEST_EXPECT_OK(ec);
            if (i + 1 != frames.size()) {
                TEST_EXPECT(!ready);
            }
        }
        TEST_EXPECT(ready);

        const auto body = reasm.message_body();
        TEST_EXPECT_EQ(body.size(), payload.size());
        TEST_EXPECT_EQ(body[0], payload[0]);
        TEST_EXPECT_EQ(body[1], payload[1]);
        TEST_EXPECT_EQ(body[body.size() - 1], payload[payload.size() - 1]);
    }

    return secs::tests::run_and_report();
}

