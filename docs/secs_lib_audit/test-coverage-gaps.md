# 测试覆盖补充建议清单（Task-4）

- 日期：2026-01-03
- 执行者：Codex（静态阅读现有代码与测试；不修改代码、不执行构建/测试）
- 输出路径：`.claude/specs/secs_lib_audit/test-coverage-gaps.md`

## 1. 背景与输入

本清单用于把审查（task-1/2/3）中识别的**12 个缺失测试场景**落地为“可直接实现”的测试用例设计文档，覆盖 HSMS / SECS-I / SECS-II / Protocol 四层。

参考输入：

- 开发计划：`.claude/specs/secs_lib_audit/dev-plan.md`（Task-4 章节）
- 审查报告：
  - `.claude/specs/secs_lib_audit/hsms-interop-analysis.md`
  - `.claude/specs/secs_lib_audit/secsii-resource-safety.md`
  - `.claude/specs/secs_lib_audit/secs1-compat-fixes.md`
- 现有测试（静态阅读）：
  - `tests/test_hsms_transport.cpp`
  - `tests/test_secs2_codec.cpp`
  - `tests/test_secs1_framing.cpp`
  - `tests/test_protocol_session.cpp`
  - 测试框架入口：`tests/test_main.hpp`（自研断言宏）

> 说明：部分“缺失场景”在当前仓库的 `tests/*.cpp` 中已出现相近用例（例如 SECS-II 深度边界、空 List 往返、SECS-I Reassembler 的 block_number 不连续等）。本清单仍按 Task-4 的 12 条逐项输出设计文档，并在每项中给出“现状覆盖点/补强建议”，避免重复造轮子或遗漏更关键的断言。

## 2. 汇总表（12 项）

| 场景 ID | 层 | 场景摘要 | 优先级 | 建议落点 |
|---|---|---|---|---|
| GAP-HSMS-001 | HSMS | Select.req 超时后迟到 Select.rsp（竞态） | High | 扩展 `tests/test_hsms_transport.cpp` |
| GAP-HSMS-002 | HSMS | Deselect.req 后收到 data（应丢弃） | High | 扩展 `tests/test_hsms_transport.cpp` |
| GAP-HSMS-003 | HSMS | LinkTest 连续超时 3 次才断连（阈值可配） | High | 扩展 `tests/test_hsms_transport.cpp` |
| GAP-HSMS-004 | HSMS | Separate.req 后资源清理完整性 | High | 扩展 `tests/test_hsms_transport.cpp` |
| GAP-SECSII-001 | SECS-II | 嵌套深度=限制值（边界） | Medium | 扩展 `tests/test_secs2_codec.cpp`（已部分覆盖） |
| GAP-SECSII-002 | SECS-II | Binary length 与 payload 不匹配（截断） | High | 扩展 `tests/test_secs2_codec.cpp`（已部分覆盖） |
| GAP-SECSII-003 | SECS-II | Boolean 非 0/1 值处理（未定义行为） | Medium | 扩展 `tests/test_secs2_codec.cpp` |
| GAP-SECSII-004 | SECS-II | 空 List `<L[0]>` 往返一致性 | Low | 扩展 `tests/test_secs2_codec.cpp`（已部分覆盖） |
| GAP-SECSI-001 | SECS-I | 多 Block block_number 非连续（错误检测） | High | 扩展 `tests/test_secs1_framing.cpp`（Reassembler 已覆盖，StateMachine 建议补齐） |
| GAP-SECSI-002 | SECS-I | reverse_bit 与 device_id 最高位冲突 | High | 扩展 `tests/test_secs1_framing.cpp` |
| GAP-PROTO-001 | Protocol | system_bytes 回绕/耗尽下的冲突检测 | Medium | 扩展 `tests/test_protocol_session.cpp`（需测试缝隙建议） |
| GAP-PROTO-002 | Protocol | 会话关闭时未应答请求的清理 | High | 扩展 `tests/test_protocol_session.cpp` |

> 覆盖率提升预期：本文档中的 “+X%” 为**估算**（用于排序与工作量评估），应在真正补齐用例后通过 `gcovr --branches` 以基线数据校验与回填。

