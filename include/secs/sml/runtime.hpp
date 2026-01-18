#pragma once

#include "secs/core/error.hpp"
#include "secs/sml/ast.hpp"
#include "secs/sml/lexer.hpp"
#include "secs/sml/parser.hpp"

#include <chrono>
#include <functional>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <vector>

namespace secs::sml {

class RenderContext;

/**
 * @brief 条件匹配失败原因（用于调试与错误回溯）
 */
enum class MatchFailureReason : std::uint8_t {
    stream_function_mismatch,
    index_out_of_bounds,
    list_index_out_of_bounds,
    render_missing_variable,
    render_type_mismatch,
    expected_value_mismatch,
    not_a_list, // [index] 用于非 List
};

/**
 * @brief 单条条件规则的匹配轨迹
 *
 * 说明：
 * - `rule_index` 为 document_.conditions 的下标；
 * - `detail` 为可读解释（便于打印日志或 UI 展示）。
 */
struct MatchTrace final {
    std::size_t rule_index{0};
    std::string condition_message_name;
    std::optional<std::size_t> condition_index;
    std::optional<std::size_t> condition_list_index;
    MatchFailureReason reason{MatchFailureReason::stream_function_mismatch};
    std::string detail;
};

/**
 * @brief 条件响应匹配结果（带失败轨迹）
 */
struct MatchResponseResult final {
    std::optional<std::string> response_name;
    std::vector<MatchTrace> traces; // 命中时为空
};

/**
 * @brief SML 运行时
 *
 * 提供:
 * - 消息模板查找 (O(1))
 * - 条件响应匹配
 * - 定时规则访问
 */
class Runtime {
public:
    Runtime() = default;

    /**
     * @brief 从 SML 源文本加载
     */
    [[nodiscard]] std::error_code load(std::string_view source) noexcept;

    /**
     * @brief 从已解析的文档加载
     */
    void load(Document doc) noexcept;

    /**
     * @brief 获取消息模板
     * @param name 消息名称；也支持直接传入 SxFy（例如 "S2F22"）
     * @return 消息定义指针，未找到返回 nullptr
     */
    [[nodiscard]] const MessageDef *
    get_message(std::string_view name) const noexcept;

    /**
     * @brief 通过 Stream/Function 获取消息
     *
     * 选择规则（用于处理“同一 (S,F) 出现多条定义”的情况）：
     * - 若存在匿名消息（name 为空）：优先返回匿名定义（与历史行为一致）；
     * - 否则：返回第一条匹配的命名消息。
     */
    [[nodiscard]] const MessageDef *
    get_message(std::uint8_t stream, std::uint8_t function) const noexcept;

    /**
     * @brief 匹配条件响应
     * @param stream 收到的 Stream
     * @param function 收到的 Function
     * @param item 收到的消息体
     * @return 匹配的响应消息名，无匹配返回 nullopt
     */
    [[nodiscard]] std::optional<std::string>
    match_response(std::uint8_t stream,
                   std::uint8_t function,
                   const ii::Item &item) const noexcept;

    /**
     * @brief 匹配条件响应（带渲染上下文）
     *
     * 说明：
     * - 条件期望值 `==<...>` 允许使用占位符（Identifier），匹配时会先用 ctx
     *   渲染期望值再做比较；
     * - 若期望值渲染失败（缺失变量/类型不匹配），该规则视为“不命中”。
     */
    [[nodiscard]] std::optional<std::string>
    match_response(std::uint8_t stream,
                   std::uint8_t function,
                   const ii::Item &item,
                   const RenderContext &ctx) const noexcept;

    /**
     * @brief 匹配条件响应并捕获数据（Data Capture）
     *
     * 行为：
     * - 若命中规则且该规则的条件包含 `<pattern>`（不带 `==`），则把 pattern 内的
     *   `$NAME` 捕获到 out_captures；
     * - out_captures 在调用前会被 clear()；
     * - 未命中时返回 nullopt，out_captures 为空。
     *
     * 说明：
     * - ctx 仍用于旧的 `==<Item>` 期望值渲染（占位符变量）；
     * - `<pattern>` 的捕获变量名称不包含 '$'（例如 `$CEID` -> "CEID"）。
     */
    [[nodiscard]] std::optional<std::string>
    match_response_with_capture(std::uint8_t stream,
                                std::uint8_t function,
                                const ii::Item &item,
                                const RenderContext &ctx,
                                RenderContext &out_captures) const noexcept;

    /**
     * @brief match_response_with_capture() 的便捷重载：使用空 RenderContext
     */
    [[nodiscard]] std::optional<std::string>
    match_response_with_capture(std::uint8_t stream,
                                std::uint8_t function,
                                const ii::Item &item,
                                RenderContext &out_captures) const noexcept;

