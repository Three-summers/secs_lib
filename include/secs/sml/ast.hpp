#pragma once

#include "secs/ii/item.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace secs::sml {

/**
 * @brief 占位符引用（用于 SMLX 模板渲染）
 *
 * 说明：
 * - 该类型仅描述“变量名”，具体值由宿主在渲染阶段提供；
 * - 目前占位符主要用于消息模板（MessageDef.item）内的值填充。
 */
struct VarRef final {
    std::string name;
    friend bool operator==(const VarRef &, const VarRef &) = default;
};

template <class T>
using ValueExpr = std::variant<T, VarRef>;

struct TplASCII final {
    ValueExpr<std::string> value;
    friend bool operator==(const TplASCII &, const TplASCII &) = default;
};

struct TplBinary final {
    std::vector<ValueExpr<secs::ii::byte>> values;
    friend bool operator==(const TplBinary &, const TplBinary &) = default;
};

struct TplBoolean final {
    std::vector<ValueExpr<bool>> values;
    friend bool operator==(const TplBoolean &, const TplBoolean &) = default;
};

struct TplI1 final {
    std::vector<ValueExpr<std::int8_t>> values;
    friend bool operator==(const TplI1 &, const TplI1 &) = default;
};
struct TplI2 final {
    std::vector<ValueExpr<std::int16_t>> values;
    friend bool operator==(const TplI2 &, const TplI2 &) = default;
};
struct TplI4 final {
    std::vector<ValueExpr<std::int32_t>> values;
    friend bool operator==(const TplI4 &, const TplI4 &) = default;
};
struct TplI8 final {
    std::vector<ValueExpr<std::int64_t>> values;
    friend bool operator==(const TplI8 &, const TplI8 &) = default;
};

struct TplU1 final {
    std::vector<ValueExpr<std::uint8_t>> values;
    friend bool operator==(const TplU1 &, const TplU1 &) = default;
};
struct TplU2 final {
    std::vector<ValueExpr<std::uint16_t>> values;
    friend bool operator==(const TplU2 &, const TplU2 &) = default;
};
struct TplU4 final {
    std::vector<ValueExpr<std::uint32_t>> values;
    friend bool operator==(const TplU4 &, const TplU4 &) = default;
};
struct TplU8 final {
    std::vector<ValueExpr<std::uint64_t>> values;
    friend bool operator==(const TplU8 &, const TplU8 &) = default;
};

struct TplF4 final {
    std::vector<ValueExpr<float>> values;
    friend bool operator==(const TplF4 &, const TplF4 &) = default;
};
struct TplF8 final {
    std::vector<ValueExpr<double>> values;
    friend bool operator==(const TplF8 &, const TplF8 &) = default;
};

class TemplateItem;
using TplList = std::vector<TemplateItem>;

/**
 * @brief SMLX Item 模板（允许在值位置引用变量）
 *
 * 说明：
 * - 该类型仅用于 SML 模块内部表示“可渲染的模板”；
 * - 真正上行/下行仍使用 `secs::ii::Item`（编码/解码见 ii::codec）；
 * - List 语义仍是“Item 序列”，当前不支持在 List 内直接插入占位符子树。
 */
class TemplateItem final {
public:
    using storage_type = std::variant<TplList,
                                      TplASCII,
                                      TplBinary,
                                      TplBoolean,
                                      TplI1,
                                      TplI2,
                                      TplI4,
                                      TplI8,
                                      TplU1,
                                      TplU2,
                                      TplU4,
                                      TplU8,
                                      TplF4,
                                      TplF8>;

    TemplateItem() = delete;

    explicit TemplateItem(TplList v) : storage_(std::move(v)) {}
    explicit TemplateItem(TplASCII v) : storage_(std::move(v)) {}
    explicit TemplateItem(TplBinary v) : storage_(std::move(v)) {}
    explicit TemplateItem(TplBoolean v) : storage_(std::move(v)) {}

