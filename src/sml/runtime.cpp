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

            // 按 Stream/Function 索引：
            // - 匿名消息（name 为空）始终占优：与历史行为一致（优先按 sf_index_ 命中）。
            // - 命名消息仅在“该 SF 尚未有匿名定义”时进入索引；同 SF 多条命名消息时，
            //   保持“第一条命中”的兼容语义（历史实现为 O(N) 线性扫描并返回首个匹配）。
            std::uint16_t key = (static_cast<std::uint16_t>(msg.stream) << 8) |
                                static_cast<std::uint16_t>(msg.function);
            if (msg.name.empty()) {
                sf_index_[key] = i;
            } else if (sf_index_.find(key) == sf_index_.end()) {
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
    std::uint16_t key = (static_cast<std::uint16_t>(stream) << 8) |
                        static_cast<std::uint16_t>(function);
    auto it = sf_index_.find(key);
    if (it != sf_index_.end()) {
        return &document_.messages[it->second];
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

MatchResponseResult
Runtime::match_response_with_trace(std::uint8_t stream,
                                   std::uint8_t function,
                                   const ii::Item &item,
                                   const RenderContext &ctx) const noexcept {
    MatchResponseResult result;
    try {
        for (std::size_t i = 0; i < document_.conditions.size(); ++i) {
            const auto &rule = document_.conditions[i];

            MatchTrace trace;
            trace.rule_index = i;
            trace.condition_message_name = rule.condition.message_name;
            trace.condition_index = rule.condition.index;
            trace.condition_list_index = rule.condition.list_index;

            if (match_condition_with_trace(
                    rule.condition, stream, function, item, ctx, trace)) {
                result.response_name = rule.response_name;
                result.traces.clear(); // 命中时不返回失败轨迹
                return result;
            }

            result.traces.push_back(std::move(trace));
        }
        return result;
    } catch (...) {
        // noexcept：异常视为“未命中且无轨迹”（避免二次分配/字符串拼接导致再次失败）。
        return {};
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
    return match_condition_impl(cond, stream, function, item, ctx, nullptr);
}

bool Runtime::match_condition_with_trace(const Condition &cond,
                                         std::uint8_t stream,
                                         std::uint8_t function,
                                         const ii::Item &item,
                                         const RenderContext &ctx,
                                         MatchTrace &trace) const noexcept {
    return match_condition_impl(cond, stream, function, item, ctx, &trace);
}

bool Runtime::match_condition_impl(const Condition &cond,
                                   std::uint8_t stream,
                                   std::uint8_t function,
                                   const ii::Item &item,
                                   const RenderContext &ctx,
                                   MatchTrace *trace) const noexcept {
    // 检查消息名是否匹配
    // 条件可以是消息名（如 s1f1），也可以直接写成 SxFy 格式（如 S1F1）

    // 尝试解析为 SxFy
    std::uint8_t cond_stream = 0, cond_function = 0;
    const bool is_sf = parse_sf(cond.message_name, cond_stream, cond_function);

    // 如果是 SxFy 格式，直接比较
    if (is_sf) {
        if (stream != cond_stream || function != cond_function) {
            if (trace) {
                trace->reason = MatchFailureReason::stream_function_mismatch;
                trace->detail =
                    "incoming S" + std::to_string(stream) + "F" +
                    std::to_string(function) + " does not match condition " +
                    cond.message_name;
            }
            return false;
        }
    } else {
        // 按消息名查找
        const MessageDef *msg = get_message(cond.message_name);
        if (!msg) {
            if (trace) {
                trace->reason = MatchFailureReason::stream_function_mismatch;
                trace->detail =
                    "condition message name not found: " + cond.message_name;
            }
            return false;
        }
        if (msg->stream != stream || msg->function != function) {
            if (trace) {
                trace->reason = MatchFailureReason::stream_function_mismatch;
                trace->detail =
                    "incoming S" + std::to_string(stream) + "F" +
                    std::to_string(function) + " does not match condition " +
                    cond.message_name + " (S" + std::to_string(msg->stream) +
                    "F" + std::to_string(msg->function) + ")";
            }
            return false;
        }
    }

    // 如果有索引和期望值，检查 Item
    if ((cond.index || cond.list_index) && cond.expected) {
        const ii::Item *elem = nullptr;

        // [i] - 0-based List 数组下标（仅对“根节点为 List”的消息体生效）。
        if (cond.list_index.has_value()) {
            const auto *list = item.get_if<ii::List>();
            if (!list) {
                if (trace) {
                    trace->reason = MatchFailureReason::not_a_list;
                    trace->detail = "item is not a list";
                }
                return false;
            }
            if (*cond.list_index >= list->size()) {
                if (trace) {
                    trace->reason = MatchFailureReason::list_index_out_of_bounds;
                    trace->detail =
                        "list index " + std::to_string(*cond.list_index) +
                        " >= list size " + std::to_string(list->size());
                }
                return false;
            }
            elem = &(*list)[*cond.list_index];
        } else if (cond.index.has_value()) {
            // (n) - 1-based 先序遍历编号（包含根节点，保持向后兼容）。
            // 注意：仅当根节点为 List 时允许索引匹配（避免对非 List 输入产生歧义）。
            if (!item.get_if<ii::List>()) {
                if (trace) {
                    trace->reason = MatchFailureReason::index_out_of_bounds;
                    trace->detail =
                        "preorder index requires root item to be a list";
                }
                return false;
            }

            const std::size_t idx = *cond.index;
            if (idx < 1) {
                if (trace) {
                    trace->reason = MatchFailureReason::index_out_of_bounds;
                    trace->detail = "preorder index must be >= 1";
                }
                return false;
            }

            elem = find_preorder_nth(item, idx);
            if (!elem) {
                if (trace) {
                    trace->reason = MatchFailureReason::index_out_of_bounds;
                    trace->detail =
                        "preorder index " + std::to_string(idx) +
                        " out of bounds";
                }
                return false;
            }
        }

        ii::Item expected{ii::List{}};
        const auto render_ec = render_item(*cond.expected, ctx, expected);
        if (render_ec) {
            if (trace) {
                if (render_ec == render_errc::missing_variable) {
                    trace->reason = MatchFailureReason::render_missing_variable;
                    trace->detail =
                        "render expected value failed: missing variable";
                } else if (render_ec == render_errc::type_mismatch) {
                    trace->reason = MatchFailureReason::render_type_mismatch;
                    trace->detail =
                        "render expected value failed: type mismatch";
                } else {
                    trace->reason = MatchFailureReason::render_type_mismatch;
                    trace->detail =
                        "render expected value failed: " + render_ec.message();
                }
            }
            return false;
        }

        if (!elem || !items_equal(*elem, expected)) {
            if (trace) {
                trace->reason = MatchFailureReason::expected_value_mismatch;
                trace->detail = "expected value mismatch";
            }
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
