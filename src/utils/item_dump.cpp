#include "secs/utils/item_dump.hpp"

#include "secs/utils/hex.hpp"

#include <algorithm>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <sstream>
#include <type_traits>

namespace secs::utils {
namespace {

struct Ansi final {
    static constexpr const char *reset = "\033[0m";
    static constexpr const char *type = "\033[1;35m";
    static constexpr const char *string = "\033[1;32m";
    static constexpr const char *value = "\033[1;33m";
    static constexpr const char *dim = "\033[2m";
    static constexpr const char *error = "\033[1;31m";
};

[[nodiscard]] const char *ansi_(bool enable, const char *code) noexcept {
    return enable ? code : "";
}

struct DumpContext final {
    std::ostringstream oss;
    ItemDumpOptions options{};
};

[[nodiscard]] std::string indent_(std::size_t depth, std::size_t spaces) {
    return std::string(depth * spaces, ' ');
}

void append_escaped_ascii_(std::ostringstream &oss,
                           const std::string &s,
                           std::size_t max_bytes,
                           bool enable_color) {
    const auto *reset = ansi_(enable_color, Ansi::reset);
    const auto *string = ansi_(enable_color, Ansi::string);

    const std::size_t total = s.size();
    const std::size_t n = (max_bytes == 0 ? total : std::min(total, max_bytes));

    oss << string;
    oss << '"';
    for (std::size_t i = 0; i < n; ++i) {
        const auto c = static_cast<unsigned char>(s[i]);
        if (c == '\\') {
            oss << "\\\\";
            continue;
        }
        if (c == '"') {
            oss << "\\\"";
            continue;
        }
        if (c >= 0x20 && c <= 0x7E) {
            oss << static_cast<char>(c);
            continue;
        }
        // 非可打印字符：用 \xHH。
        oss << "\\x" << std::hex << std::setw(2) << std::setfill('0')
            << static_cast<int>(c) << std::dec;
    }
    if (max_bytes != 0 && total > max_bytes) {
        oss << "...";
    }
    oss << '"';
    oss << reset;
}

template <class T>
void append_array_(std::ostringstream &oss,
                   const char *type_name,
                   const std::vector<T> &values,
                   std::size_t max_items,
                   bool enable_color) {
    static_assert(std::is_arithmetic_v<T>, "T must be arithmetic");
    const auto *reset = ansi_(enable_color, Ansi::reset);
    const auto *type = ansi_(enable_color, Ansi::type);
    const auto *value = ansi_(enable_color, Ansi::value);
    const auto *dim = ansi_(enable_color, Ansi::dim);

    const std::size_t total = values.size();
    const std::size_t n = (max_items == 0 ? total : std::min(total, max_items));

    oss << type << type_name << '[' << total << ']' << reset;
    if (total == 0) {
        return;
    }
    oss << ' ';
    oss << value;
    for (std::size_t i = 0; i < n; ++i) {
        if constexpr (std::is_floating_point_v<T>) {
            // 浮点默认用较高精度，便于定位差异。
            oss << std::setprecision(10) << values[i];
        } else {
            oss << values[i];
        }
        if (i + 1 != n) {
            oss << ' ';
        }
    }
    oss << reset;
    if (max_items != 0 && total > max_items) {
        oss << ' ' << dim << "..." << reset;
    }
}

void append_bool_array_(std::ostringstream &oss,
                        const std::vector<bool> &values,
                        std::size_t max_items,
                        bool enable_color) {
    const auto *reset = ansi_(enable_color, Ansi::reset);
    const auto *type = ansi_(enable_color, Ansi::type);
    const auto *value_color = ansi_(enable_color, Ansi::value);
    const auto *dim = ansi_(enable_color, Ansi::dim);

    const std::size_t total = values.size();
    const std::size_t n = (max_items == 0 ? total : std::min(total, max_items));

    oss << type << "BOOLEAN[" << total << ']' << reset;
    if (total == 0) {
        return;
    }
    oss << ' ';
    oss << value_color;
    for (std::size_t i = 0; i < n; ++i) {
        oss << (values[i] ? '1' : '0');
        if (i + 1 != n) {
            oss << ' ';
        }
    }
    oss << reset;
    if (max_items != 0 && total > max_items) {
        oss << ' ' << dim << "..." << reset;
    }
}

void append_binary_(std::ostringstream &oss,
                    const std::vector<secs::ii::byte> &bytes,
                    std::size_t max_bytes,
                    bool enable_color) {
    const auto *reset = ansi_(enable_color, Ansi::reset);
    const auto *type = ansi_(enable_color, Ansi::type);
    const auto *value_color = ansi_(enable_color, Ansi::value);
    const auto *dim = ansi_(enable_color, Ansi::dim);

    const std::size_t total = bytes.size();
    const std::size_t n = (max_bytes == 0 ? total : std::min(total, max_bytes));

    oss << type << "BINARY[" << total << ']' << reset;
    if (total == 0) {
        return;
    }
    oss << ' ';

    oss << value_color;
    for (std::size_t i = 0; i < n; ++i) {
        oss << std::hex << std::setw(2) << std::setfill('0')
            << static_cast<int>(bytes[i]) << std::dec;
        if (i + 1 != n) {
            oss << ' ';
        }
    }
    oss << reset;
    if (max_bytes != 0 && total > max_bytes) {
        oss << ' ' << dim << "..." << reset;
    }
}

void append_item_(DumpContext &ctx, const secs::ii::Item &item, std::size_t depth);

void append_list_(DumpContext &ctx, const secs::ii::List &list, std::size_t depth) {
    const auto &opt = ctx.options;
    const bool enable_color = opt.enable_color;
    const auto *reset = ansi_(enable_color, Ansi::reset);
    const auto *type = ansi_(enable_color, Ansi::type);
    const auto *dim = ansi_(enable_color, Ansi::dim);

    const std::size_t total = list.size();
    const std::size_t n =
        (opt.max_list_items == 0 ? total : std::min(total, opt.max_list_items));

    ctx.oss << type << "L[" << total << ']' << reset;

    if (total == 0) {
        return;
    }

    if (depth >= opt.max_depth) {
        ctx.oss << ' ' << dim << "..." << reset;
        return;
    }

    if (!opt.multiline) {
        ctx.oss << ' ' << dim << "{ " << reset;
        for (std::size_t i = 0; i < n; ++i) {
            append_item_(ctx, list[i], depth + 1);
            if (i + 1 != n) {
                ctx.oss << ", ";
            }
        }
        if (opt.max_list_items != 0 && total > opt.max_list_items) {
            ctx.oss << ", " << dim << "..." << reset;
        }
        ctx.oss << ' ' << dim << '}' << reset;
        return;
    }

    ctx.oss << ' ' << dim << "{\n" << reset;
    for (std::size_t i = 0; i < n; ++i) {
        ctx.oss << indent_(depth + 1, opt.indent_spaces);
        append_item_(ctx, list[i], depth + 1);
        ctx.oss << '\n';
    }
    if (opt.max_list_items != 0 && total > opt.max_list_items) {
        ctx.oss << indent_(depth + 1, opt.indent_spaces) << dim << "..." << reset
                << '\n';
    }
    ctx.oss << indent_(depth, opt.indent_spaces) << dim << '}' << reset;
}

void append_item_(DumpContext &ctx, const secs::ii::Item &item, std::size_t depth) {
    const bool enable_color = ctx.options.enable_color;
    const auto *reset = ansi_(enable_color, Ansi::reset);
    const auto *type = ansi_(enable_color, Ansi::type);
    const auto *error = ansi_(enable_color, Ansi::error);

    std::visit(
        [&](const auto &v) {
            using T = std::decay_t<decltype(v)>;

            if constexpr (std::is_same_v<T, secs::ii::List>) {
                append_list_(ctx, v, depth);
            } else if constexpr (std::is_same_v<T, secs::ii::ASCII>) {
                ctx.oss << type << "A[" << v.value.size() << ']' << reset << ' ';
                append_escaped_ascii_(
                    ctx.oss,
                    v.value,
                    ctx.options.max_payload_bytes,
                    enable_color);
            } else if constexpr (std::is_same_v<T, secs::ii::Binary>) {
                append_binary_(
                    ctx.oss,
                    v.value,
                    ctx.options.max_payload_bytes,
                    enable_color);
            } else if constexpr (std::is_same_v<T, secs::ii::Boolean>) {
                append_bool_array_(
                    ctx.oss,
                    v.values,
                    ctx.options.max_array_items,
                    enable_color);
            } else if constexpr (std::is_same_v<T, secs::ii::I1>) {
                append_array_(
                    ctx.oss,
                    "I1",
                    v.values,
                    ctx.options.max_array_items,
                    enable_color);
            } else if constexpr (std::is_same_v<T, secs::ii::I2>) {
                append_array_(
                    ctx.oss,
                    "I2",
                    v.values,
                    ctx.options.max_array_items,
                    enable_color);
            } else if constexpr (std::is_same_v<T, secs::ii::I4>) {
                append_array_(
                    ctx.oss,
                    "I4",
                    v.values,
                    ctx.options.max_array_items,
                    enable_color);
            } else if constexpr (std::is_same_v<T, secs::ii::I8>) {
                append_array_(
                    ctx.oss,
                    "I8",
                    v.values,
                    ctx.options.max_array_items,
                    enable_color);
            } else if constexpr (std::is_same_v<T, secs::ii::U1>) {
                append_array_(
                    ctx.oss,
                    "U1",
                    v.values,
                    ctx.options.max_array_items,
                    enable_color);
            } else if constexpr (std::is_same_v<T, secs::ii::U2>) {
                append_array_(
                    ctx.oss,
                    "U2",
                    v.values,
                    ctx.options.max_array_items,
                    enable_color);
            } else if constexpr (std::is_same_v<T, secs::ii::U4>) {
                append_array_(
                    ctx.oss,
                    "U4",
                    v.values,
                    ctx.options.max_array_items,
                    enable_color);
            } else if constexpr (std::is_same_v<T, secs::ii::U8>) {
                append_array_(
                    ctx.oss,
                    "U8",
                    v.values,
                    ctx.options.max_array_items,
                    enable_color);
            } else if constexpr (std::is_same_v<T, secs::ii::F4>) {
                append_array_(
                    ctx.oss,
                    "F4",
                    v.values,
                    ctx.options.max_array_items,
                    enable_color);
            } else if constexpr (std::is_same_v<T, secs::ii::F8>) {
                append_array_(
                    ctx.oss,
                    "F8",
                    v.values,
                    ctx.options.max_array_items,
                    enable_color);
            } else {
                ctx.oss << error << "(unknown item)" << reset;
            }
        },
        item.storage());
}

} // namespace

std::string dump_item(const secs::ii::Item &item, ItemDumpOptions options) {
    DumpContext ctx;
    ctx.options = options;

    append_item_(ctx, item, 0);
    return ctx.oss.str();
}

} // namespace secs::utils
