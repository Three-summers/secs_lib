# HSMS 控制消息互通性验证审查（SEMI E37）

- 日期：2026-01-03
- 执行者：Codex（静态代码审查）
- 代码基线：`9b89aa3d3ea0d437fef946d9eb6e0152d79b89bd`
- 范围（按任务要求）：`src/hsms/session.cpp`、`src/hsms/connection.cpp`、`src/hsms/message.cpp`、`include/secs/hsms/session.hpp`、`tests/test_hsms_transport.cpp`

> 说明：仓库内未包含 SEMI E37 原文，本报告的章节号引用以任务描述中出现的 `E37 §8.3.2` 为锚点；其余控制消息章节号请在你所使用的 E37 版本中核对（通常位于 HSMS-SS 的 §8 “Control Message Procedure/State Model” 相关小节）。

## 1. 当前实现行为概览（只基于代码事实）

### 1.1 状态模型

- 真实状态仅三态：`SessionState::{disconnected, connected, selected}`（`include/secs/hsms/session.hpp:23`）
- `connected` 对应协议语义的 “NOT_SELECTED”（但库内未显式命名）
- 断线统一经 `Session::on_disconnected_()`，会清空 `pending_`/`inbound_data_` 并 cancel 等待者（`src/hsms/session.cpp:84`）

### 1.2 控制事务超时（T6）策略

所有控制事务（SELECT/DESELECT/LINKTEST 等）统一走 `Session::async_control_transaction_()`：

- 发送请求后等待 `pending->ready.async_wait(timeout)`
- 若超时（`core::errc::timeout`）：立即 `connection_.async_close()`，随后 `on_disconnected_(timeout)`，会话进入 `disconnected`（`src/hsms/session.cpp:293`-`315`）

这意味着：控制事务超时不会回到 `connected/NOT_SELECTED`，而是直接断线收敛。

### 1.3 入站控制消息分发（reader_loop_）

`Session::reader_loop_()` 对控制消息的处理（`src/hsms/session.cpp:140` 起）：

| SType | 当前行为（摘要） | 关键代码 |
|---|---|---|
| Select.req | 被动端：校验 `SessionID`；拒绝路径会关闭连接；接受则回 `Select.rsp(OK)` 并进入 `selected` | `src/hsms/session.cpp:143`-`177` |
| Select.rsp | 主动端：先 fulfill pending；若 status==0 则进入 `selected` | `src/hsms/session.cpp:179`-`188` |
| Deselect.req | 回 `Deselect.rsp(OK)` 后关闭连接并 `disconnected` | `src/hsms/session.cpp:189`-`197` |
| Deselect.rsp | fulfill pending 后关闭连接并 `disconnected` | `src/hsms/session.cpp:198`-`205` |
| Linktest.req | 立即回 `Linktest.rsp(OK)` | `src/hsms/session.cpp:206`-`211` |
| Linktest.rsp | fulfill pending | `src/hsms/session.cpp:212`-`215` |
| Separate.req | 关闭连接并 `disconnected` | `src/hsms/session.cpp:216`-`221` |
| 其他控制类型（含 Reject.req） | 直接忽略（不回 Reject） | `src/hsms/session.cpp:222`-`225` |

### 1.4 写入串行化与“无优先级”

`Connection::async_write_message()` 使用 `write_in_progress_ + write_gate_` 做协程锁，保证“整帧写入不交错”，但没有控制消息优先级/抢占机制（`src/hsms/connection.cpp:211`-`241`）。

这会影响 Deselect/Separate 等“希望立即停止发送数据并优先回包”的互通性要求（见风险点 AUDIT-HSMS-003）。

## 2. 与 SEMI E37 的互通性风险点（≥5）

下列风险点均给出：严重等级、代码定位、规范引用（章节）、互通性影响与修复建议（含期望行为）。

### AUDIT-HSMS-001 — Select.req 超时(T6)后进入 disconnected（而非 NOT_SELECTED）

