#include "secs/utils/hex.hpp"

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <sstream>

namespace secs::utils {
namespace {

struct Ansi final {
    static constexpr const char *reset = "\033[0m";
    static constexpr const char *dim = "\033[2m";
    static constexpr const char *bytes = "\033[1;33m";
    static constexpr const char *ascii = "\033[1;32m";
    static constexpr const char *error = "\033[1;31m";
};

[[nodiscard]] const char *ansi_(bool enable, const char *code) noexcept {
    return enable ? code : "";
}

[[nodiscard]] bool is_hex_digit_(unsigned char c) noexcept {
    return std::isxdigit(c) != 0;
}

[[nodiscard]] std::uint8_t hex_value_(unsigned char c) noexcept {
    if (c >= '0' && c <= '9') {
        return static_cast<std::uint8_t>(c - '0');
    }
    if (c >= 'a' && c <= 'f') {
        return static_cast<std::uint8_t>(c - 'a' + 10);
    }
    if (c >= 'A' && c <= 'F') {
        return static_cast<std::uint8_t>(c - 'A' + 10);
    }
    return 0;
}

[[nodiscard]] bool is_separator_(unsigned char c) noexcept {
    if (std::isspace(c) != 0) {
        return true;
    }
    switch (c) {
    case ',':
    case ';':
    case ':':
    case '-':
    case '_':
    case '|':
    case '/':
    case '\\':
    case '[':
    case ']':
    case '(':
    case ')':
    case '{':
    case '}':
    case '<':
    case '>':
    case '\'':
    case '"':
        return true;
    default:
        return false;
    }
}

[[nodiscard]] char to_printable_ascii_(secs::core::byte b) noexcept {
    const auto c = static_cast<unsigned char>(b);
    if (c >= 0x20 && c <= 0x7E) {
        return static_cast<char>(c);
    }
    return '.';
}

} // namespace

std::string hex_dump(secs::core::bytes_view bytes, HexDumpOptions options) {
    std::ostringstream oss;
    const bool enable_color = options.enable_color;
    const auto *reset = ansi_(enable_color, Ansi::reset);
    const auto *dim = ansi_(enable_color, Ansi::dim);
    const auto *bytes_color = ansi_(enable_color, Ansi::bytes);
    const auto *ascii_color = ansi_(enable_color, Ansi::ascii);
    const auto *error = ansi_(enable_color, Ansi::error);

    const std::size_t total = bytes.size();
    const std::size_t max_bytes =
        (options.max_bytes == 0 ? total : std::min(total, options.max_bytes));
    const std::size_t per_line = (options.bytes_per_line == 0
                                      ? static_cast<std::size_t>(16)
                                      : options.bytes_per_line);

    for (std::size_t offset = 0; offset < max_bytes; offset += per_line) {
        const std::size_t line_n = std::min(per_line, max_bytes - offset);

        if (options.show_offset) {
            oss << dim;
            oss << std::setw(4) << std::setfill('0') << std::hex << offset
                << ": ";
            oss << reset;
        }

        oss << bytes_color;
        for (std::size_t i = 0; i < line_n; ++i) {
            const auto b = bytes[offset + i];
            oss << std::setw(2) << std::setfill('0') << std::hex
                << static_cast<int>(b);
            if (i + 1 != line_n) {
                oss << ' ';
            }
        }
        oss << reset;

        if (options.show_ascii) {
            // 对齐：补齐未输出的字节位，保证 ASCII 列对齐。
            if (line_n < per_line) {
                const std::size_t missing = per_line - line_n;
                // 每个 byte 输出 "HH "（末尾可能无空格），这里粗略补齐 3*missing。
                oss << std::string(missing * 3, ' ');
            } else {
                oss << ' ';
            }
            oss << "  ";
            oss << ascii_color;
            for (std::size_t i = 0; i < line_n; ++i) {
                oss << to_printable_ascii_(bytes[offset + i]);
            }
            oss << reset;
        }

        oss << '\n';
    }

    if (options.max_bytes != 0 && total > options.max_bytes) {
        oss << error << "... (truncated, total=" << std::dec << total << " bytes)"
            << reset << '\n';
    }

    return oss.str();
}

std::error_code parse_hex(std::string_view text,
                          std::vector<secs::core::byte> &out) noexcept {
    out.clear();

    int hi_nibble = -1;

    for (std::size_t i = 0; i < text.size(); ++i) {
        const auto c = static_cast<unsigned char>(text[i]);

        if (is_separator_(c)) {
            continue;
        }

        // 支持可选 0x/0X 前缀：忽略 '0' 后紧跟的 'x'/'X'。
        if (c == '0' && (i + 1) < text.size()) {
            const auto n = static_cast<unsigned char>(text[i + 1]);
            if (n == 'x' || n == 'X') {
                ++i;
                continue;
            }
        }

        if (!is_hex_digit_(c)) {
            return secs::core::make_error_code(secs::core::errc::invalid_argument);
        }

        const auto v = static_cast<int>(hex_value_(c));
        if (hi_nibble < 0) {
            hi_nibble = v;
            continue;
        }

        const auto byte =
            static_cast<secs::core::byte>(((hi_nibble & 0x0F) << 4) | (v & 0x0F));
        out.push_back(byte);
        hi_nibble = -1;
    }

    // 16 进制必须是偶数字节（两个 nibble 组成一个 byte）。
    if (hi_nibble >= 0) {
        return secs::core::make_error_code(secs::core::errc::invalid_argument);
    }

    return {};
}

} // namespace secs::utils