---

## 3. HSMS 缺失场景（4）

### GAP-HSMS-001 — Select.req 超时后立即收到 Select.rsp（竞态处理）

- 触发条件：
  - 主动端 `async_open_active()` 发起 `Select.req`，等待 `T6`。
  - 在 `T6` 触发超时后（或极贴近超时边界），对端发送迟到的 `Select.rsp(OK)`。
- 输入构造方法（建议基于 `tests/test_hsms_transport.cpp` 的 MemoryDuplex/Connection 模式）：
  - 客户端：`secs::hsms::Session client`，设置较短 `opt.t6`（例如 5–20ms），禁用 `auto_reconnect` 以避免重连干扰断言。
  - 服务端：不用 `Session`，直接用 `secs::hsms::Connection server_conn` 手工协议：
    1) 读到 `Select.req`（`async_read_message`）
    2) **延迟**到 `T6 + δ`（`asio::steady_timer`）再写 `Select.rsp(OK)`（system_bytes 回显请求的 system_bytes）
  - 客户端协程：执行 `ec = co_await client.async_open_active(std::move(client_conn))`，预期 `ec == timeout`。
  - 继续运行 `io_context` 一小段时间，让“迟到 Select.rsp”有机会被 reader_loop_ 处理（若实现存在竞态漏洞，此时可能误触发 `set_selected_()`）。
  - 为降低 flaky，建议使用“可控 Stream/可控 close 时序”的测试替身（见补强建议）。
- 期望行为：
  - `T6` 超时后会话应进入“断线/未选择”的稳定状态；迟到的 `Select.rsp` 必须被忽略，不得令会话进入 `selected`，也不得触发 `selected_generation` 递增或启动 `linktest_loop_`。
- 断言要点：
  - `client.async_open_active(...)` 返回 `errc::timeout`。
  - 等待短暂时间后：`client.state() == disconnected` 且 `client.is_selected() == false`。
  - **关键断言（用于捕获竞态副作用）**：`client.selected_generation()` 不应因为迟到 `Select.rsp` 被递增（建议断言仍为 0）。
  - （可选）启动 `async_wait_selected(min_generation=1, timeout=...)` 应超时或被取消，但不能成功。
- 优先级：High
- 覆盖率提升预期：
  - 主要覆盖 `src/hsms/session.cpp` 中 `async_control_transaction_()` 的超时分支 + `reader_loop_` 对 `select_rsp` 的“非 pending 命中”路径；预计 **HSMS 分支覆盖 +0.5%~1.0%**（粗估）。
- 测试文件放置建议：
  - **扩展现有文件**：`tests/test_hsms_transport.cpp`（可复用 MemoryDuplex/Connection 与现有超时测试 `test_session_t6_timeout_on_select()`）。
  - 补强建议（避免 sleep/时间敏感）：新增一个“可控的 Stream Stub”（例如让 `async_close` 延后生效或让 read/write 可脚本化）以稳定复现竞态。

---

### GAP-HSMS-002 — Deselect.req 发送后收到数据消息（应丢弃）

- 触发条件：
  - 会话处于 `selected`。
  - 一端发送 `Deselect.req`（或收到 `Deselect.req`）后，在断线完成前收到对端 `data message`。
- 输入构造方法：
  - 基于 `tests/test_hsms_transport.cpp` 的手工服务端模式：
    1) server_conn 读 `Select.req`，回 `Select.rsp(OK)`，完成选择。
    2) client 发送 `Deselect.req`：`client.async_send(make_deselect_req(...))`。
    3) server_conn 收到 `Deselect.req` 后，**先**发送一条 `data message`（任意 stream/function，W=0），**再**发送 `Deselect.rsp(OK)` 并关闭连接。
  - 客户端并发开启一个接收协程：`auto [ec, msg] = co_await client.async_receive_data(…)`，用于观察是否“错误交付”了 data。
- 期望行为：
  - 一旦进入 Deselect 流程，HSMS 层不应再向上层交付任何 `data message`（应丢弃/忽略），以符合“去选择后停止数据收发”的互通性预期。
  - 断线发生后，`inbound_data_` 应清空。