- 严重等级：Critical
- 规范引用：SEMI E37 `§8.3.2`（任务锚点：Select.req 超时应回到 NOT_SELECTED）
- 代码证据：
  - 控制事务超时统一断线：`src/hsms/session.cpp:308`-`315`
  - 断线会把状态置为 `disconnected`：`src/hsms/session.cpp:84`-`101`
  - 主动端 SELECT 流程依赖 `async_control_transaction_`：`src/hsms/session.cpp:402`-`407`
- 互通性影响（典型场景）：
  - 若对端实现遵循 “超时仅退回 NOT_SELECTED，TCP 仍保持” 的语义，本库直接断线会被对端视为网络异常而非选择失败，可能触发对端的异常恢复路径（例如快速重连抑制、报警、或进入错误状态）。
  - 在高负载/短暂阻塞时，对端可能在 T6 边界后才发出 `Select.rsp`；本库已关闭连接，导致 “迟到响应” 被丢弃，形成反复重连抖动。
- 修复建议（不改代码，仅给出可执行方向）：
  - 建议在 `src/hsms/session.cpp:293`-`315` 将“超时必断线”的策略拆分为“按消息类型决定”：
    - 期望行为：`Select.req` 超时后会话状态回到 `connected`（NOT_SELECTED），并保持 TCP 连接处于可继续收发控制消息的状态；允许上层决定是否重试 SELECT、或发送 `Separate.req` 再断开。
    - 实现落点：让 `async_control_transaction_()` 在超时分支只做 `pending_.erase()` 并返回 `timeout`，不调用 `connection_.async_close()`/`on_disconnected_()`；将“是否断线”移动到调用点（例如 `async_open_active`）并按 E37 语义处理。

### AUDIT-HSMS-002 — Deselect.req/Deselect.rsp 被实现为“完成后断线”，无法进入 NOT_SELECTED

- 严重等级：High
- 规范引用：SEMI E37（Deselect Procedure，通常位于 `§8.4`）；以及 HSMS-SS 状态模型（NOT_SELECTED 与 SEPARATE 的职责分离）
- 代码证据：
  - 收到 `Deselect.req`：回 `Deselect.rsp` 后 `async_close()` + `on_disconnected_(cancelled)`：`src/hsms/session.cpp:189`-`197`
  - 收到 `Deselect.rsp`：同样关闭连接并 `disconnected`：`src/hsms/session.cpp:198`-`205`
  - 相关单测明确验证“Deselect 会导致双方 disconnected”：`tests/test_hsms_transport.cpp:795`-`840`
- 互通性影响：
  - E37 将 Deselect 与 Separate 拆分：Deselect 结束选择关系但不必断开 TCP；Separate 才是断线/断开 HSMS 连接。当前实现把 Deselect 等价成断线，会导致：
    - 对端期望 Deselect 后仍保持连接并再次 Select 的情况下，出现“对端仍写、我方已关”造成读写错误/异常恢复。
    - 对端可能在 Deselect 后发送一些控制流（如重新 Select、Linktest 或其它厂商扩展），但本库已断线。
- 修复建议：
  - 建议将 `src/hsms/session.cpp:189`-`205` 的行为改为：
    - 期望行为：完成 Deselect 交互后，状态从 `selected` 回到 `connected`（NOT_SELECTED），保持底层连接 open；不调用 `on_disconnected_()`。
    - 同时：应停止 `linktest_loop_`（通过 generation/状态判断已可退出，但需要先把 state 切到 connected），并取消/失败化所有挂起的数据事务（避免 NOT_SELECTED 时仍有人等待 T3）。
  - 如果产品策略坚持“Deselect 即断线”，建议在对外文档中明确该非典型语义，并评估与目标设备/Host 的互通性（尤其是遵循严格状态机的实现）。

### AUDIT-HSMS-003 — 收到 Deselect.req 后并不能“立即停止发送数据消息”（存在竞态与写入优先级问题）