    explicit TemplateItem(TplI1 v) : storage_(std::move(v)) {}
    explicit TemplateItem(TplI2 v) : storage_(std::move(v)) {}
    explicit TemplateItem(TplI4 v) : storage_(std::move(v)) {}
    explicit TemplateItem(TplI8 v) : storage_(std::move(v)) {}

    explicit TemplateItem(TplU1 v) : storage_(std::move(v)) {}
    explicit TemplateItem(TplU2 v) : storage_(std::move(v)) {}
    explicit TemplateItem(TplU4 v) : storage_(std::move(v)) {}
    explicit TemplateItem(TplU8 v) : storage_(std::move(v)) {}

    explicit TemplateItem(TplF4 v) : storage_(std::move(v)) {}
    explicit TemplateItem(TplF8 v) : storage_(std::move(v)) {}

    [[nodiscard]] const storage_type &storage() const noexcept {
        return storage_;
    }
    [[nodiscard]] storage_type &storage() noexcept { return storage_; }

    template <class T>
    [[nodiscard]] const T *get_if() const noexcept {
        return std::get_if<T>(&storage_);
    }

    template <class T>
    [[nodiscard]] T *get_if() noexcept {
        return std::get_if<T>(&storage_);
    }

private:
    storage_type storage_;
};

/**
 * @brief 捕获变量（用于 Data Capture：匹配时提取数据）
 *
 * 约定：
 * - 语法上写作 `$NAME`；
 * - AST 中的 `name` 不包含前导 '$'；
 * - 该类型仅用于条件 pattern（不影响消息模板渲染的 VarRef）。
 */
struct CaptureVar final {
    std::string name;
    friend bool operator==(const CaptureVar &, const CaptureVar &) = default;
};

struct PatASCII final {
    std::optional<CaptureVar> capture;
    std::string value; // 字面量；capture 有值时该字段忽略
    friend bool operator==(const PatASCII &, const PatASCII &) = default;
};

struct PatBinary final {
    std::optional<CaptureVar> capture;
    std::vector<secs::ii::byte> values; // capture 有值时该字段忽略
    friend bool operator==(const PatBinary &, const PatBinary &) = default;
};

struct PatBoolean final {
    std::optional<CaptureVar> capture;
    std::vector<bool> values; // capture 有值时该字段忽略
    friend bool operator==(const PatBoolean &, const PatBoolean &) = default;
};

struct PatI1 final {
    std::optional<CaptureVar> capture;
    std::vector<std::int8_t> values;
    friend bool operator==(const PatI1 &, const PatI1 &) = default;
};
struct PatI2 final {
    std::optional<CaptureVar> capture;
    std::vector<std::int16_t> values;
    friend bool operator==(const PatI2 &, const PatI2 &) = default;
};
struct PatI4 final {
    std::optional<CaptureVar> capture;
    std::vector<std::int32_t> values;
    friend bool operator==(const PatI4 &, const PatI4 &) = default;
};
struct PatI8 final {
    std::optional<CaptureVar> capture;
    std::vector<std::int64_t> values;
    friend bool operator==(const PatI8 &, const PatI8 &) = default;
};

struct PatU1 final {
    std::optional<CaptureVar> capture;
    std::vector<std::uint8_t> values;
    friend bool operator==(const PatU1 &, const PatU1 &) = default;
};
struct PatU2 final {
    std::optional<CaptureVar> capture;
    std::vector<std::uint16_t> values;
    friend bool operator==(const PatU2 &, const PatU2 &) = default;
};
struct PatU4 final {
    std::optional<CaptureVar> capture;
    std::vector<std::uint32_t> values;
    friend bool operator==(const PatU4 &, const PatU4 &) = default;
};
struct PatU8 final {
    std::optional<CaptureVar> capture;
    std::vector<std::uint64_t> values;
    friend bool operator==(const PatU8 &, const PatU8 &) = default;
};

struct PatF4 final {
    std::optional<CaptureVar> capture;
    std::vector<float> values;
    friend bool operator==(const PatF4 &, const PatF4 &) = default;
};
struct PatF8 final {
    std::optional<CaptureVar> capture;
    std::vector<double> values;
    friend bool operator==(const PatF8 &, const PatF8 &) = default;
};