- 断言要点：
  - 关键断言：`async_receive_data` 不应返回 `ok` 且携带上述注入的 data；允许返回 `cancelled/broken_pipe/timeout`，但**不能交付消息体**。
  - 断线后再次调用 `async_receive_data(short_timeout)` 应返回 `timeout`（队列已清空，不存在“残留 data”）。
  - 断线后 `client.state()==disconnected`。
- 优先级：High
- 覆盖率提升预期：
  - 覆盖 `src/hsms/session.cpp` 中 data 入队路径在“控制流收敛期”的行为与断线清空路径；预计 **HSMS 分支覆盖 +0.3%~0.8%**（粗估）。
- 测试文件放置建议：
  - **扩展现有文件**：`tests/test_hsms_transport.cpp`（现有 `test_session_deselect_req_disconnects_both_sides()` 仅验证断线，不验证 data 丢弃）。

---

### GAP-HSMS-003 — LinkTest 连续超时 3 次触发断连（阈值配置）

- 触发条件：
  - `linktest_interval > 0`，会话处于 `selected`，自动周期发送 `Linktest.req`。
  - 对端连续 N 次不响应，达到阈值（本场景 N=3）后触发断连；阈值应可配置。
- 输入构造方法：
  - 前置需求（当前代码缺口）：`secs::hsms::SessionOptions` 需新增类似 `linktest_max_consecutive_failures` 的配置项（审查建议见 `.claude/specs/secs_lib_audit/hsms-interop-analysis.md`）。
  - 构造：
    1) client：`linktest_interval=5ms`，`t6` 设置为很短（例如 5–10ms），阈值设为 3。
    2) server_conn：完成 select 之后，读取并丢弃 `Linktest.req`，始终不回 `Linktest.rsp`。
    3) 通过 server 侧计数“已收到第 1/2/3 次 Linktest.req”来同步断言（避免纯 sleep）。
- 期望行为：
  - 连续失败次数 <3：会话保持连接（仍为 `selected`，或至少不 `disconnected`）。
  - 第 3 次失败后：会话断连（可通过 `Separate.req` 或 close 收敛，按实现策略）。
  - （可选补强）若中途收到一次成功 `Linktest.rsp`，失败计数应清零。
- 断言要点：
  - 在 server 观测到第 1/2 次 `Linktest.req` 后：`client.state()==selected`（或 `client.is_selected()==true`）。
  - 在第 3 次失败后：`client.state()==disconnected`。
  - （可选）`client.async_wait_reader_stopped(…)` 最终应返回 `ok`（确保 reader_loop 可回收）。
- 优先级：High
- 覆盖率提升预期：
  - 覆盖 linktest_loop 的“失败计数/阈值分支”与断线收敛路径；预计 **HSMS 分支覆盖 +0.5%~1.5%**（粗估，取决于实现复杂度）。
- 测试文件放置建议：
  - **扩展现有文件**：`tests/test_hsms_transport.cpp`（现有 `test_session_linktest_interval_disconnect_on_failure()` 仅验证“最终会断开”，不覆盖“阈值=3/可配置/中途成功重置”等关键互通语义）。

---

### GAP-HSMS-004 — Separate.req 收到后的资源清理完整性

- 触发条件：
  - 会话处于 `selected`。
  - 存在：1) 未完成的数据事务（pending request）或 2) 未消费的 inbound data。
  - 对端发送 `Separate.req`，触发断线。
- 输入构造方法：
  - client `Session` + 手工 `server_conn`：
    1) 先完成 select（server_conn 读 `Select.req` 回 `Select.rsp(OK)`）。
    2) server_conn 发送一条 data message 给 client，但 client **不调用** `async_receive_data` 消费它（制造“入站队列残留”）。
    3) client 发起 `async_request_data(W=1)`（制造 pending 事务），server 不回复 secondary。
    4) server_conn 发送 `Separate.req` 并关闭。
  - 之后 client 侧验证 pending 与 inbound 是否被清理，并验证可重连复用（可参考现有 `test_session_reopen_after_separate()`）。
