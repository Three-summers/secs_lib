#include "secs/utils/secs1_dump.hpp"

#include "secs/core/error.hpp"
#include "secs/ii/codec.hpp"

#include <cstdint>
#include <iomanip>
#include <sstream>

namespace secs::utils {
namespace {

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
                           const secs::secs1::DecodedBlock &block) {
    const auto &h = block.header;
    oss << "SECS-I:\n";
    oss << "  device_id=" << fmt_hex_u16_(h.device_id)
        << " reverse_bit=" << (h.reverse_bit ? 1 : 0) << '\n';
    oss << "  S" << static_cast<int>(h.stream) << "F" << static_cast<int>(h.function)
        << " W=" << (h.wait_bit ? 1 : 0) << '\n';
    oss << "  block_number=" << h.block_number << " end_bit=" << (h.end_bit ? 1 : 0)
        << '\n';
    oss << "  system_bytes=" << fmt_hex_u32_(h.system_bytes) << '\n';
    oss << "  data=" << block.data.size() << " bytes\n";
}

void maybe_append_secs2_(std::ostringstream &oss,
                         secs::core::bytes_view body,
                         const Secs1DumpOptions &options) {
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
        oss << "SECS-II:\n";
        oss << "  decode_failed: " << ec.message() << '\n';
        return;
    }

    oss << "SECS-II:\n";
    oss << "  consumed=" << consumed << "/" << body.size();
    if (consumed != body.size()) {
        oss << " (not fully consumed)";
    }
    oss << '\n';
    oss << "  item: " << dump_item(item, options.item) << '\n';
}

} // namespace

std::string dump_secs1_block_frame(secs::core::bytes_view frame,
                                   Secs1DumpOptions options) {
    std::ostringstream oss;

    if (options.include_hex) {
        oss << "RAW(SECS-I block frame):\n";
        oss << hex_dump(frame, options.hex);
    }

    secs::secs1::DecodedBlock block{};
    const auto ec = secs::secs1::decode_block(frame, block);
    if (ec) {
        oss << "SECS-I decode_block failed: " << ec.message() << '\n';
        return oss.str();
    }

    append_block_summary_(oss, block);

    // 单 block 完整消息：end_bit=1 且 block_number=1。
    if (block.header.end_bit && block.header.block_number == 1) {
        maybe_append_secs2_(oss, block.data, options);
    }

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

    oss << "SECS-I message:\n";
    oss << "  device_id=" << fmt_hex_u16_(message_header_.device_id)
        << " reverse_bit=" << (message_header_.reverse_bit ? 1 : 0) << '\n';
    oss << "  S" << static_cast<int>(message_header_.stream) << "F"
        << static_cast<int>(message_header_.function)
        << " W=" << (message_header_.wait_bit ? 1 : 0) << '\n';
    oss << "  blocks_end_bit=" << (message_header_.end_bit ? 1 : 0) << '\n';
    oss << "  system_bytes=" << fmt_hex_u32_(message_header_.system_bytes) << '\n';
    oss << "  body=" << message_body_.size() << " bytes\n";

    if (options.include_hex) {
        oss << "RAW(SECS-I message body):\n";
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

