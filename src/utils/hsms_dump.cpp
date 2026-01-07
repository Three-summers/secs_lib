#include "secs/utils/hsms_dump.hpp"

#include "secs/core/error.hpp"
#include "secs/ii/codec.hpp"

#include <cstdint>
#include <iomanip>
#include <optional>
#include <sstream>

namespace secs::utils {
namespace {

[[nodiscard]] const char *stype_name_(secs::hsms::SType s) noexcept {
    using secs::hsms::SType;
    switch (s) {
    case SType::data:
        return "data";
    case SType::select_req:
        return "select.req";
    case SType::select_rsp:
        return "select.rsp";
    case SType::deselect_req:
        return "deselect.req";
    case SType::deselect_rsp:
        return "deselect.rsp";
    case SType::linktest_req:
        return "linktest.req";
    case SType::linktest_rsp:
        return "linktest.rsp";
    case SType::reject_req:
        return "reject.req";
    case SType::separate_req:
        return "separate.req";
    default:
        return "unknown";
    }
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

void append_message_summary_(std::ostringstream &oss,
                             const secs::hsms::Message &msg) {
    const auto stype_u8 = static_cast<std::uint8_t>(msg.header.s_type);

    oss << "HSMS:\n";
    oss << "  session_id=" << fmt_hex_u16_(msg.header.session_id) << '\n';
    oss << "  header_byte2=0x" << std::hex << std::setw(2) << std::setfill('0')
        << static_cast<int>(msg.header.header_byte2) << std::dec << '\n';
    oss << "  header_byte3=0x" << std::hex << std::setw(2) << std::setfill('0')
        << static_cast<int>(msg.header.header_byte3) << std::dec << '\n';
    oss << "  p_type=0x" << std::hex << std::setw(2) << std::setfill('0')
        << static_cast<int>(msg.header.p_type) << std::dec << '\n';
    oss << "  s_type=0x" << std::hex << std::setw(2) << std::setfill('0')
        << static_cast<int>(stype_u8) << std::dec << " (" << stype_name_(msg.header.s_type)
        << ")\n";
    oss << "  system_bytes=" << fmt_hex_u32_(msg.header.system_bytes) << '\n';

    if (msg.is_data()) {
        oss << "  data: S" << static_cast<int>(msg.stream()) << "F"
            << static_cast<int>(msg.function()) << " W=" << (msg.w_bit() ? 1 : 0)
            << '\n';
    }

    oss << "  body=" << msg.body.size() << " bytes\n";
}

void maybe_append_secs2_(std::ostringstream &oss,
                         const secs::hsms::Message &msg,
                         const HsmsDumpOptions &options) {
    if (!options.enable_secs2_decode) {
        return;
    }
    if (!msg.is_data()) {
        return;
    }
    if (msg.body.empty()) {
        return;
    }

    secs::ii::Item item = secs::ii::Item::list({});
    std::size_t consumed = 0;
    const auto ec =
        secs::ii::decode_one(secs::core::bytes_view{msg.body.data(), msg.body.size()},
                             item,
                             consumed,
                             options.secs2_limits);
    if (ec) {
        oss << "SECS-II:\n";
        oss << "  decode_failed: " << ec.message() << '\n';
        return;
    }

    oss << "SECS-II:\n";
    oss << "  consumed=" << consumed << "/" << msg.body.size();
    if (consumed != msg.body.size()) {
        oss << " (not fully consumed)";
    }
    oss << '\n';
    oss << "  item: " << dump_item(item, options.item) << '\n';
}

} // namespace

std::string dump_hsms_frame(secs::core::bytes_view frame,
                            HsmsDumpOptions options) {
    std::ostringstream oss;

    if (options.include_hex) {
        oss << "RAW(HSMS frame):\n";
        oss << hex_dump(frame, options.hex);
    }

    secs::hsms::Message msg{};
    std::size_t consumed = 0;
    const auto ec = secs::hsms::decode_frame(frame, msg, consumed);
    if (ec) {
        oss << "HSMS decode_frame failed: " << ec.message() << '\n';
        return oss.str();
    }

    oss << "consumed=" << consumed << "/" << frame.size() << '\n';
    append_message_summary_(oss, msg);
    maybe_append_secs2_(oss, msg, options);
    return oss.str();
}

std::string dump_hsms_payload(secs::core::bytes_view payload,
                              HsmsDumpOptions options) {
    std::ostringstream oss;

    if (options.include_hex) {
        oss << "RAW(HSMS payload):\n";
        oss << hex_dump(payload, options.hex);
    }

    secs::hsms::Message msg{};
    const auto ec = secs::hsms::decode_payload(payload, msg);
    if (ec) {
        oss << "HSMS decode_payload failed: " << ec.message() << '\n';
        return oss.str();
    }

    append_message_summary_(oss, msg);
    maybe_append_secs2_(oss, msg, options);
    return oss.str();
}

} // namespace secs::utils