- 期望行为：
  - `Separate.req` 导致会话断线收敛：取消 `pending_`，清空 `inbound_data_`，取消等待事件，`reader_loop_` 退出。
  - pending 的请求应**尽快**返回错误（cancelled/broken_pipe 等），不应一直挂到 `T3 timeout` 才返回。
- 断言要点：
  - pending 的 `async_request_data` 返回的 `ec`：
    - 必须为错误且 **不等于 timeout**（避免“泄漏/悬挂到超时”）。
  - 断线后 `async_receive_data(short_timeout)` 返回 `timeout`（队列已清空）。
  - `client.async_wait_reader_stopped(…)` 最终返回 `ok`。
  - （可选）重连后 `selected_generation` 增加（复用测试 `test_session_reopen_after_separate()` 的断言方式）。
- 优先级：High
- 覆盖率提升预期：
  - 覆盖 `SType::separate_req` 分支 + `on_disconnected_` 的 pending/inbound 清理路径；预计 **HSMS 分支覆盖 +0.5%~1.2%**（粗估）。
- 测试文件放置建议：
  - **扩展现有文件**：`tests/test_hsms_transport.cpp`（现有 `test_session_pending_cancelled_on_disconnect()` 更接近“服务端发 Separate 取消挂起请求”，但未覆盖“入站队列残留清理 + reader_stopped 等待 + 复用”三件套组合断言）。

---

## 4. SECS-II 缺失场景（4）

### GAP-SECSII-001 — 嵌套深度 = 当前限制值（边界值测试）

- 触发条件：
  - 解码输入为深层嵌套 List，嵌套深度 == 当前限制值（现实现为 `kMaxDecodeDepth=64`，见 `src/ii/codec.cpp`）。
- 输入构造方法：
  - 构造最小字节流（每层 List 只含 1 个子项）：
    - 重复 `depth` 次：`0x01 0x01`（List, lenBytes=1, length=1）
    - 末尾：`0x21 0x00`（Binary, lenBytes=1, length=0）
  - 分别测试：
    - `depth=64`：应成功
    - `depth=65`：应拒绝（invalid_header）
- 期望行为：
  - 深度 == 64 可解码成功；深度 >64 明确返回 `errc::invalid_header`。
- 断言要点：
  - `depth=64`：`decode_one(...)==ok`，`consumed==in.size()`，并逐层验证 List[0] 链到底为 `Binary{}`。
  - `depth=65`：返回 `invalid_header` 且 `consumed==0`。
- 优先级：Medium
- 覆盖率提升预期：
  - 覆盖 `if (depth > kMaxDecodeDepth)` 的 off-by-one 边界；预计 **`src/ii/codec.cpp` 分支覆盖 +0.2%~0.5%**（粗估）。
- 测试文件放置建议：
  - **扩展现有文件**：`tests/test_secs2_codec.cpp`。
  - 现状对齐：当前已存在 `test_decode_depth_limit_boundary()`（depth=64）与 `test_decode_deep_list_nesting_rejected()`（depth=256），建议补强 `depth=65` 的“紧邻边界”拒绝用例以防回归。

---

### GAP-SECSII-002 — Binary length 声明与实际 payload 不匹配（截断检测）

- 触发条件：
  - Binary item header 声明 length=N，但实际输入 payload 长度 < N。
- 输入构造方法：
  - 最小截断样例（便于定位/减少噪音）：
    - `0x21 0x02 0xAA`
      - `0x21`：Binary（format_code=0x08），lenBytes=1
      - `0x02`：声明 length=2
      - 仅提供 1 字节 payload `0xAA`
- 期望行为：
  - 解码返回 `errc::truncated`，且不得发生过量分配（尤其在声明超大 length 的情况下）。
- 断言要点：
  - `decode_one(...)` 返回 `truncated` 且 `consumed==0`。
  - （可选补强）保留一个“超大 length（例如 0xFFFFFF）但短 payload”的回归用例，确保不会提前分配超大缓冲区。
- 优先级：High
- 覆盖率提升预期：
  - 覆盖 `SpanReader::read_payload` 的截断分支与 Binary 分支；预计 **`src/ii/codec.cpp` 分支覆盖 +0.2%~0.6%**（粗估）。
