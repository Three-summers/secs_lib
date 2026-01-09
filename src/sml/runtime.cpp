#include "secs/sml/runtime.hpp"

#include "secs/ii/codec.hpp"
#include "secs/sml/render.hpp"

#include <charconv>
#include <cmath>
#include <limits>
#include <new>

namespace secs::sml {

/*
 * SML Runtime 实现：在已解析的 Document 上提供查询与匹配能力。
 *
 * 主要职责：
 * - build_index()：构建 “name -> tell index” 与 “(S,F) -> index” 的索引，便于 O(1)
 *   查找消息模板；
 * - match_response()：按条件规则匹配入站消息，返回对应的响应消息名；
 * - items_equal()：为条件匹配提供 Item 比较语义（其中浮点采用容差比较，提高规则
 *   易用性；其它类型复用 ii::Item 的严格相等）。
 *
 * 与 SECS-II 的关系：
 * - Runtime 处理的是“结构化 Item”（ii::Item），不直接处理 on-wire 编解码；
 * - 需要从字节解析时，应先用 ii::codec 解码为 Item，再交给 Runtime 匹配。
 */

namespace {

[[nodiscard]] bool parse_sf(std::string_view name,
                            std::uint8_t &stream,
                            std::uint8_t &function) noexcept {
    if (name.size() < 4) {
        return false;
    }
    if (name[0] != 'S' && name[0] != 's') {
        return false;
    }

    const auto f_pos = name.find_first_of("Ff");
    if (f_pos == std::string_view::npos || f_pos < 2) {
        return false;
    }

    int s = 0;
    int f = 0;
    const auto *s_begin = name.data() + 1;
    const auto *s_end = name.data() + f_pos;
    auto [s_ptr, s_ec] = std::from_chars(s_begin, s_end, s);
    if (s_ec != std::errc{} || s_ptr != s_end) {
        return false;
    }

    const auto *f_begin = name.data() + f_pos + 1;
    const auto *f_end = name.data() + name.size();
    auto [f_ptr, f_ec] = std::from_chars(f_begin, f_end, f);
    if (f_ec != std::errc{} || f_ptr != f_end) {
        return false;
    }

    if (s < 0 || s > 127 || f < 0 || f > 255) {
        return false;
    }
    stream = static_cast<std::uint8_t>(s);
    function = static_cast<std::uint8_t>(f);
    return true;
}

[[nodiscard]] bool float_almost_equal(float a, float b) noexcept {
    constexpr float kAbsTol = 0.0001f;
    return std::fabs(a - b) <= kAbsTol;
}

[[nodiscard]] bool double_almost_equal(double a, double b) noexcept {
    constexpr double kAbsTol = 0.0001;
    return std::fabs(a - b) <= kAbsTol;
}

// 按先序遍历查找第 N 个 Item（1-based，包含根节点）。
// 约定：
// - 仅在根节点为 List 时用于条件匹配（与历史行为保持一致）；
// - 遍历顺序：node -> children（List 的子元素从左到右）。
[[nodiscard]] const ii::Item *
find_preorder_nth(const ii::Item &root, std::size_t n) noexcept {
    if (n < 1) {
        return nullptr;
    }

    std::size_t cur = 0;
    const ii::Item *found = nullptr;

    struct Walk final {
        static bool run(const ii::Item &item,
                        std::size_t n,
                        std::size_t &cur,
                        const ii::Item *&found) noexcept {
            ++cur;
            if (cur == n) {
                found = &item;
                return true;
            }
            if (const auto *list = item.get_if<ii::List>()) {
                for (const auto &child : *list) {
                    if (run(child, n, cur, found)) {
                        return true;
                    }
                }
            }
            return false;
        }
    };

    (void)Walk::run(root, n, cur, found);
    return found;
}

} // namespace

std::error_code Runtime::load(std::string_view source) noexcept {
    try {
        auto result = parse_sml(source);
        if (result.ec) {
            return result.ec;
        }

        load(std::move(result.document));
        if (!loaded_) {
            return secs::core::make_error_code(secs::core::errc::out_of_memory);
        }
        return {};
    } catch (const std::bad_alloc &) {
        return secs::core::make_error_code(secs::core::errc::out_of_memory);
    } catch (...) {
        return secs::core::make_error_code(secs::core::errc::invalid_argument);
    }
}

void Runtime::load(Document doc) noexcept {
    try {
        document_ = std::move(doc);
        loaded_ = build_index();
    } catch (...) {
        loaded_ = false;
    }
}

bool Runtime::build_index() noexcept {
    name_index_.clear();
    sf_index_.clear();
    try {
        for (std::size_t i = 0; i < document_.messages.size(); ++i) {
            const auto &msg = document_.messages[i];

            // 按名称索引
            if (!msg.name.empty()) {
                name_index_[msg.name] = i;
            }

            // 按 Stream/Function 索引（仅匿名消息）
            if (msg.name.empty()) {
                std::uint16_t key =
                    (static_cast<std::uint16_t>(msg.stream) << 8) |
                    static_cast<std::uint16_t>(msg.function);
                sf_index_[key] = i;
            }
        }
        return true;
    } catch (...) {
        name_index_.clear();
        sf_index_.clear();
        return false;
    }
}

const MessageDef *Runtime::get_message(std::string_view name) const noexcept {
    auto it = name_index_.find(name);
    if (it != name_index_.end()) {
        return &document_.messages[it->second];
    }

    // 兼容：允许直接用 "SxFy" 形式查找（例如 sample.sml 中的条件响应常写成 s2f22）。
    std::uint8_t stream = 0;
    std::uint8_t function = 0;
    if (parse_sf(name, stream, function)) {
        return get_message(stream, function);
    }
    return nullptr;
}

const MessageDef *Runtime::get_message(std::uint8_t stream,
                                       std::uint8_t function) const noexcept {
    // 先尝试按 SF 查找匿名消息
    std::uint16_t key = (static_cast<std::uint16_t>(stream) << 8) |
                        static_cast<std::uint16_t>(function);
    auto it = sf_index_.find(key);
    if (it != sf_index_.end()) {
        return &document_.messages[it->second];
    }

    // 遍历查找命名消息
    for (const auto &msg : document_.messages) {
        if (msg.stream == stream && msg.function == function) {
            return &msg;
        }
    }

    return nullptr;
}

std::optional<std::string>
Runtime::match_response(std::uint8_t stream,
                        std::uint8_t function,
                        const ii::Item &item) const noexcept {
    try {
        RenderContext ctx{};
        for (const auto &rule : document_.conditions) {
            if (match_condition(rule.condition, stream, function, item, ctx)) {
                return rule.response_name;
            }
        }
        return std::nullopt;
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<std::string>
Runtime::match_response(std::uint8_t stream,
                        std::uint8_t function,
                        const ii::Item &item,
                        const RenderContext &ctx) const noexcept {
    try {
        for (const auto &rule : document_.conditions) {
            if (match_condition(rule.condition, stream, function, item, ctx)) {
                return rule.response_name;
            }
        }
        return std::nullopt;
    } catch (...) {
        return std::nullopt;
    }
}

std::error_code
Runtime::encode_message_body(std::string_view name_or_sf,
                             const RenderContext &ctx,
                             std::vector<secs::core::byte> &out_body,
                             std::uint8_t *out_stream,
                             std::uint8_t *out_function,
                             bool *out_w_bit) const noexcept {
    try {
        out_body.clear();

        const auto *msg = get_message(name_or_sf);
        if (!msg) {
            return secs::core::make_error_code(secs::core::errc::invalid_argument);
        }

        secs::ii::Item rendered{secs::ii::List{}};
        const auto render_ec = secs::sml::render_item(msg->item, ctx, rendered);
        if (render_ec) {
            return render_ec;
        }

        const auto enc_ec = secs::ii::encode(rendered, out_body);
        if (enc_ec) {
            return enc_ec;
        }

        if (out_stream) {
            *out_stream = msg->stream;
        }
        if (out_function) {
            *out_function = msg->function;
        }
        if (out_w_bit) {
            *out_w_bit = msg->w_bit;
        }
        return {};
    } catch (const std::bad_alloc &) {
        return secs::core::make_error_code(secs::core::errc::out_of_memory);
    } catch (...) {
        return secs::core::make_error_code(secs::core::errc::invalid_argument);
    }
}

bool Runtime::match_condition(const Condition &cond,
                              std::uint8_t stream,
                              std::uint8_t function,
                              const ii::Item &item,
                              const RenderContext &ctx) const noexcept {
    // 检查消息名是否匹配
    // 条件可以是消息名（如 s1f1），也可以直接写成 SxFy 格式（如 S1F1）

    // 尝试解析为 SxFy
    std::uint8_t cond_stream = 0, cond_function = 0;
    const bool is_sf = parse_sf(cond.message_name, cond_stream, cond_function);

    // 如果是 SxFy 格式，直接比较
    if (is_sf) {
        if (stream != cond_stream || function != cond_function) {
            return false;
        }
    } else {
        // 按消息名查找
        const MessageDef *msg = get_message(cond.message_name);
        if (!msg || msg->stream != stream || msg->function != function) {
            return false;
        }
    }

    // 如果有索引和期望值，检查 Item
    if (cond.index && cond.expected) {
        // 兼容 sample.sml：索引采用“先序遍历编号（包含根节点）”。
        // 注意：仅当根节点为 List 时允许索引匹配（避免对非 List 输入产生歧义）。
        if (!item.get_if<ii::List>()) {
            return false;
        }

        const std::size_t idx = *cond.index;
        const ii::Item *elem = find_preorder_nth(item, idx);
        if (!elem) {
            return false;
        }

        ii::Item expected{ii::List{}};
        if (render_item(*cond.expected, ctx, expected)) {
            return false;
        }

        if (!items_equal(*elem, expected)) {
            return false;
        }
    }

    return true;
}

bool Runtime::items_equal(const ii::Item &a, const ii::Item &b) const noexcept {
    // 优先对浮点做容差比较（提升规则匹配的易用性，避免设备端小误差导致无法命中）。
    if (const auto *af4 = a.get_if<ii::F4>()) {
        const auto *bf4 = b.get_if<ii::F4>();
        if (!bf4) {
            return false;
        }
        if (af4->values.size() != bf4->values.size()) {
            return false;
        }
        for (std::size_t i = 0; i < af4->values.size(); ++i) {
            if (!float_almost_equal(af4->values[i], bf4->values[i])) {
                return false;
            }
        }
        return true;
    }

    if (const auto *af8 = a.get_if<ii::F8>()) {
        const auto *bf8 = b.get_if<ii::F8>();
        if (!bf8) {
            return false;
        }
        if (af8->values.size() != bf8->values.size()) {
            return false;
        }
        for (std::size_t i = 0; i < af8->values.size(); ++i) {
            if (!double_almost_equal(af8->values[i], bf8->values[i])) {
                return false;
            }
        }
        return true;
    }

    // 其余类型直接复用 ii::Item 的严格比较（支持
    // List、Binary、Boolean、整数等）。
    return a == b;
}

} // namespace secs::sml