    /**
     * @brief 匹配条件响应（返回详细失败轨迹）
     *
     * 约定：
     * - 命中规则时：`response_name` 有值，且 `traces` 为空；
     * - 未命中时：`response_name` 为空，`traces` 包含每条规则的失败原因与详情。
     */
    [[nodiscard]] MatchResponseResult
    match_response_with_trace(std::uint8_t stream,
                              std::uint8_t function,
                              const ii::Item &item,
                              const RenderContext &ctx) const noexcept;

    /**
     * @brief 渲染并编码消息模板（用于“代码主动发送”）
     *
     * @param name_or_sf 消息名；也支持直接传入 SxFy（例如 "S2F22"）
     * @param ctx 渲染上下文（变量名 -> SECS-II Item）
     * @param out_body 输出：编码后的 SECS-II body bytes
     * @param out_stream 可选输出：Stream
     * @param out_function 可选输出：Function
     * @param out_w_bit 可选输出：W 位
     *
     * @return 渲染失败返回 sml.render；编码失败返回 ii::errc；找不到消息返回
     * secs.core/invalid_argument。
     */
    [[nodiscard]] std::error_code
    encode_message_body(std::string_view name_or_sf,
                        const RenderContext &ctx,
                        std::vector<secs::core::byte> &out_body,
                        std::uint8_t *out_stream = nullptr,
                        std::uint8_t *out_function = nullptr,
                        bool *out_w_bit = nullptr) const noexcept;

    /**
     * @brief 获取所有定时规则
     */
    [[nodiscard]] const std::vector<TimerRule> &timers() const noexcept {
        return document_.timers;
    }

    /**
     * @brief 获取所有消息定义
     */
    [[nodiscard]] const std::vector<MessageDef> &messages() const noexcept {
        return document_.messages;
    }

    /**
     * @brief 获取所有条件规则
     */
    [[nodiscard]] const std::vector<ConditionRule> &
    conditions() const noexcept {
        return document_.conditions;
    }

    /**
     * @brief 检查是否已加载
     */
    [[nodiscard]] bool loaded() const noexcept { return loaded_; }

private:
    struct TransparentStringHash {
        using is_transparent = void;

        std::size_t operator()(std::string_view sv) const noexcept {
            return std::hash<std::string_view>{}(sv);
        }

        std::size_t operator()(const std::string &s) const noexcept {
            return std::hash<std::string_view>{}(s);
        }
    };

    [[nodiscard]] bool build_index() noexcept;
    [[nodiscard]] bool match_condition(const Condition &cond,
                                       std::uint8_t stream,
                                       std::uint8_t function,
                                       const ii::Item &item,
                                       const RenderContext &ctx) const noexcept;
    [[nodiscard]] bool match_condition_with_trace(const Condition &cond,
                                                  std::uint8_t stream,
                                                  std::uint8_t function,
                                                  const ii::Item &item,
                                                  const RenderContext &ctx,
                                                  MatchTrace &trace) const noexcept;
    [[nodiscard]] bool match_condition_impl(const Condition &cond,
                                            std::uint8_t stream,
                                            std::uint8_t function,
                                            const ii::Item &item,
                                            const RenderContext &ctx,
                                            MatchTrace *trace,
                                            RenderContext *out_captures) const noexcept;
    [[nodiscard]] bool items_equal(const ii::Item &a,
                                   const ii::Item &b) const noexcept;

    Document document_;
    std::unordered_map<std::string,
                       std::size_t,
                       TransparentStringHash,
                       std::equal_to<>>
        name_index_; // 消息名 -> messages 下标（支持 std::string_view
                     // 透明查找，避免临时分配）
    std::unordered_map<std::uint16_t, std::size_t>
        sf_index_; // (stream<<8|function) -> messages 下标
    bool loaded_{false};
};

/**
 * @brief 便捷函数：解析 SML 源文本
 */
[[nodiscard]] inline ParseResult parse_sml(std::string_view source) noexcept {
    try {
        Lexer lexer(source);
        auto lex_result = lexer.tokenize();
        if (lex_result.ec) {
            ParseResult result;
            result.ec = lex_result.ec;
            result.error_line = lex_result.error_line;
            result.error_column = lex_result.error_column;
            result.error_message = std::move(lex_result.error_message);
            return result;
        }

        Parser parser(std::move(lex_result.tokens));
        return parser.parse();
    } catch (const std::bad_alloc &) {
        ParseResult result;
        result.ec = secs::core::make_error_code(secs::core::errc::out_of_memory);
        result.error_message = "out of memory";
        return result;
    } catch (...) {
        ParseResult result;
        result.ec = secs::core::make_error_code(secs::core::errc::invalid_argument);
        result.error_message = "unexpected exception";
        return result;
    }
}

} // namespace secs::sml