- 严重等级：Critical
- 规范引用：SEMI E37（Deselect 过程中对数据消息收发的约束，通常位于 `§8.4`）
- 代码证据：
  - 处理 `Deselect.req` 时未先切换状态/阻断 data：`src/hsms/session.cpp:189`-`196`
  - `async_send` 仅在 `state_ != selected` 时拒绝 data：`src/hsms/session.cpp:480`-`488`
  - `Connection::async_write_message` 无控制消息优先级：`src/hsms/connection.cpp:211`-`241`
- 互通性影响（具体竞态路径）：
  1. `reader_loop_` 收到 `Deselect.req` 后，在写出 `Deselect.rsp` 之前，`state_` 仍为 `selected`。
  2. 同时，其他协程可能调用 `Session::async_send(data)` 或 `async_request_data()`：由于 `state_ == selected`，会通过检查并进入写锁等待。
  3. 写锁没有优先级，“先抢到锁的 data” 可能先于 `Deselect.rsp` 写出，形成“Deselect.req 之后仍发送 data”的序列。
  4. 对端实现若在发出 Deselect.req 后立即切断 data 通道，可能将后续 data 判为协议错误（常见行为是直接 SEPARATE/断线，或 Reject 控制消息）。
- 修复建议：
  - 在 `src/hsms/session.cpp:189` 的 `Deselect.req` 分支内，建议先执行“立即阻断 data”再写回包：
    - 期望行为：在处理 `Deselect.req` 的最开始，立刻把状态切换为 `connected`（NOT_SELECTED），使 `async_send(data)` 立即失败，避免任何新 data 排队写出。
  - 对“已在等待写锁的 data 协程”：
    - 期望行为：应能被快速失败/取消，避免它们在 Deselect.rsp 前抢占写锁。
    - 实现落点：需要为 `Connection::async_write_message` 引入“控制消息优先级/抢占”或在 Session 层引入“写门禁（gate）”并在 Deselect 开始时 cancel 所有等待写入的 data。

### AUDIT-HSMS-004 — Reject 控制消息未实现：既不发送 Reject.req，也不解析 reason code

- 严重等级：High
- 规范引用：SEMI E37（Reject Procedure 与 reason code 表，通常位于 `§8.7` 或相邻小节）
- 代码证据：
  - `reader_loop_` 对未实现控制类型“直接忽略”，注释点名“后续可扩展 Reject”：`src/hsms/session.cpp:222`-`225`
  - `SType::reject_req` 被标记为“known”，但没有任何编码助手/消息体格式：`include/secs/hsms/message.hpp:17`-`28`、`src/hsms/message.cpp:33`-`47`
  - 单测明确验证“Reject.req 被忽略且连接仍 selected”：`tests/test_hsms_transport.cpp:983`-`1036`
  - 对未知 `SType`：解码阶段直接返回 `invalid_argument`，无法进入 Session 做 Reject：`src/hsms/message.cpp:156`-`161`
- 互通性影响：
  - 对端发送 `Reject.req`（例如对本端控制消息字段不符合要求）时，本库忽略该信息，上层无法得知对端拒绝原因，容易继续发送同类消息，导致对端升级为 `Separate.req` 或硬断线。
  - 对端发送“厂商扩展控制类型”或偶发畸形控制消息时，本库要么忽略（若 SType 恰好在 known 集合内，如 reject_req），要么在解码层直接判错并断线（若 SType 不在 known 集合内）。两者都偏离“用 Reject 反馈原因”的互通性常规路径。
- 修复建议：
  - 在 `include/secs/hsms/message.hpp`/`src/hsms/message.cpp` 增加 Reject 的显式数据模型与构造/解析：
    - 期望行为：Reject.req 需携带 reason code，并包含被拒绝消息的 10B header（E37 的典型要求）；reason code 应按 E37 定义做枚举映射（避免各厂商数值差异）。
  - 在 `src/hsms/session.cpp:222` 默认分支中：
    - 期望行为：对“未知/不支持/字段非法”的控制消息发送 `Reject.req`（而不是静默忽略），并按 E37 要求决定是否继续保持连接或进入 NOT_SELECTED。
  - 在 `src/hsms/message.cpp:156`-`161`：
    - 期望行为：至少能“解析出 header 并把原始 SType 值保留”，以便上层发送 Reject（否则无法对未知 SType 做规范化拒绝）。

