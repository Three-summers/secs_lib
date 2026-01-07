#include "secs/utils/secs1_dump.hpp"

#include "secs/core/error.hpp"
#include "secs/ii/codec.hpp"

#include <cstdint>
#include <iomanip>
#include <sstream>

namespace secs::utils {
namespace {

struct Ansi final {
    static constexpr const char *reset = "\033[0m";
    static constexpr const char *header = "\033[1;36m";
    static constexpr const char *label = "\033[1;33m";
    static constexpr const char *key = "\033[1;32m";
    static constexpr const char *value = "\033[1;37m";
    static constexpr const char *dim = "\033[2m";
    static constexpr const char *error = "\033[1;31m";
};

[[nodiscard]] const char *ansi_(bool enable, const char *code) noexcept {
    return enable ? code : "";
}

[[nodiscard]] std::string fmt_hex_u16_(std::uint16_t v) {
    std::ostringstream oss;
    oss << "0x" << std::hex << std::setw(4) << std::setfill('0') << v;
    return oss.str();
}

[[nodiscard]] std::string fmt_hex_u32_(std::uint32_t v) {
    std::ostringstream oss;
    oss << "0x" << std::hex << std::setw(8) << std::setfill('0') << v;
    return oss.str();
}

void append_block_summary_(std::ostringstream &oss,
                           const secs::secs1::DecodedBlock &block,
                           bool enable_color) {
    const auto *reset = ansi_(enable_color, Ansi::reset);
    const auto *header = ansi_(enable_color, Ansi::header);
    const auto *key = ansi_(enable_color, Ansi::key);
    const auto *value = ansi_(enable_color, Ansi::value);

    const auto &h = block.header;
    oss << header << "SECS-I:" << reset << '\n';
    oss << "  " << key << "device_id" << reset << "=" << value
        << fmt_hex_u16_(h.device_id) << reset << ' ' << key << "reverse_bit"
        << reset << "=" << value << (h.reverse_bit ? 1 : 0) << reset << '\n';
    oss << "  " << value << "S" << static_cast<int>(h.stream) << "F"
        << static_cast<int>(h.function) << reset << ' ' << key << "W" << reset
        << "=" << value << (h.wait_bit ? 1 : 0) << reset << '\n';
    oss << "  " << key << "block_number" << reset << "=" << value << h.block_number
        << reset << ' ' << key << "end_bit" << reset << "=" << value
        << (h.end_bit ? 1 : 0) << reset << '\n';
    oss << "  " << key << "system_bytes" << reset << "=" << value
        << fmt_hex_u32_(h.system_bytes) << reset << '\n';
    oss << "  " << key << "data" << reset << "=" << value << block.data.size()
        << reset << " bytes\n";
}

void maybe_append_secs2_(std::ostringstream &oss,
                         secs::core::bytes_view body,
                         const Secs1DumpOptions &options) {
    const auto *reset = ansi_(options.enable_color, Ansi::reset);
    const auto *header = ansi_(options.enable_color, Ansi::header);
    const auto *key = ansi_(options.enable_color, Ansi::key);
    const auto *value = ansi_(options.enable_color, Ansi::value);
    const auto *dim = ansi_(options.enable_color, Ansi::dim);
    const auto *error = ansi_(options.enable_color, Ansi::error);

    if (!options.enable_secs2_decode) {
        return;
    }
    if (body.empty()) {
        return;
    }

    secs::ii::Item item = secs::ii::Item::list({});
    std::size_t consumed = 0;
    const auto ec =
        secs::ii::decode_one(body, item, consumed, options.secs2_limits);
    if (ec) {
        oss << header << "SECS-II:" << reset << '\n';
        oss << "  " << error << "decode_failed" << reset << ": " << error
            << ec.message() << reset << '\n';
        return;
    }

    oss << header << "SECS-II:" << reset << '\n';
    oss << "  " << key << "consumed" << reset << "=" << value << consumed << reset
        << "/" << value << body.size() << reset;
    if (consumed != body.size()) {
        oss << ' ' << dim << "(not fully consumed)" << reset;
    }
    oss << '\n';
    oss << "  " << key << "item" << reset << ": " << dump_item(item, options.item)
        << '\n';
}

} // namespace

std::string dump_secs1_block_frame(secs::core::bytes_view frame,
                                   Secs1DumpOptions options) {
    std::ostringstream oss;
    const bool enable_color = options.enable_color;
    const auto *reset = ansi_(enable_color, Ansi::reset);
    const auto *label = ansi_(enable_color, Ansi::label);
    const auto *error = ansi_(enable_color, Ansi::error);

    if (options.include_hex) {
        oss << label << "RAW(SECS-I block frame):" << reset << '\n';
        oss << hex_dump(frame, options.hex);
    }

    secs::secs1::DecodedBlock block{};
    const auto ec = secs::secs1::decode_block(frame, block);
    if (ec) {
        oss << error << "SECS-I decode_block failed: " << ec.message() << reset
            << '\n';
        return oss.str();
    }

    append_block_summary_(oss, block, enable_color);

    // 单 block 完整消息：end_bit=1 且 block_number=1。
    if (block.header.end_bit && block.header.block_number == 1) {
        maybe_append_secs2_(oss, block.data, options);
    }

    return oss.str();
}

std::string dump_secs1_message(const secs::secs1::Header &header,
                               secs::core::bytes_view body,
                               Secs1DumpOptions options) {
    std::ostringstream oss;
    const bool enable_color = options.enable_color;
    const auto *reset = ansi_(enable_color, Ansi::reset);
    const auto *header_color = ansi_(enable_color, Ansi::header);
    const auto *label = ansi_(enable_color, Ansi::label);
    const auto *key = ansi_(enable_color, Ansi::key);
    const auto *value = ansi_(enable_color, Ansi::value);

    oss << header_color << "SECS-I message:" << reset << '\n';
    oss << "  " << key << "device_id" << reset << "=" << value
        << fmt_hex_u16_(header.device_id) << reset << ' ' << key << "reverse_bit"
        << reset << "=" << value << (header.reverse_bit ? 1 : 0) << reset << '\n';
    oss << "  " << value << "S" << static_cast<int>(header.stream) << "F"
        << static_cast<int>(header.function) << reset << ' ' << key << "W"
        << reset << "=" << value << (header.wait_bit ? 1 : 0) << reset << '\n';
    oss << "  " << key << "blocks_end_bit" << reset << "=" << value
        << (header.end_bit ? 1 : 0) << reset << '\n';
    oss << "  " << key << "system_bytes" << reset << "=" << value
        << fmt_hex_u32_(header.system_bytes) << reset << '\n';
    oss << "  " << key << "body" << reset << "=" << value << body.size() << reset
        << " bytes\n";

    if (options.include_hex) {
        oss << label << "RAW(SECS-I message body):" << reset << '\n';
        oss << hex_dump(body, options.hex);
    }

    maybe_append_secs2_(oss, body, options);
    return oss.str();
}

Secs1MessageReassembler::Secs1MessageReassembler(
    std::optional<std::uint16_t> expected_device_id)
    : reassembler_(expected_device_id) {}

void Secs1MessageReassembler::reset() noexcept {
    reassembler_.reset();
    message_header_ = secs::secs1::Header{};
    message_body_.clear();
}

std::error_code Secs1MessageReassembler::accept_frame(secs::core::bytes_view frame,
                                                      bool &message_ready) {
    message_ready = false;

    secs::secs1::DecodedBlock block{};
    const auto ec = secs::secs1::decode_block(frame, block);
    if (ec) {
        return ec;
    }

    const auto acc = reassembler_.accept(block);
    if (acc) {
        return acc;
    }

    if (!reassembler_.has_message()) {
        return {};
    }

    // 完整消息到达：拷贝一份 body 以保证 reset 后依旧可读。
    message_header_ = reassembler_.message_header();
    const auto body_view = reassembler_.message_body();
    message_body_.assign(body_view.begin(), body_view.end());

    reassembler_.reset();
    message_ready = true;
    return {};
}

std::string Secs1MessageReassembler::dump_message(Secs1DumpOptions options) const {
    std::ostringstream oss;
    const bool enable_color = options.enable_color;
    const auto *reset = ansi_(enable_color, Ansi::reset);
    const auto *header = ansi_(enable_color, Ansi::header);
    const auto *label = ansi_(enable_color, Ansi::label);
    const auto *key = ansi_(enable_color, Ansi::key);
    const auto *value = ansi_(enable_color, Ansi::value);

    oss << header << "SECS-I message:" << reset << '\n';
    oss << "  " << key << "device_id" << reset << "=" << value
        << fmt_hex_u16_(message_header_.device_id) << reset << ' ' << key
        << "reverse_bit" << reset << "=" << value
        << (message_header_.reverse_bit ? 1 : 0) << reset << '\n';
    oss << "  " << value << "S" << static_cast<int>(message_header_.stream) << "F"
        << static_cast<int>(message_header_.function) << reset << ' ' << key
        << "W" << reset << "=" << value << (message_header_.wait_bit ? 1 : 0)
        << reset << '\n';
    oss << "  " << key << "blocks_end_bit" << reset << "=" << value
        << (message_header_.end_bit ? 1 : 0) << reset << '\n';
    oss << "  " << key << "system_bytes" << reset << "=" << value
        << fmt_hex_u32_(message_header_.system_bytes) << reset << '\n';
    oss << "  " << key << "body" << reset << "=" << value << message_body_.size()
        << reset << " bytes\n";

    if (options.include_hex) {
        oss << label << "RAW(SECS-I message body):" << reset << '\n';
        oss << hex_dump(secs::core::bytes_view{message_body_.data(), message_body_.size()},
                        options.hex);
    }

    maybe_append_secs2_(
        oss,
        secs::core::bytes_view{message_body_.data(), message_body_.size()},
        options);
    return oss.str();
}

std::error_code Secs1MessageReassembler::decode_message_body_as_secs2(
    secs::ii::Item &out,
    std::size_t &consumed,
    const secs::ii::DecodeLimits &limits) const noexcept {
    consumed = 0;
    if (message_body_.empty()) {
        return secs::core::make_error_code(secs::core::errc::invalid_argument);
    }

    return secs::ii::decode_one(
        secs::core::bytes_view{message_body_.data(), message_body_.size()},
        out,
        consumed,
        limits);
}

} // namespace secs::utils