- 测试文件放置建议：
  - **扩展现有文件**：`tests/test_secs2_codec.cpp`。
  - 现状对齐：当前已有 `test_decode_large_binary_length_truncated()`（大 length），建议再补一个“小 length 截断”以避免只覆盖“超大声明”路径。

---

### GAP-SECSII-003 — Boolean 类型的非 0/1 值处理（协议未定义行为）

- 触发条件：
  - Boolean item payload 出现非 `0x00/0x01` 的字节（例如 `0x02/0xFF`）。
- 输入构造方法：
  - 直接构造 raw bytes（绕过编码器的“只输出 0/1”特性）：
    - `0x25 0x04 0x00 0x02 0x01 0xFF`
      - `0x25 = (0x09<<2)|0x01`：Boolean，lenBytes=1
      - length=4
      - payload：`00 02 01 FF`
- 期望行为（基于当前实现事实）：
  - 解码成功，按“非 0 即 true”归一化：`00->false`，其余均为 `true`（见 `src/ii/codec.cpp`：`v.push_back(b != 0)`）。
  - 重新编码时，应输出规范化字节（`false->0x00`，`true->0x01`）。
- 断言要点：
  - 解码后 `Item == Item::boolean({false, true, true, true})`。
  - （可选补强）对 decoded item 再次 `encode`，断言输出 payload 只包含 `0x00/0x01`（验证“规范化编码”）。
- 优先级：Medium
- 覆盖率提升预期：
  - 覆盖 boolean 解码分支 + 规范化编码分支；预计 **`src/ii/codec.cpp` 分支覆盖 +0.2%~0.6%**（粗估）。
- 测试文件放置建议：
  - **扩展现有文件**：`tests/test_secs2_codec.cpp`（新增 `test_decode_boolean_non_01_values()` 一类的测试函数）。

---

### GAP-SECSII-004 — 空 List `<L[0]>` 的编解码往返一致性

- 触发条件：
  - List length=0（空列表）。
- 输入构造方法：
  - 编码路径：`Item::list({})` -> `encode(...)`
  - 解码路径：对编码结果调用 `decode_one(...)`
- 期望行为：
  - 往返一致（decode 后等于原 Item），且编码是**规范形式**：`[0x01, 0x00]`（List, lenBytes=1, length=0）。
- 断言要点：
  - `encode(Item::list({}))` 输出 size=2，且 bytes 精确等于 `0x01 0x00`。
  - decode 后 `Item == Item::list({})` 且 `consumed==2`。
- 优先级：Low
- 覆盖率提升预期：
  - 覆盖 List length=0 的编码/解码路径；预计 **`src/ii/codec.cpp` 分支覆盖 +0.1%~0.3%**（粗估）。
- 测试文件放置建议：
  - **扩展现有文件**：`tests/test_secs2_codec.cpp`。
  - 现状对齐：现有 `test_roundtrip_all_types()` 已包含 `expect_roundtrip(Item::list({}))`，建议补充“字节级规范形式”断言，避免未来编码器选择不同 length_bytes 或输出非规范形式。

---

## 5. SECS-I 缺失场景（2）

### GAP-SECSI-001 — 多 Block 消息的 block_number 非连续（错误检测）

- 触发条件：
  - 对端发送多 block 消息，block_number 序列出现跳号（例如 `1 -> 3`，缺失 `2`）。
- 输入构造方法（建议覆盖到 StateMachine 层，而不仅仅是 Reassembler）：
  - 使用 `MemoryLink::create()` 建立链路对 `a/b`，在 `b` 侧启动 `secs::secs1::StateMachine receiver`。
  - 在 `a` 侧“手工扮演对端”：
    1) 发送 `ENQ`，等待 `receiver` 回 `EOT` 完成握手
    2) 发送 block#1（`end_bit=false, block_number=1`）
    3) 等待对端 ACK（若协议实现如此）
    4) 发送 block#3（`end_bit=true, block_number=3`）而非 block#2
  - 两个 block 帧可用 `secs::secs1::encode_block(header, body, frame)` 生成（只需修改 header 的 `block_number/end_bit` 与 body 分段）。
