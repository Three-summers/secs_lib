#pragma once

#include "secs/sml/ast.hpp"
#include "secs/sml/lexer.hpp"
#include "secs/sml/parser.hpp"

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <vector>

namespace secs::sml {

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
     * @param name 消息名称
     * @return 消息定义指针，未找到返回 nullptr
     */
    [[nodiscard]] const MessageDef *
    get_message(std::string_view name) const noexcept;

    /**
     * @brief 通过 Stream/Function 获取消息
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

    void build_index() noexcept;
    [[nodiscard]] bool match_condition(const Condition &cond,
                                       std::uint8_t stream,
                                       std::uint8_t function,
                                       const ii::Item &item) const noexcept;
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
}

} // namespace secs::sml