### AUDIT-HSMS-005 — Select.rsp 状态码过于粗粒度（仅 0/1），多种拒绝原因被合并

- 严重等级：Medium
- 规范引用：SEMI E37 `§8.3.2`（Select.rsp status 的语义定义；非 0 表示拒绝且通常可区分原因）
- 代码证据：
  - 仅定义 `kRspOk=0` 与 `kRspReject=1`：`src/hsms/session.cpp:16`-`18`
  - 对“session_id 不匹配”“配置拒绝”“already selected”等不同场景均回同一 status：`src/hsms/session.cpp:148`-`176`
- 互通性影响：
  - 对端若按 status 值采取不同恢复动作（例如 “already selected” 不应断线、但 “session id mismatch” 可能需要配置修正），当前实现无法表达差异，可能导致对端采取错误策略。
  - 本库在部分拒绝路径还会主动断线（`src/hsms/session.cpp:151`-`155`、`167`-`171`），进一步放大歧义。
- 修复建议：
  - 在 `src/hsms/session.cpp:143`-`177` 为不同拒绝原因返回不同的 Select.rsp status（按 E37 定义的状态码集合），并统一明确“拒绝后是否保持连接/退回 NOT_SELECTED”：
    - 期望行为：对“已 selected 重复 Select.req”等非致命场景：回对应 status 并保持连接与状态不变；
    - 对“SessionID 不匹配”等致命配置错误：回对应 status，随后退回 NOT_SELECTED 或按策略 Separate（需与 E37 约束一致）。

### AUDIT-HSMS-006 — Linktest 失败即断线，且“连续失败阈值”不可配置

- 严重等级：High
- 规范引用：SEMI E37（Linktest Procedure，通常位于 `§8.5`）
- 代码证据：
  - 仅提供 `linktest_interval`，无失败阈值配置：`include/secs/hsms/session.hpp:42`-`49`
  - 周期 linktest 任一错误即关闭连接：`src/hsms/session.cpp:262`-`266`
  - `async_linktest` 依赖 `async_control_transaction_`，其超时会触发断线：`src/hsms/session.cpp:531`-`546`、`src/hsms/session.cpp:308`-`315`
  - 单测验证“服务端不回 linktest.rsp 时客户端断线”：`tests/test_hsms_transport.cpp:1254`-`1321`
- 互通性影响：
  - 生产环境常见短暂抖动（线程暂停、网络瞬断、CPU 抢占）可能导致一次 T6 超时；立即断线会造成不必要的重连风暴，影响吞吐与稳定性。
  - 某些厂商实现会允许 N 次连续 Linktest 失败后才 Separate（避免误判）；当前实现无法配置。
- 修复建议：
  - 建议在 `include/secs/hsms/session.hpp:29`-`49` 增加可配置项，例如：
    - `linktest_max_consecutive_failures`（默认 1，但可调大以适配生产）
    - （可选）`linktest_timeout`（与通用 `t6` 解耦，避免影响其它控制事务）
  - 在 `src/hsms/session.cpp:237`-`267` 的 `linktest_loop_` 中维护失败计数：
    - 期望行为：未超过阈值时保持连接并继续下一周期；超过阈值时再执行 `Separate.req` 或关闭连接（按 E37 与产品策略确定）。

### AUDIT-HSMS-007 — Linktest.rsp / Separate.req 的 SessionID 字段语义需要与 E37 严格对齐

