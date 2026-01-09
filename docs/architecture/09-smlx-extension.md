# SML 扩展提案：占位符（变量注入）与脚本化收发（SMLX）

> 文档生成日期：2026-01-09  
> 执行者：Codex  
> 状态：已实现部分（SMLX v0，见“当前实现状态”）  
> 关联：现有 SML 子集与运行时说明见 `docs/architecture/06-sml-module.md`

## 0. 当前实现状态（SMLX v0）

已实现（代码已落地）：

- **消息模板支持占位符**（值位置写标识符）：`<A MDLN>`、`<U2 1 SVIDS 3>`、`<B BYTES>` 等
- **渲染接口**：
  - `include/secs/sml/render.hpp`：`secs::sml::RenderContext` + `secs::sml::render_item()`
  - `include/secs/sml/runtime.hpp`：`secs::sml::Runtime::encode_message_body()`（面向“代码主动发送”）

当前限制（后续可扩展）：

- `if (...) ==<Item>` 的 **期望值不允许占位符**（解析阶段会报 `sml.parser/invalid_condition`）
- `every N send ...` 的 `N` 暂不支持变量
- ASCII 字符串内的 `${VAR}` 插值暂未实现（仅支持 `<A VAR>` 整值替换）

### 0.1 代码主动发送（示例）

当你想在 C++ 业务代码中“按模板主动发送某条消息”时，可按下面流程：

1) 准备 `secs::sml::Runtime` 并加载 SMLX 源文本  
2) 准备 `secs::sml::RenderContext` 注入变量（变量值用 `secs::ii::Item` 表达）  
3) 调用 `Runtime::encode_message_body()` 得到 `stream/function/w_bit + SECS-II body bytes`  
4) 用 `secs::protocol::Session` 的 `async_send/async_request` 发出去

示意代码（省略错误处理与 io/executor 管理细节）：

```cpp
#include "secs/protocol/session.hpp"
#include "secs/sml/render.hpp"
#include "secs/sml/runtime.hpp"

secs::sml::Runtime rt;
rt.load(sml_source);

secs::sml::RenderContext ctx;
ctx.set("MDLN", secs::ii::Item::ascii("WET.01"));

std::vector<secs::core::byte> body;
std::uint8_t stream = 0, function = 0;
bool w = false;

auto ec = rt.encode_message_body("establish", ctx, body, &stream, &function, &w);
if (!ec) {
  const secs::core::bytes_view body_view{body.data(), body.size()};
  if (w) co_await session.async_request(stream, function, body_view);
  else   co_await session.async_send(stream, function, body_view);
}
```

## 1. 这件事“值不值得做”

结论：**值得做**，且非常贴合 `secs::sml` 当前“模板 + 规则”的定位。

`secs::sml` 已具备：

- **消息模板**：`name: SxFy [W] <Item>.`
- **自动应答规则**：`if (条件) 响应消息名.`
- **定时发送规则**：`every N send 消息名.`

但目前模板是“纯常量”，在以下场景会很快变得难维护：

- 同一条消息在多处复用，只是 **少数几个字段不同**（例如设备名、版本号、SVID 列表、CEID、参数值）。
- 自动应答需要把 **配置/上下文**（例如 MDLN、SOFTREV、设备/产线信息）注入到响应体中。
- 定时发送需要带上 **运行时参数**（例如 heartbeat 的计数器、周期参数、动态列表）。

因此，引入“占位符/变量注入”可以把“业务变化”从 C++ handler 或重复 SML 片段中抽离出来，让 SML 更像“可复用的脚本与模板”。

## 2. 设计目标与边界

### 2.1 目标

- **模板可参数化**：在 `<Item>` 内允许引用变量，运行时替换成真实值。
- **保持现有语法心智模型**：`if (...) ...` 与 `every N send ...` 原样保留，只是在它们引用的消息模板里允许动态值。
- **类型对齐 SECS-II**：变量替换必须能映射到具体的 SECS-II Item 类型与取值范围（例如 `U2` 必须在 `[0,65535]`）。
- **对“脚本”保持克制**：优先做“模板渲染”，避免把 SMLX 变成复杂的通用编程语言。

### 2.2 非目标（本提案不覆盖）

- 不追求完整的表达式语言（算术、函数、循环等）。
- 不替代现有 C++ Router/TypedHandler（复杂业务仍建议用代码处理）。
- 不定义与传输层（HSMS/SECS-I）耦合的行为语义（SMLX 只描述“要发什么/怎么回”）。

## 3. SMLX 总体思路（在现有 SML 上做“增量扩展”）

### 3.1 保持“规则引擎”结构不变

- `if` 规则：仍然是“收到某类消息 -> 选择某个响应模板”。
- `every` 规则：仍然是“按固定周期 -> 发送某个模板”。

变化仅在：**模板渲染时允许用变量填充**。

### 3.2 引入“渲染上下文（Render Context）”

渲染上下文是一份键值对（变量名 -> 值），由宿主程序在运行时提供/更新。SMLX 不强制规定宿主 API 形态，但建议满足：

