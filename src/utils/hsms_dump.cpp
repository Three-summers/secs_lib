#include "secs/utils/hsms_dump.hpp"

#include "secs/core/error.hpp"
#include "secs/ii/codec.hpp"

#include <cstdint>
#include <iomanip>
#include <optional>
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
                             const secs::hsms::Message &msg,
                             bool enable_color) {
    const auto *reset = ansi_(enable_color, Ansi::reset);
    const auto *header = ansi_(enable_color, Ansi::header);
    const auto *key = ansi_(enable_color, Ansi::key);
    const auto *value = ansi_(enable_color, Ansi::value);
    const auto *dim = ansi_(enable_color, Ansi::dim);

    const auto stype_u8 = static_cast<std::uint8_t>(msg.header.s_type);

    oss << header << "HSMS:" << reset << '\n';
    oss << "  " << key << "session_id" << reset << "=" << value
        << fmt_hex_u16_(msg.header.session_id) << reset << '\n';
    oss << "  " << key << "header_byte2" << reset << "=" << value << "0x"
        << std::hex << std::setw(2) << std::setfill('0')
        << static_cast<int>(msg.header.header_byte2) << std::dec << reset << '\n';
    oss << "  " << key << "header_byte3" << reset << "=" << value << "0x"
        << std::hex << std::setw(2) << std::setfill('0')
        << static_cast<int>(msg.header.header_byte3) << std::dec << reset << '\n';
    oss << "  " << key << "p_type" << reset << "=" << value << "0x" << std::hex
        << std::setw(2) << std::setfill('0') << static_cast<int>(msg.header.p_type)
        << std::dec << reset << '\n';
    oss << "  " << key << "s_type" << reset << "=" << value << "0x" << std::hex
        << std::setw(2) << std::setfill('0') << static_cast<int>(stype_u8) << std::dec
        << reset << " (" << dim << stype_name_(msg.header.s_type) << reset << ")\n";
    oss << "  " << key << "system_bytes" << reset << "=" << value
        << fmt_hex_u32_(msg.header.system_bytes) << reset << '\n';

    if (msg.is_data()) {
        oss << "  " << key << "data" << reset << ": " << value << "S"
            << static_cast<int>(msg.stream()) << "F" << static_cast<int>(msg.function())
            << reset << " " << key << "W" << reset << "=" << value
            << (msg.w_bit() ? 1 : 0) << reset << '\n';
    }

    oss << "  " << key << "body" << reset << "=" << value << msg.body.size() << reset
        << " bytes\n";
}

void maybe_append_secs2_(std::ostringstream &oss,
                         const secs::hsms::Message &msg,
                         const HsmsDumpOptions &options) {
    const auto *reset = ansi_(options.enable_color, Ansi::reset);
    const auto *header = ansi_(options.enable_color, Ansi::header);
    const auto *key = ansi_(options.enable_color, Ansi::key);
    const auto *value = ansi_(options.enable_color, Ansi::value);
    const auto *dim = ansi_(options.enable_color, Ansi::dim);
    const auto *error = ansi_(options.enable_color, Ansi::error);

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
        oss << header << "SECS-II:" << reset << '\n';
        oss << "  " << error << "decode_failed" << reset << ": " << error
            << ec.message() << reset << '\n';
        return;
    }

    oss << header << "SECS-II:" << reset << '\n';
    oss << "  " << key << "consumed" << reset << "=" << value << consumed << reset
        << "/" << value << msg.body.size() << reset;
    if (consumed != msg.body.size()) {
        oss << ' ' << dim << "(not fully consumed)" << reset;
    }
    oss << '\n';
    oss << "  " << key << "item" << reset << ": " << dump_item(item, options.item)
        << '\n';
}

} // namespace

std::string dump_hsms_frame(secs::core::bytes_view frame,
                            HsmsDumpOptions options) {
    std::ostringstream oss;
    const bool enable_color = options.enable_color;
    const auto *reset = ansi_(enable_color, Ansi::reset);
    const auto *label = ansi_(enable_color, Ansi::label);
    const auto *dim = ansi_(enable_color, Ansi::dim);
    const auto *error = ansi_(enable_color, Ansi::error);

    if (options.include_hex) {
        oss << label << "RAW(HSMS frame):" << reset << '\n';
        oss << hex_dump(frame, options.hex);
    }

    secs::hsms::Message msg{};
    std::size_t consumed = 0;
    const auto ec = secs::hsms::decode_frame(frame, msg, consumed);
    if (ec) {
        oss << error << "HSMS decode_frame failed: " << ec.message() << reset << '\n';
        return oss.str();
    }

    oss << dim << "consumed=" << consumed << "/" << frame.size() << reset << '\n';
    append_message_summary_(oss, msg, enable_color);
    maybe_append_secs2_(oss, msg, options);
    return oss.str();
}

std::string dump_hsms_payload(secs::core::bytes_view payload,
                              HsmsDumpOptions options) {
    std::ostringstream oss;
    const bool enable_color = options.enable_color;
    const auto *reset = ansi_(enable_color, Ansi::reset);
    const auto *label = ansi_(enable_color, Ansi::label);
    const auto *error = ansi_(enable_color, Ansi::error);

    if (options.include_hex) {
        oss << label << "RAW(HSMS payload):" << reset << '\n';
        oss << hex_dump(payload, options.hex);
    }

    secs::hsms::Message msg{};
    const auto ec = secs::hsms::decode_payload(payload, msg);
    if (ec) {
        oss << error << "HSMS decode_payload failed: " << ec.message() << reset << '\n';
        return oss.str();
    }

    append_message_summary_(oss, msg, enable_color);
    maybe_append_secs2_(oss, msg, options);
    return oss.str();
}

} // namespace secs::utils