- 严重等级：Medium
- 规范引用：SEMI E37（HSMS Header 字段在各控制消息中的语义约束，通常位于 §8 的 “Message Header/Control Message Format” 小节）
- 代码证据：
  - `make_linktest_rsp(status, ...)` 将 `SessionID` 用作 status（与 `Select.rsp` 同套路）：`src/hsms/message.cpp:88`-`90`
  - `make_separate_req(session_id, ...)` 也直接写入 session_id：`src/hsms/message.cpp:92`-`95`
  - `reader_loop_` 对 `Linktest.req` 的响应固定用 `status=0`：`src/hsms/session.cpp:206`-`210`
- 互通性影响：
  - 若对端实现对控制消息 header 字段做严格校验（例如要求 Linktest.rsp 的 SessionID 必须等于协商 session id，或必须为保留值），当前实现可能被对端判为非法控制消息，从而触发对端 Reject/Separate，最终表现为“周期 linktest 总失败”。
- 修复建议：
  - 建议在 `src/hsms/message.cpp:83`-`95` 明确区分“只有 Select.rsp/Deselect.rsp 使用 SessionID=Status”的特殊规则，其余控制消息按 E37 的字段语义填写：
    - 期望行为：Linktest.req/rsp、Separate.req 的 SessionID 填充规则与 E37 一致（要么为当前 session_id，要么为保留值；需在实现中固化并补测试）。
  - 在 `tests/test_hsms_transport.cpp` 增加对 Linktest.req/rsp header 字段的字节级断言，防止字段语义漂移。

## 3. 现有测试覆盖分析（不运行，仅静态阅读）

### 3.1 已覆盖（与本次审查相关）

- Select 基本握手与显式 Linktest：`tests/test_hsms_transport.cpp:638`-`686`
- 重复 Select.req（already selected 分支）：`tests/test_hsms_transport.cpp:688`-`740`
- Select.req SessionID 不匹配：`tests/test_hsms_transport.cpp:742`-`790`
- Deselect.req 导致双方 disconnected（当前实现语义）：`tests/test_hsms_transport.cpp:795`-`840`
- Reject.req 被忽略且仍保持 selected：`tests/test_hsms_transport.cpp:983`-`1036`
- T6：Select 阶段超时返回 timeout：`tests/test_hsms_transport.cpp:1100`-`1129`
- Linktest 周期失败触发断线：`tests/test_hsms_transport.cpp:1254`-`1321`
- Separate.req 相关断线与重连：`tests/test_hsms_transport.cpp:1131` 起（以及 `:1389` 起的重连测试）

### 3.2 关键缺口（与互通性风险直接相关）

1. Select.req 超时后的状态机语义（是否回 NOT_SELECTED 而非断线）未被断言：当前只断言 `async_open_active` 返回 `timeout`（`tests/test_hsms_transport.cpp:1100`-`1129`）。
2. Deselect.req 收到后“立即停止发送 data”与“控制消息写优先级”未覆盖（task-1 审查重点 2）。
3. Reject reason code 的编解码/映射未覆盖（当前也未实现；task-1 审查重点 3）。
4. Linktest 连续超时 N 次才断线（阈值可配置）未覆盖（task-1 审查重点 4）。

## 4. 结论与建议优先级

### 4.1 结论

当前 HSMS 控制流实现的核心策略是“超时/部分控制流直接断线收敛”，这在工程上简化状态一致性，但与 SEMI E37 对 NOT_SELECTED/SELECTED/SEPARATE 的语义分离存在明显偏差风险，且对 Deselect/Reject 的互通性要求覆盖不足。

### 4.2 建议优先级（按互通性破坏程度）

1.（Critical）AUDIT-HSMS-001：Select 超时回 NOT_SELECTED 而非断线。
2.（Critical）AUDIT-HSMS-003：Deselect.req 后立即阻断 data + 控制消息优先级。
3.（High）AUDIT-HSMS-002：Deselect 不应等价断线（至少需可配置/可选策略）。
4.（High）AUDIT-HSMS-004：实现 Reject.req（含 reason code）并在未知/非法控制消息时发送。
5.（High）AUDIT-HSMS-006：Linktest 连续失败阈值可配置，避免一次超时即重连风暴。

