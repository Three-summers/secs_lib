/**
 * @file utils_dump_example.cpp
 * @brief 演示 secs::utils 的二进制报文解析与可视化输出
 *
 * 典型用途：
 * - 从抓包/日志里复制一段十六进制字符串，快速解析并查看 HSMS/SECS-I 头字段；
 * - 可选将 Data Message 的 body 尝试解码为 SECS-II Item，并以可读格式输出。
 *
 * 运行：
 * - 无参数：运行内置示例（会生成 HSMS 帧 + SECS-I 单 block + SECS-I 多 block）
 * - 指定输入（将十六进制字符串整体作为一个参数传入）：
 *   - ./build/examples/utils_dump_example hsms "<hex>" [--secs2] [--no-hex] [--no-color]
 *   - ./build/examples/utils_dump_example hsms-payload "<hex>" [--secs2] [--no-hex] [--no-color]
 *   - ./build/examples/utils_dump_example secs1 "<hex>" [--secs2] [--no-hex] [--no-color]
 */

#include <secs/core/common.hpp>
#include <secs/hsms/message.hpp>
#include <secs/ii/item.hpp>
#include <secs/secs1/block.hpp>
#include <secs/utils/hex.hpp>
#include <secs/utils/hsms_dump.hpp>
#include <secs/utils/ii_helpers.hpp>
#include <secs/utils/item_dump.hpp>
#include <secs/utils/secs1_dump.hpp>

#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

using namespace secs;

namespace {

[[nodiscard]] bool has_flag(int argc, char **argv, std::string_view flag) {
    for (int i = 0; i < argc; ++i) {
        if (argv[i] == flag) {
            return true;
        }
    }
    return false;
}

void print_usage(const char *argv0) {
    std::cout << "用法:\n";
    std::cout << "  " << argv0 << "\n";
    std::cout << "  " << argv0
              << " hsms \"<hex>\" [--secs2] [--no-hex] [--no-color]\n";
    std::cout << "  " << argv0
              << " hsms-payload \"<hex>\" [--secs2] [--no-hex] [--no-color]\n";
    std::cout << "  " << argv0
              << " secs1 \"<hex>\" [--secs2] [--no-hex] [--no-color]\n";
}

[[nodiscard]] std::string bytes_to_hex_string(core::bytes_view bytes) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        oss << std::setw(2) << static_cast<int>(bytes[i]);
        if (i + 1 != bytes.size()) {
            oss << ' ';
        }
    }
    return oss.str();
}

[[nodiscard]] core::bytes_view as_view(const std::vector<core::byte> &bytes) {
    return core::bytes_view{bytes.data(), bytes.size()};
}

int run_dump_from_hex(std::string_view mode,
                      std::string_view hex_text,
                      bool include_hex,
                      bool enable_secs2_decode,
                      bool enable_color) {
    std::vector<core::byte> bytes;
    const auto ec = utils::parse_hex(hex_text, bytes);
    if (ec) {
        std::cerr << "parse_hex 失败: " << ec.message() << "\n";
        return 2;
    }

    if (mode == "hsms") {
        utils::HsmsDumpOptions opt;
        opt.include_hex = include_hex;
        opt.enable_secs2_decode = enable_secs2_decode;
        opt.enable_color = enable_color;
        opt.hex.max_bytes = 256;
        opt.hex.show_ascii = true;
        opt.hex.enable_color = enable_color;
        opt.item.enable_color = enable_color;

        std::cout << utils::dump_hsms_frame(as_view(bytes), opt) << "\n";
        return 0;
    }

    if (mode == "hsms-payload") {
        utils::HsmsDumpOptions opt;
        opt.include_hex = include_hex;
        opt.enable_secs2_decode = enable_secs2_decode;
        opt.enable_color = enable_color;
        opt.hex.max_bytes = 256;
        opt.hex.show_ascii = true;
        opt.hex.enable_color = enable_color;
        opt.item.enable_color = enable_color;

        std::cout << utils::dump_hsms_payload(as_view(bytes), opt) << "\n";
        return 0;
    }

    if (mode == "secs1") {
        utils::Secs1DumpOptions opt;
        opt.include_hex = include_hex;
        opt.enable_secs2_decode = enable_secs2_decode;
        opt.enable_color = enable_color;
        opt.hex.max_bytes = 256;
        opt.hex.show_ascii = true;
        opt.hex.enable_color = enable_color;
        opt.item.enable_color = enable_color;

        std::cout << utils::dump_secs1_block_frame(as_view(bytes), opt) << "\n";
        return 0;
    }

    std::cerr << "未知模式: " << mode << "\n";
    return 2;
}

void demo_hsms_frame_dump() {
    std::cout << "\n====================\n";
    std::cout << "示例 1：HSMS 帧 dump（含 SECS-II 解码）\n";
    std::cout << "====================\n";

    // 构造一个 S1F2 的 HSMS Data Message，body 是 SECS-II: A \"OK\"。
    auto [enc_ec, secs2_body] = secs::utils::encode_item(ii::Item::ascii("OK"));
    if (enc_ec) {
        std::cerr << "SECS-II encode 失败: " << enc_ec.message() << "\n";
        return;
    }

    const auto msg = hsms::make_data_message(
        0x0001,
        1,
        2,
        false,
        0x11223344,
        as_view(secs2_body));

    std::vector<core::byte> frame;
    {
        const auto ec = hsms::encode_frame(msg, frame);
        if (ec) {
            std::cerr << "HSMS encode_frame 失败: " << ec.message() << "\n";
            return;
        }
    }

    // 演示“抓包十六进制字符串 -> parse_hex -> dump_hsms_frame”的常见流程。
    const auto hex_text = bytes_to_hex_string(as_view(frame));
    std::cout << "可复制的十六进制字符串（HSMS frame，含 4B Length）：\n";
    std::cout << hex_text << "\n\n";

    std::vector<core::byte> parsed;
    {
        const auto ec = utils::parse_hex(hex_text, parsed);
        if (ec) {
            std::cerr << "parse_hex 失败: " << ec.message() << "\n";
            return;
        }
    }

    utils::HsmsDumpOptions opt;
    opt.include_hex = true;
    opt.enable_color = true;
    opt.hex.max_bytes = 256;
    opt.hex.show_ascii = true;
    opt.hex.enable_color = true;
    opt.enable_secs2_decode = true;
    opt.item.max_payload_bytes = 64;
    opt.item.enable_color = true;

    std::cout << utils::dump_hsms_frame(as_view(parsed), opt) << "\n";
}

