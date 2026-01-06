#pragma once

#include "secs/ii/item.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace secs::sml {

/**
 * @brief 消息定义
 *
 * 格式：名称: SxFy [W] <Item>.
 */
struct MessageDef {
    std::string name;         // 消息名称（可为空，表示匿名）
    std::uint8_t stream{0};   // Stream 号
    std::uint8_t function{0}; // Function 号
    bool w_bit{false};        // W 位（等待位）
    ii::Item item;            // 消息体

    MessageDef() : item(ii::List{}) {}
};

/**
 * @brief 条件表达式
 *
 * 格式：消息名[(index)][==<Item>]
 */
struct Condition {
    std::string message_name;         // 触发消息名或 SxFy
    // 可选索引（从 1 开始）：
    // - 采用 SECS-II Item 的先序遍历编号（包含根节点）。
    // - 若消息体为 <L ...>，则根 List 的编号为 1，第一个子元素编号为 2。
    std::optional<std::size_t> index;
    std::optional<ii::Item> expected; // 可选的期望值
};

/**
 * @brief 条件响应规则
 *
 * 格式：if (条件) 响应消息名.
 */
struct ConditionRule {
    Condition condition;
    std::string response_name; // 响应消息名
};

/**
 * @brief 定时发送规则
 *
 * 格式：every N send 消息名.
 */
struct TimerRule {
    std::uint32_t interval_seconds{0};
    std::string message_name;
};

/**
 * @brief SML 文档
 *
 * 包含所有消息定义和规则
 */
struct Document {
    std::vector<MessageDef> messages;
    std::vector<ConditionRule> conditions;
    std::vector<TimerRule> timers;

    [[nodiscard]] const MessageDef *
    find_message(std::string_view name) const noexcept {
        for (const auto &msg : messages) {
            if (msg.name == name) {
                return &msg;
            }
        }
        return nullptr;
    }

    [[nodiscard]] const MessageDef *
    find_by_sf(std::uint8_t stream, std::uint8_t function) const noexcept {
        for (const auto &msg : messages) {
            if (msg.stream == stream && msg.function == function &&
                msg.name.empty()) {
                return &msg;
            }
        }
        return nullptr;
    }
};

} // namespace secs::sml