- 期望行为：
  - `StateMachine::async_receive()` 返回明确错误（优先期望 `secs::secs1::errc::block_sequence_error`），并清理当前重组状态，回到可接收下一条消息的状态。
- 断言要点：
  - `auto [ec, msg] = co_await receiver.async_receive(...)`：
    - `ec == secs::secs1::errc::block_sequence_error`（或若实现折叠为 `protocol_error`，需在断言中对齐库的契约）
    - `msg` 应为空或被忽略（不交付半包数据）
  - （补强）错误后再发送一条合法单 block 消息，receiver 能成功接收（验证“错误恢复/状态清理”）。
- 优先级：High
- 覆盖率提升预期：
  - 覆盖 `StateMachine` 接收侧的“重组序列错误”分支与恢复路径；预计 **SECS-I 分支覆盖 +0.5%~1.2%**（粗估，取决于 state_machine 的分支复杂度）。
- 测试文件放置建议：
  - **扩展现有文件**：`tests/test_secs1_framing.cpp`（已包含大量 StateMachine receive 错误分支测试）。
  - 现状对齐：当前 `Reassembler` 层已存在 `block_number 不连续` 的单测（见 `tests/test_secs1_framing.cpp` 内 `test_reassembler_mismatch_after_first_block()` 的相关分支），建议把错误覆盖推进到 `StateMachine::async_receive()` 的对外行为与恢复能力。

---

### GAP-SECSI-002 — reverse_bit 与 device_id 最高位冲突场景

- 触发条件：
  - `device_id` 超出 15 位合法域（`device_id >= 0x8000`），其最高位与 header Byte1 的 reverse_bit 位发生编码语义冲突。
- 输入构造方法：
  - 构造 `secs::secs1::Header h`：
    - `h.device_id = 0x8000`（冲突边界值）
    - 分别取 `h.reverse_bit=false/true`（两种组合都应拒绝）
  - 调用：`secs::secs1::encode_block(h, payload, frame)`。
- 期望行为：
  - `encode_block` 返回 `errc::invalid_argument`，拒绝产生“歧义 header”。
- 断言要点：
  - 返回值 == `secs::core::errc::invalid_argument`。
  - （可选）确保 frame 未被写入或保持空（对齐 encode_block 的输出契约）。
- 优先级：High
- 覆盖率提升预期：
  - 覆盖 device_id 合法域校验的边界分支；预计 **SECS-I 分支覆盖 +0.2%~0.5%**（粗估）。
- 测试文件放置建议：
  - **扩展现有文件**：`tests/test_secs1_framing.cpp`（紧邻 `test_encode_invalid_device_id()`，用 `0x8000` 补齐“最高位冲突”边界，而不是只覆盖 `0xFFFF` 这种远端非法值）。

---

## 6. Protocol 层缺失场景（2）

### GAP-PROTO-001 — system_bytes 回环重用的冲突检测（32 位空间耗尽）

- 触发条件：
  - system_bytes 分配器发生回绕（wrap-around）或可用值被全部占用（极端：`2^32-1` 都在用），若处理不当可能导致“在用值被重复分配”，进而造成请求-响应错配（冲突）。
- 输入构造方法（分两级：SystemBytes 单测 + Session 集成测试）：
  1) **SystemBytes 单测（建议优先落地）**
     - 目标：覆盖 `SystemBytes::allocate()` 在“空间耗尽”时返回 `buffer_overflow` 的分支，并验证不会重复分配。
     - 难点：真实 32 位空间无法在单测中穷举填满。
     - 建议的测试缝隙（实现后即可单测覆盖）：
       - 给 `SystemBytes` 增加一个**仅测试可用**的“上限参数/策略注入”（例如 `max_usable` 或模板化范围），将可用空间缩小到 3–16 个值。
       - 在小空间中构造：全部在用 -> allocate 返回 `buffer_overflow`；释放一个 -> allocate 可重用释放值；回绕时遇到仍在用的值 -> 必须跳过或最终 overflow。
  2) **protocol::Session 集成测试（次级，依赖上面的注入能力）**
     - 目标：当 system_bytes 分配失败时：
       - `async_send/async_request` 应直接返回 `buffer_overflow`
       - 不得插入 pending（避免泄漏/脏状态）
       - 不得复用仍在用的 system_bytes（避免冲突）
     - 同样建议通过注入“小空间 SystemBytes”使并发请求快速触发耗尽。