void demo_secs1_block_dump() {
    std::cout << "\n====================\n";
    std::cout << "示例 2：SECS-I 单 block dump（含 SECS-II 解码）\n";
    std::cout << "====================\n";

    auto [enc_ec, secs2_body] = secs::utils::encode_item(ii::Item::ascii("OK"));
    if (enc_ec) {
        std::cerr << "SECS-II encode 失败: " << enc_ec.message() << "\n";
        return;
    }

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
    {
        const auto ec = secs1::encode_block(hdr, as_view(secs2_body), block_frame);
        if (ec) {
            std::cerr << "SECS-I encode_block 失败: " << ec.message() << "\n";
            return;
        }
    }

    utils::Secs1DumpOptions opt;
    opt.include_hex = true;
    opt.enable_color = true;
    opt.hex.max_bytes = 256;
    opt.hex.show_ascii = true;
    opt.hex.enable_color = true;
    opt.enable_secs2_decode = true;
    opt.item.max_payload_bytes = 64;
    opt.item.enable_color = true;

    std::cout << utils::dump_secs1_block_frame(as_view(block_frame), opt) << "\n";
}

void demo_secs1_multiblock_reassemble() {
    std::cout << "\n====================\n";
    std::cout << "示例 3：SECS-I 多 block 重组（并可选解码 SECS-II）\n";
    std::cout << "====================\n";

    // 构造一个较大的 SECS-II body（Binary[700]），确保会被分片成多个 block。
    std::vector<ii::byte> raw_binary(700);
    for (std::size_t i = 0; i < raw_binary.size(); ++i) {
        raw_binary[i] = static_cast<ii::byte>(i & 0xFFU);
    }
    const ii::Item big_item = ii::Item::binary(raw_binary);

    auto [enc_ec, secs2_body] = secs::utils::encode_item(big_item);
    if (enc_ec) {
        std::cerr << "SECS-II encode 失败: " << enc_ec.message() << "\n";
        return;
    }

    secs::secs1::Header base{};
    base.reverse_bit = false;
    base.device_id = 1;
    base.wait_bit = true;
    base.stream = 7;
    base.function = 1;
    base.end_bit = false; // 会在 fragment_message 内按最后一片自动置位
    base.block_number = 1;
    base.system_bytes = 0x01020304;

    const auto frames = secs1::fragment_message(base, as_view(secs2_body));
    std::cout << "分片数量: " << frames.size() << "\n";
    if (frames.size() <= 1) {
        std::cout << "提示：该 payload 未触发分片，可适当调大 Binary 长度。\n";
    }

    utils::Secs1MessageReassembler reasm{std::optional<std::uint16_t>{1}};

    bool ready = false;
    for (std::size_t i = 0; i < frames.size(); ++i) {
        const auto ec = reasm.accept_frame(as_view(frames[i]), ready);
        if (ec) {
            std::cerr << "accept_frame 失败: " << ec.message() << "\n";
            return;
        }
        if (ready) {
            break;
        }
    }

    if (!ready) {
        std::cerr << "未重组出完整消息（可能丢包或输入不完整）\n";
        return;
    }

    utils::Secs1DumpOptions opt;
    opt.include_hex = false; // body 很大，避免输出过长
    opt.enable_color = true;
    opt.enable_secs2_decode = true;
    opt.item.max_payload_bytes = 64; // Binary 仅展示前 64 bytes
    opt.item.multiline = true;
    opt.item.enable_color = true;

    std::cout << reasm.dump_message(opt) << "\n";
}

} // namespace

int main(int argc, char **argv) {
    // 传入参数：将抓包十六进制字符串转换为 bytes 并直接 dump。
    if (argc >= 3) {
        const std::string_view mode = argv[1];
        const std::string_view hex_text = argv[2];

        const bool include_hex = !has_flag(argc, argv, "--no-hex");
        const bool enable_secs2_decode = has_flag(argc, argv, "--secs2");
        const bool enable_color = !has_flag(argc, argv, "--no-color");

        const int rc =
            run_dump_from_hex(
                mode,
                hex_text,
                include_hex,
                enable_secs2_decode,
                enable_color);
        if (rc == 2) {
            print_usage(argv[0]);
        }
        return rc;
    }

    if (argc != 1) {
        print_usage(argv[0]);
        return 2;
    }

    std::cout << "=== secs::utils dump 工具示例 ===\n";
    demo_hsms_frame_dump();
    demo_secs1_block_dump();
    demo_secs1_multiblock_reassemble();

    std::cout << "\n提示：你也可以把抓包的十六进制字符串直接喂给本程序。\n";
    std::cout << "例如：\n";
    std::cout << "  " << argv[0] << " hsms \"00 00 00 0e ...\" --secs2\n";
    return 0;
}