- 能对同一个 `Runtime` 实例反复更新上下文（例如设备名、版本号在启动时注入；运行计数器可能周期变化）。
- 支持按发送/应答单次覆盖（例如某次发送临时把 `SVIDS` 设置成不同列表）。

## 4. 语法扩展（推荐：对 Lexer/Parser 侵入最小的写法）

本节只描述“新增/扩展点”，现有基础语法以 `docs/architecture/06-sml-module.md` 为准。

### 4.1 值占位符：在“原本写字面量”的位置写变量名

把原本写数值/字符串的地方，允许写一个 **标识符（Identifier）** 作为占位符。

#### 4.1.1 数值类 Item（`B/Boolean/U*/I*/F*`）

- 原语法：`<U2 1 2 3>`
- SMLX：允许混写字面量与变量：

```sml
status_query: S1F3 W
<L
  <U2 SVIDS>           // SVIDS：运行时注入，可是单值或数组
  <U1 1 ACK_CODE 3>    // ACK_CODE：运行时注入，单值
>.
```

建议的变量值类型约定：

- 单值占位符：展开为 1 个元素
- 数组占位符：展开为 N 个元素（拼接到该类型的 values 列表中）

#### 4.1.2 ASCII Item（`A`）

支持两种写法：

1) **整值替换**（占位符直接作为 A 的值）：

```sml
establish: S1F13 W <L <A MDLN> <A SOFTREV>>.
```

2) **字符串插值**（在双引号字符串内使用 `${VAR}` 片段）：

```sml
establish: S1F13 W
<L
  <A "MDLN=${MDLN}">
  <A "REV=${SOFTREV}">
>.
```

说明：

- 字符串插值只影响 `A "..."` 的内容，不影响词法层；实现时可在渲染阶段扫描 `${...}` 并替换。
- `${...}` 中仅允许变量名（不引入表达式），不支持嵌套。

### 4.2 条件规则与定时规则的扩展点（可选，但推荐支持）

#### 4.2.1 `if` 规则中的期望值支持占位符

使“匹配条件”可以由配置/上下文驱动：

```sml
ack_ok: S2F22 <L <U1 0>>.
ack_ng: S2F22 <L <U1 1>>.

// 当第 2 个元素等于 <A EXPECT_CMD> 时回 OK
if (S2F21(2)==<A EXPECT_CMD>) ack_ok.
```

#### 4.2.2 `every` 的间隔支持变量（仍以秒为单位）

```sml
every HEARTBEAT_INTERVAL send heartbeat.
```

约束：`HEARTBEAT_INTERVAL` 必须能解析为正整数秒。

## 5. 语义约定（实现时需要明确的行为）

### 5.1 渲染时机

- **发送**：在执行 `send`（主动发送/定时发送）之前渲染模板。
- **应答**：在命中 `if` 规则并确定响应模板后渲染模板。

### 5.2 类型与范围校验

渲染时必须做类型/范围校验（对应 SECS-II）：

- `U1/U2/U4/U8`：变量必须可转换为无符号整数且不溢出
- `I1/I2/I4/I8`：变量必须可转换为有符号整数且不溢出
- `F4/F8`：变量必须可转换为浮点；可接受整数提升
- `B`：变量可为整数（0~255）或字节数组
- `Boolean`：变量可为 `0/1`（以及兼容 `true/false` 的宿主表示）
- `A`：变量可为字符串；若宿主提供非字符串，建议按“可打印表示”转换（由实现决定，需在文档/日志中明确）

### 5.3 缺失变量的处理策略

建议的默认策略：**缺失变量 = 渲染失败**，并向上返回错误（或在自动应答/定时发送场景下记录错误并跳过本次发送）。

原因：SECS 报文是强类型载体，隐式降级为默认值往往导致“静默错误”，难以排查。

## 6. 示例：用同一份 SMLX 同时完成“自动应答 + 定时发送”

```sml
/* SMLX：占位符示例（自动应答 + 每 30 秒发送一次） */

are_you_there: S1F1 W.

establish_rsp: S1F2
<L
  <A MDLN>
  <A SOFTREV>
>.

heartbeat: S1F13 W
<L
  <A MDLN>
  <A SOFTREV>
>.

if (S1F1) establish_rsp.
every 30 send heartbeat.
```

宿主在运行时注入（示意）：

- `MDLN = "WET.01"`
- `SOFTREV = "REV.01"`

## 7. 与当前实现的兼容性与落地顺序（建议）

### 7.1 兼容性原则

- 现有 SML 文件应在 SMLX 实现后 **保持可解析且语义不变**。
- SMLX 是对“值位置”的增量放宽，不应改变现有 token/语句的解释。

### 7.2 推荐落地顺序（按收益/风险排序）

1) **模板渲染（仅消息体 Item 支持变量）**：先支持 `<A VAR>` 与数值位 `VAR`，不动 `if/every` 语法。
2) **条件期望值支持变量**：把占位符扩展到 `if (...) == <Item>` 的 `<Item>` 内。
3) **定时间隔支持变量**：把 `every N` 的 `N` 放宽为 `Integer | Identifier`。
4)（可选）**字符串插值**：支持 `A "xx ${VAR} yy"` 的渲染替换。
