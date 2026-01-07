#include "test_main.hpp"

#include <secs/core/error.hpp>
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
    // 1.1) hex 解析：反例
    {
        std::vector<core::byte> bytes;
        const auto ec = utils::parse_hex("00 GG", bytes);
        TEST_EXPECT_EQ(ec, core::make_error_code(core::errc::invalid_argument));
    }
    {
        std::vector<core::byte> bytes;
        const auto ec = utils::parse_hex("0", bytes); // odd nibble
        TEST_EXPECT_EQ(ec, core::make_error_code(core::errc::invalid_argument));
    }

    // 1.2) hex dump：选项分支
    {
        const std::vector<core::byte> bytes = {
            static_cast<core::byte>(0x41), // 'A'
            static_cast<core::byte>(0x00),
            static_cast<core::byte>(0x7F),
            static_cast<core::byte>(0x42), // 'B'
        };
        utils::HexDumpOptions opt;
        opt.bytes_per_line = 0; // 覆盖“默认回退到 16”分支
        opt.max_bytes = 2;      // 覆盖截断提示
        opt.show_offset = true;
        opt.show_ascii = true;
        opt.enable_color = true;
        const auto s = utils::hex_dump(
            core::bytes_view{bytes.data(), bytes.size()}, opt);
        TEST_EXPECT(s.find("0000:") != std::string::npos);
        TEST_EXPECT(s.find("A") != std::string::npos);
        TEST_EXPECT(s.find("... (truncated") != std::string::npos);
        TEST_EXPECT(s.find("\033[") != std::string::npos);
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
    // 2.1) Item dump：覆盖更多类型/截断/格式选项
    {
        std::string tricky;
        tricky.push_back('a');
        tricky.push_back('"');
        tricky.push_back('\\');
        tricky.push_back(static_cast<char>(0x01));
        tricky.append("zzz");

        std::vector<core::byte> bin = {static_cast<core::byte>(0x00),
                                       static_cast<core::byte>(0xFF),
                                       static_cast<core::byte>(0x2A)};

        std::vector<bool> bools;
        for (int i = 0; i < 10; ++i) {
            bools.push_back((i % 2) == 0);
        }

        ii::Item item = ii::Item::list(
            {
                ii::Item::ascii(tricky),
                ii::Item::ascii(""),
                ii::Item::binary(std::move(bin)),
                ii::Item::binary({}),
                ii::Item::boolean(std::move(bools)),
                ii::Item::boolean({}),

                ii::Item::i1({-1, 0, 1, 2}),
                ii::Item::i2({-1, 0, 1, 2}),
                ii::Item::i4({-1, 0, 1, 2}),
                ii::Item::i8({-1, 0, 1, 2}),

                ii::Item::u1({0, 1, 2, 255}),
                ii::Item::u2({0, 1, 2, 65535}),
                ii::Item::u4({0, 1, 2, 100}),
                ii::Item::u8({0, 1, 2, 100}),

                ii::Item::f4({0.1f, 1.5f, 2.25f, 3.75f}),
                ii::Item::f8({0.1, 1.23456789, 2.5, 3.75}),

                ii::Item::list({ii::Item::ascii("nested")}),
                ii::Item::list({}),
            });

        utils::ItemDumpOptions opt;
        opt.enable_color = true;
        opt.multiline = false;
        opt.max_payload_bytes = 4;
        opt.max_array_items = 3;
        opt.max_list_items = 0;
        const auto s = utils::dump_item(item, opt);
        TEST_EXPECT(s.find("\\\"") != std::string::npos);
        TEST_EXPECT(s.find("\\\\") != std::string::npos);
        TEST_EXPECT(s.find("\\x") != std::string::npos);
        TEST_EXPECT(s.find("...") != std::string::npos);
        TEST_EXPECT(s.find("\033[") != std::string::npos);
    }
    // 2.1.1) Item dump：max_list_items 截断分支
    {
        ii::Item item = ii::Item::list({
            ii::Item::ascii("one"),
            ii::Item::ascii("two"),
            ii::Item::ascii("three"),
        });
        utils::ItemDumpOptions opt;
        opt.max_list_items = 1;
        opt.multiline = true;
        const auto s = utils::dump_item(item, opt);
        TEST_EXPECT(s.find("L[3]") != std::string::npos);
        TEST_EXPECT(s.find("...") != std::string::npos);
    }
    // 2.2) Item dump：max_depth 覆盖
    {
        ii::Item item = ii::Item::list({
            ii::Item::list({
                ii::Item::ascii("deep"),
            }),
        });
        utils::ItemDumpOptions opt;
        opt.max_depth = 0;
        const auto s = utils::dump_item(item, opt);
        TEST_EXPECT(s.find("...") != std::string::npos);
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
    // 3.1) HSMS dump：反例 + not fully consumed
    {
        // 非法帧：应输出错误信息。
        const std::vector<core::byte> bad = {0x00, 0x01, 0x02};
        const auto out = utils::dump_hsms_frame(
            core::bytes_view{bad.data(), bad.size()});
        TEST_EXPECT(out.find("decode_frame failed") != std::string::npos);
    }
    {
        // not fully consumed：SECS-II body 后面追加垃圾字节。
        std::vector<core::byte> body = secs2_body;
        body.push_back(static_cast<core::byte>(0x00));
        body.push_back(static_cast<core::byte>(0x01));

        const auto msg = hsms::make_data_message(
            0x0001,
            1,
            2,
            false,
            0x11223345,
            core::bytes_view{body.data(), body.size()});

        std::vector<core::byte> frame;
        TEST_EXPECT_OK(hsms::encode_frame(msg, frame));

        utils::HsmsDumpOptions opt;
        opt.include_hex = true;
        opt.enable_color = true;
        opt.enable_secs2_decode = true;

        const auto out = utils::dump_hsms_frame(
            core::bytes_view{frame.data(), frame.size()},
            opt);
        TEST_EXPECT(out.find("not fully consumed") != std::string::npos);
        TEST_EXPECT(out.find("\033[") != std::string::npos);
    }
    {
        // SECS-II 解码失败分支：body 不是合法 item。
        const std::vector<core::byte> bad_body = {
            static_cast<core::byte>(0xFF),
            static_cast<core::byte>(0xFF),
            static_cast<core::byte>(0xFF),
        };
        const auto msg = hsms::make_data_message(
            0x0001,
            1,
            2,
            false,
            0x11223346,
            core::bytes_view{bad_body.data(), bad_body.size()});

        std::vector<core::byte> frame;
        TEST_EXPECT_OK(hsms::encode_frame(msg, frame));

        utils::HsmsDumpOptions opt;
        opt.include_hex = false;
        opt.enable_secs2_decode = true;
        const auto out = utils::dump_hsms_frame(
            core::bytes_view{frame.data(), frame.size()},
            opt);
        TEST_EXPECT(out.find("decode_failed") != std::string::npos);
    }
    {
        // 控制消息：enable_secs2_decode=true 也不应输出 SECS-II 段。
        const auto msg = hsms::make_select_req(0xFFFF, 0x01020304);
        std::vector<core::byte> frame;
        TEST_EXPECT_OK(hsms::encode_frame(msg, frame));

        utils::HsmsDumpOptions opt;
        opt.enable_secs2_decode = true;
        // payload：跳过前 4B 长度字段
        const auto out = utils::dump_hsms_payload(
            core::bytes_view{frame.data() + hsms::kLengthFieldSize,
                             frame.size() - hsms::kLengthFieldSize},
            opt);
        TEST_EXPECT(out.find("SECS-II:") == std::string::npos);
    }

    // 3.2) HSMS dump：覆盖更多 control SType 分支（stype_name_）
    {
        const hsms::Header rejected_hdr = hsms::make_data_message(0x0001,
                                                                  1,
                                                                  1,
                                                                  false,
                                                                  0x0,
                                                                  core::bytes_view{})
                                             .header;
        const hsms::Message control_cases[] = {
            hsms::make_deselect_req(0xFFFF, 0x01020304),
            hsms::make_deselect_rsp(0xFFFF, /*status=*/0, 0x01020305),
            hsms::make_linktest_req(0xFFFF, 0x01020306),
            hsms::make_linktest_rsp(0xFFFF, 0x01020307),
            hsms::make_reject_req(/*reason_code=*/1, rejected_hdr),
            hsms::make_separate_req(0xFFFF, 0x01020308),
        };
        const char *expect_names[] = {
            "deselect.req",
            "deselect.rsp",
            "linktest.req",
            "linktest.rsp",
            "reject.req",
            "separate.req",
        };

        for (std::size_t i = 0; i < (sizeof(control_cases) / sizeof(control_cases[0]));
             ++i) {
            std::vector<core::byte> frame;
            TEST_EXPECT_OK(hsms::encode_frame(control_cases[i], frame));
            const auto out = utils::dump_hsms_frame(
                core::bytes_view{frame.data(), frame.size()});
            TEST_EXPECT(out.find(expect_names[i]) != std::string::npos);
        }
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
    // 4.0) SECS-I message dump（header+body）：覆盖 dump_secs1_message
    {
        secs::secs1::Header hdr{};
        hdr.reverse_bit = true;
        hdr.device_id = 0x1234;
        hdr.wait_bit = true;
        hdr.stream = 2;
        hdr.function = 1;
        hdr.end_bit = true;
        hdr.block_number = 7;
        hdr.system_bytes = 0xAABBCCDD;

        utils::Secs1DumpOptions opt;
        opt.include_hex = false;
        opt.enable_secs2_decode = true;

        const auto out = utils::dump_secs1_message(
            hdr, core::bytes_view{secs2_body.data(), secs2_body.size()}, opt);
        TEST_EXPECT(out.find("SECS-I message:") != std::string::npos);
        TEST_EXPECT(out.find("SECS-II:") != std::string::npos);
    }
    // 4.1) SECS-I dump：反例 + not fully consumed
    {
        const std::vector<core::byte> bad = {0x00, 0x01, 0x02};
        const auto out = utils::dump_secs1_block_frame(
            core::bytes_view{bad.data(), bad.size()});
        TEST_EXPECT(out.find("decode_block failed") != std::string::npos);
    }
    {
        // not fully consumed：SECS-II body 后面追加垃圾字节。
        std::vector<core::byte> body = secs2_body;
        body.push_back(static_cast<core::byte>(0x00));
        body.push_back(static_cast<core::byte>(0x01));

        secs::secs1::Header hdr{};
        hdr.reverse_bit = false;
        hdr.device_id = 1;
        hdr.wait_bit = false;
        hdr.stream = 1;
        hdr.function = 2;
        hdr.end_bit = true;
        hdr.block_number = 1;
        hdr.system_bytes = 0x11223355;

        std::vector<core::byte> block_frame;
        TEST_EXPECT_OK(secs1::encode_block(
            hdr,
            core::bytes_view{body.data(), body.size()},
            block_frame));

        utils::Secs1DumpOptions opt;
        opt.include_hex = true;
        opt.enable_color = true;
        opt.enable_secs2_decode = true;

        const auto out = utils::dump_secs1_block_frame(
            core::bytes_view{block_frame.data(), block_frame.size()},
            opt);
        TEST_EXPECT(out.find("not fully consumed") != std::string::npos);
        TEST_EXPECT(out.find("\033[") != std::string::npos);
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

        // dump_message：覆盖格式化分支（不要求内容完全一致，只要包含关键信息）
        {
            utils::Secs1DumpOptions opt;
            opt.include_hex = false;
            const auto s = reasm.dump_message(opt);
            TEST_EXPECT(s.find("SECS-I message:") != std::string::npos);
            TEST_EXPECT(s.find("S7F1") != std::string::npos);
        }
    }
    // 5.1) 重组器：空消息 decode 的反例
    {
        utils::Secs1MessageReassembler reasm;
        ii::Item out = ii::Item::list({});
        std::size_t consumed = 0;
        const auto ec = reasm.decode_message_body_as_secs2(
            out, consumed, ii::DecodeLimits{});
        TEST_EXPECT_EQ(ec, core::make_error_code(core::errc::invalid_argument));
    }

    // 5.2) 重组器：成功解码 SECS-II（覆盖 decode_message_body_as_secs2 的成功路径）
    {
        std::string big_ascii(512, 'A');
        std::vector<core::byte> body;
        TEST_EXPECT_OK(ii::encode(ii::Item::ascii(std::move(big_ascii)), body));

        secs::secs1::Header base{};
        base.reverse_bit = false;
        base.device_id = 1;
        base.wait_bit = true;
        base.stream = 1;
        base.function = 1;
        base.end_bit = false;
        base.block_number = 1;
        base.system_bytes = 0x11223344;

        const auto frames =
            secs1::fragment_message(base, core::bytes_view{body.data(), body.size()});
        TEST_EXPECT(frames.size() > 1);

        utils::Secs1MessageReassembler reasm;
        bool ready = false;
        for (const auto &frame : frames) {
            TEST_EXPECT_OK(reasm.accept_frame(core::bytes_view{frame.data(), frame.size()},
                                              ready));
        }
        TEST_EXPECT(ready);

        ii::Item decoded = ii::Item::list({});
        std::size_t consumed = 0;
        TEST_EXPECT_OK(reasm.decode_message_body_as_secs2(decoded,
                                                          consumed,
                                                          ii::DecodeLimits{}));
        TEST_EXPECT_EQ(consumed, body.size());

        const auto *ascii = decoded.get_if<ii::ASCII>();
        TEST_EXPECT(ascii != nullptr);
        TEST_EXPECT_EQ(ascii->value.size(), static_cast<std::size_t>(512));
    }

    return secs::tests::run_and_report();
}