class PatternItem;
using PatChildren = std::vector<PatternItem>;

struct PatL final {
    std::optional<std::size_t> size_hint;
    std::optional<CaptureVar> capture;
    PatChildren items; // capture 有值时仍可为空；匹配时仍可用 size_hint 做校验
    friend bool operator==(const PatL &, const PatL &) = default;
};

/**
 * @brief 条件匹配用的 Item Pattern（支持捕获变量）
 *
 * 说明：
 * - 该类型用于 `if (...)` 条件中的 `<pattern>`（不带 `==`）；
 * - 捕获变量用于“匹配时提取数据”，其值以 `secs::ii::Item` 形式输出给运行时。
 */
class PatternItem final {
public:
    using storage_type = std::variant<PatL,
                                      PatASCII,
                                      PatBinary,
                                      PatBoolean,
                                      PatI1,
                                      PatI2,
                                      PatI4,
                                      PatI8,
                                      PatU1,
                                      PatU2,
                                      PatU4,
                                      PatU8,
                                      PatF4,
                                      PatF8>;

    PatternItem() = delete;

    explicit PatternItem(PatL v) : storage_(std::move(v)) {}
    explicit PatternItem(PatASCII v) : storage_(std::move(v)) {}
    explicit PatternItem(PatBinary v) : storage_(std::move(v)) {}
    explicit PatternItem(PatBoolean v) : storage_(std::move(v)) {}

    explicit PatternItem(PatI1 v) : storage_(std::move(v)) {}
    explicit PatternItem(PatI2 v) : storage_(std::move(v)) {}
    explicit PatternItem(PatI4 v) : storage_(std::move(v)) {}
    explicit PatternItem(PatI8 v) : storage_(std::move(v)) {}

    explicit PatternItem(PatU1 v) : storage_(std::move(v)) {}
    explicit PatternItem(PatU2 v) : storage_(std::move(v)) {}
    explicit PatternItem(PatU4 v) : storage_(std::move(v)) {}
    explicit PatternItem(PatU8 v) : storage_(std::move(v)) {}

    explicit PatternItem(PatF4 v) : storage_(std::move(v)) {}
    explicit PatternItem(PatF8 v) : storage_(std::move(v)) {}

    [[nodiscard]] const storage_type &storage() const noexcept {
        return storage_;
    }
    [[nodiscard]] storage_type &storage() noexcept { return storage_; }

    template <class T>
    [[nodiscard]] const T *get_if() const noexcept {
        return std::get_if<T>(&storage_);
    }

    template <class T>
    [[nodiscard]] T *get_if() noexcept {
        return std::get_if<T>(&storage_);
    }

    friend bool operator==(const PatternItem &, const PatternItem &) = default;

private:
    storage_type storage_;
};

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
    TemplateItem item;        // 消息体模板（可包含占位符）

    MessageDef() : item(TplList{}) {}
};

/**
 * @brief 条件表达式
 *
 * 格式：消息名[(index)|[index]][==<Item>]
 */
struct Condition {
    std::string message_name;         // 触发消息名或 SxFy
    // 可选索引（从 1 开始）：
    // - 采用 SECS-II Item 的先序遍历编号（包含根节点）。
    // - 若消息体为 <L ...>，则根 List 的编号为 1，第一个子元素编号为 2。
    std::optional<std::size_t> index;
    // 可选索引（从 0 开始）：对 List 做“数组下标”访问（仅选择第 N 个子元素）。
    std::optional<std::size_t> list_index;
    // 深层路径索引（从 0 开始）：支持链式 `[i][j][k]`。
    // - 为空表示未使用该语法；
    // - 当 size()==1 时，语义等价于 list_index（解析器会保持两者一致以兼容旧用法）。
    std::vector<std::size_t> list_path;
    std::optional<TemplateItem> expected; // 可选的期望值（允许占位符）
    std::optional<PatternItem> pattern;   // 可选的结构匹配/捕获 pattern（不带 ==）
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