- 期望行为：
  - 在空间耗尽时，分配器明确返回 `buffer_overflow`，上层请求不会因“重复分配”而产生错配。
  - 释放后可重用（但仅能重用已释放的值）。
- 断言要点：
  - SystemBytes：
    - 连续分配直到耗尽：最后一次返回 `buffer_overflow`
    - `in_use_count()` 与 `is_in_use()` 随释放/重分配保持一致
    - 不会分配 `0`
  - protocol::Session（若实现注入）：
    - 第 N+1 个并发请求返回 `buffer_overflow`
    - 已在 pending 的请求不会被错误唤醒/错配
- 优先级：Medium
- 覆盖率提升预期：
  - 若引入可测试的小空间策略，可覆盖 `src/protocol/system_bytes.cpp` 中目前标注为 “极端分支难覆盖” 的 overflow 路径；预计 **Protocol 分支覆盖 +0.3%~0.8%**（粗估）。
- 测试文件放置建议：
  - **扩展现有文件**：`tests/test_protocol_session.cpp`（已有 `test_system_bytes_unique_release_reuse_and_wrap()`，可在其后新增 `test_system_bytes_exhaustion_returns_overflow()`）。
  - 若需要注入能力：建议新增一个小型专用测试文件 `tests/test_protocol_system_bytes.cpp` 以隔离“测试专用构造器/策略”带来的样板代码（可选）。

---

### GAP-PROTO-002 — 会话关闭时未应答请求的清理（内存泄漏风险）

- 触发条件：
  - protocol::Session 存在挂起的 `async_request`（已发送 primary，等待 secondary）。
  - 底层会话断线（HSMS 的 `async_receive_data` 返回错误；或 SECS-I 的 `async_receive` 返回错误），导致接收循环退出。
- 输入构造方法（HSMS 后端，建议复用 `tests/test_protocol_session.cpp` 的集成测试结构）：
  - 建立 HSMS server/client（MemoryDuplex + `hsms::Session::async_open_*`），创建 `protocol::Session proto_client(client, ...)`。
  - 发起一个会挂起的请求：
    - 服务端不启动 `protocol::Session::async_run()` 或不注册处理器，保证不会发送 secondary。
  - 在请求挂起期间触发断线：
    - 由 server 主动 `stop()` 或发送 `Separate.req` 并关闭连接（确保 client 侧 `async_receive_data` 返回非 timeout 的错误）。
  - 断言该请求应被**立即取消**而不是一直等到 T3 超时。
- 期望行为（对应当前实现的设计意图）：
  - `protocol::Session::async_run()` 在接收错误时调用 `cancel_all_pending_(ec)`，取消所有挂起请求并释放 system_bytes，避免协程悬挂与内存泄漏。
- 断言要点：
  - `auto [ec, rsp] = co_await proto_client.async_request(...)`：
    - `ec` 应为 `cancelled/broken_pipe/…`（以实际传递的错误为准），但关键是 **不等于 timeout**（避免挂到 T3）。
  - 触发断线后，`proto_client.stop()` 可安全调用且幂等（不崩溃/不二次唤醒）。
  - （可选）若随后重新建立新会话/新 proto_client，新请求仍可成功（验证无残留状态污染）。
- 优先级：High
- 覆盖率提升预期：
  - 覆盖 `src/protocol/session.cpp` 中 `async_run()` 的“接收错误 -> cancel_all_pending”路径以及 pending->ready.cancel 分支；预计 **Protocol 分支覆盖 +0.5%~1.2%**（粗估）。
- 测试文件放置建议：
  - **扩展现有文件**：`tests/test_protocol_session.cpp`。
  - 现状对齐：当前已有 `test_hsms_protocol_stop_cancels_pending()` 覆盖“显式 stop 取消 pending”；本场景补齐“底层断线导致的自动清理”，二者覆盖的分支不同。

