# SECS 库综合审查详细报告（Task-5）

- 日期：2026-01-03
- 执行者：Codex（静态汇总审查）
- 代码基线：`9b89aa3d3ea0d437fef946d9eb6e0152d79b89bd`
- 输入来源：
  - task-1：`.claude/specs/secs_lib_audit/hsms-interop-analysis.md`
  - task-2：`.claude/specs/secs_lib_audit/secsii-resource-safety.md`
  - task-3：`.claude/specs/secs_lib_audit/secs1-compat-fixes.md`
  - task-4：`.claude/specs/secs_lib_audit/test-coverage-gaps.md`

## 1. 风险矩阵（严重等级 × 修复难度）

> 说明：
> - 严重等级（Critical/High/Medium/Low）来自 task-1/2/3 的审查结论，以及 task-4 的“测试缺口优先级”（对 `GAP-*` 条目而言，严重等级表达的是**回归风险**，不等价于“已确认缺陷”）。
> - 修复难度（Low/Medium/High）依据：涉及模块规模（代码行数）、跨文件/跨层级改动范围、并发/状态机/协议字节级约束带来的复杂度。

### 修复难度口径

- **Low**：单文件或少量改动；不改变对外语义或仅补测试/文档；预计 ≤1 人日。
- **Medium**：需要跨 2–4 个文件/模块协同；涉及配置项/接口契约调整或新增多用例测试；预计 2–5 人日。
- **High**：涉及状态机语义、并发竞态、协议字段语义或预算模型等“系统性约束”；需要跨层联调与较多回归测试；预计 ≥1 人周。

### 矩阵表

| 严重等级 \\ 难度 | Low | Medium | High |
|---|---|---|---|
| **Critical** |  | `AUDIT-SECSII-001`<br>`AUDIT-SECSII-002`<br>`AUDIT-SECSII-003` | `AUDIT-HSMS-001`<br>`AUDIT-HSMS-003` |
| **High** | `GAP-SECSI-002`<br>`GAP-SECSII-002` | `AUDIT-HSMS-002`<br>`AUDIT-HSMS-004`<br>`AUDIT-HSMS-006`<br>`GAP-HSMS-001`<br>`GAP-HSMS-002`<br>`GAP-HSMS-003`<br>`GAP-HSMS-004`<br>`GAP-SECSI-001`<br>`GAP-PROTO-002` | `AUDIT-SECSII-004`<br>`AUDIT-SECSI-001` |
| **Medium** | `AUDIT-SECSII-005`<br>`AUDIT-SECSI-002`<br>`GAP-SECSII-001`<br>`GAP-SECSII-003` | `AUDIT-HSMS-007`<br>`GAP-PROTO-001` |  |
| **Low** | `AUDIT-HSMS-005`<br>`AUDIT-SECSI-003`<br>`GAP-SECSII-004` |  |  |

## 2. 问题清单（≥15）

> 统一字段：
> - **描述**：问题是什么（尽量用协议术语/实现行为描述）
> - **影响**：对互通性、稳定性、资源、可维护性造成的结果
> - **根因**：为什么会发生（实现策略/接口模型/测试缺口）
> - **建议**：可执行的修复方向（不写具体代码实现）
>
> 追溯说明：
> - `AUDIT-HSMS-*` 直接来自 task-1 的编号。
> - task-2（SECS-II）与 task-3（SECS-I）原文未为每条问题分配统一 ID；本综合报告为便于风险矩阵/路线图追踪，补充了 `AUDIT-SECSII-*` 与 `AUDIT-SECSI-*`：
>   - `AUDIT-SECSII-001/002/003/004` 对应 task-2 的攻击向量 1/2/3/4；`AUDIT-SECSII-005` 对应“深度上限默认值/可配置性”结论。
>   - `AUDIT-SECSI-001` 对应 task-3 的 “block_number 高位差异”；`AUDIT-SECSI-002` 对应 device_id 合法域差异；`AUDIT-SECSI-003` 对应 checksum 口径歧义的说明。

### HSMS（SEMI E37）互通性问题（AUDIT-HSMS-***）

#### AUDIT-HSMS-001（Critical × High）

- 描述：`Select.req` 控制事务超时（T6）后会话进入 `disconnected`，而非回到 NOT_SELECTED（库内对应 `connected` 状态）。
- 影响：对端实现若遵循“超时退回 NOT_SELECTED 但保持 TCP”语义，本库会被视为网络异常；迟到 `Select.rsp` 被丢弃，导致重连抖动与互通性故障。
- 根因：`async_control_transaction_()` 将“超时”与“断线收敛”绑定，所有控制事务超时统一触发 `connection_.async_close()` + `on_disconnected_()`。
- 建议：将“是否断线”从通用控制事务抽离到调用点；`Select.req` 超时应回到 NOT_SELECTED 并保持连接可继续控制交互（参考 task-1 中 `src/hsms/session.cpp:293-315` 的建议落点）。

#### AUDIT-HSMS-002（High × Medium）

- 描述：`Deselect.req/Deselect.rsp` 被实现为“完成交互即断线”，无法进入 NOT_SELECTED。
- 影响：对端期望 Deselect 后复用同一连接再次 Select 时会失败；对端若在 Deselect 后发送控制流/扩展消息，本端已断线导致读写错误与异常恢复。
- 根因：`reader_loop_` 的 Deselect 分支在发送 rsp 后直接关闭连接并调用 `on_disconnected_()`；现有单测也固化了该语义。
- 建议：将 Deselect 处理改为“selected -> connected(NOT_SELECTED)，保持连接 open”；同时停止 `linktest_loop_` 并处理挂起数据事务的取消/失败化，避免 NOT_SELECTED 时仍等待 T3。

#### AUDIT-HSMS-003（Critical × High）

- 描述：收到 `Deselect.req` 后无法“立即停止发送 data”，控制消息与 data 写入无优先级，存在竞态使 data 可能先于 `Deselect.rsp` 发出。
- 影响：对端常见实现会在 Deselect.req 后立即切断 data 通道；若仍收到 data，可能直接判为协议错误并 Separate/断线。
- 根因：状态切换发生在写回包之后；`Connection::async_write_message` 仅做串行化，不具备“控制消息优先级/抢占”；已经排队等待写锁的 data 协程可能先获得写锁。
- 建议：在 Deselect.req 处理起始处立即切换到 NOT_SELECTED，阻断新的 data；并引入“控制消息优先级/写门禁 + 取消等待写入的 data”，确保 Deselect.rsp 与后续控制流优先于 data。

#### AUDIT-HSMS-004（High × Medium）

- 描述：Reject 控制消息未实现：既不发送 `Reject.req`，也不解析 reason code；对未知/不支持控制类型常走“忽略/解码失败”路径。
- 影响：对端发送 Reject 时，本端无法获知拒绝原因而持续发送同类消息，容易被对端升级为 Separate/断线；对未知控制类型无法按 E37 常规路径进行“显式拒绝”。
- 根因：`SType::reject_req` 虽被标记为 known，但缺少编码/解码数据模型；解码层对未知 `SType` 直接判 `invalid_argument`，导致无法进入 Session 层做 Reject。
- 建议：补齐 Reject.req 的数据模型（reason code + 被拒绝消息 header）；对未知/非法/不支持控制消息应发送 Reject.req；必要时保留原始 `SType` 以支持拒绝未知类型。

#### AUDIT-HSMS-005（Medium × Low）

- 描述：`Select.rsp` status 仅有 `0/1` 两类，多种拒绝原因被合并（例如 already selected、SessionID mismatch、配置拒绝等）。
- 影响：对端若依据 status 做不同恢复策略（重试、保持连接、报警、重新配置），当前实现无法表达差异；部分拒绝路径还会主动断线，进一步放大歧义。
- 根因：实现仅定义 `kRspOk/kRspReject` 两个码，并在多个分支复用同一 reject 值。
- 建议：按 E37 的 status 语义细化返回码，并统一“拒绝后是否保持连接/退回 NOT_SELECTED”的策略；对非致命拒绝（如重复 Select）避免断线。

#### AUDIT-HSMS-006（High × Medium）

- 描述：周期 Linktest 任一失败即断线，且“连续失败阈值”不可配置。
- 影响：生产环境中短暂抖动可能导致一次 T6 超时；立即断线会造成不必要的重连风暴，影响吞吐与稳定性；与允许 N 次失败再 Separate 的实现互通性较差。
- 根因：仅提供 `linktest_interval` 配置；`linktest_loop_` 任一错误直接 close；且 `async_linktest` 复用“超时即断线”的通用控制事务策略。
- 建议：增加 `linktest_max_consecutive_failures`（及可选独立超时）配置；在 linktest_loop 维护失败计数，超过阈值再 Separate/断线。

#### AUDIT-HSMS-007（Medium × Medium）

- 描述：`Linktest.rsp / Separate.req` 的 `SessionID` 字段语义可能与 E37 不一致（实现存在“沿用 Select.rsp 的 SessionID=Status”模式）。
- 影响：对端若对控制消息 header 字段做严格校验，可能判为非法控制消息并触发 Reject/Separate，表现为周期 Linktest 总失败。
- 根因：消息构造函数对不同控制类型的 header 字段复用同一写入策略，缺少“每种控制消息字段语义”层面的显式约束与测试。
- 建议：明确区分“只有 Select.rsp/Deselect.rsp 使用 SessionID=Status”的规则，其余控制消息按 E37 的字段语义填写；补充字节级断言测试防漂移。

### SECS-II 资源安全与协议健壮性问题（AUDIT-SECSII-***）

#### AUDIT-SECSII-001（Critical × Medium）

- 描述：List 的 `length` 表示“子元素个数”，但允许到 `0xFFFFFF`，并在验证子项存在前执行 `items.reserve(length)`；仅 4 字节头即可触发超大预分配。
- 影响：可能触发数百 MB~1GB 的分配；在 `noexcept` 解码路径中触发 `std::bad_alloc` 时可能 `std::terminate`（DoS）；即使不崩溃也会造成内存尖峰。
- 根因：对 List 的 length 沿用了 `kMaxLength=0xFFFFFF`（3 字节 length 字段最大值）的上限口径，未区分“字节数 vs 子项数”；且 reserve 发生在有效性检查之前。
- 建议：引入 `kMaxDecodeListItems` 硬上限，并在 `reserve` 前校验；对宽 List 增加最小输入长度预检查，避免对截断输入做超大 reserve。

#### AUDIT-SECSII-002（Critical × Medium）

- 描述：宽 List + 最小子元素可制造 O(n) CPU 消耗与巨大常驻内存（即便输入大小约 32MB，解析循环可达 1677 万次）。
- 影响：CPU 被长时间占用、内存维持在数百 MB~1GB；在多连接/并发解码时更易造成服务不可用（DoS）。
- 根因：缺少 `kMaxDecodeListItems/kMaxDecodeTotalItems` 等节点预算，解析循环按声明的 length 逐项递归。
- 建议：在 List 分支进入循环前限制 `length`；并维护 `kMaxDecodeTotalItems`（全树节点预算）以抑制“多个宽 List 叠加”。

#### AUDIT-SECSII-003（Critical × Medium）

- 描述：ASCII/Binary 允许 `length=0xFFFFFF`（16MB）并进行整块分配+拷贝；成功路径必然产生大对象。
- 影响：单项 16MB 的内存与 O(length) 拷贝确定发生；在内存紧张时可能触发异常并在 `noexcept` 下终止进程；并发时峰值不可控。
- 根因：payload length 仅按 length 字段上限校验，缺乏对“单项 payload 上限”的业务/互通约束；`Item` 采用拥有存储的容器模型，天然需要分配与拷贝。
- 建议：增加 `kMaxDecodePayloadBytes`（建议远小于 16MB，如 1MB/4MB）；在读取 payload 前拒绝超限；若业务必须支持大 payload，应单独评审“零拷贝/流式”接口（属于接口层变更）。

#### AUDIT-SECSII-004（High × High）

- 描述：缺乏“总预算”（Total Bytes/Items）限制，组合消息（例如 List 内含多个 16MB Binary）轻易超过 64MB。
- 影响：在允许并发解码或长连接持续接收时，资源上限不可控；即使单项限制存在，组合仍可突破整体内存预算。
- 根因：解码过程没有跨节点的预算上下文；每个节点独立分配，无法在树级别累计并提前拒绝。
- 建议：在 `decode_item()` 递归过程中引入预算上下文，累计并扣减 `kMaxDecodeTotalBytes` 与 `kMaxDecodeTotalItems`；在进入子项循环或创建 string/vector 前做预算检查。

#### AUDIT-SECSII-005（Medium × Low）

- 描述：decode 深度上限目前为常量 `kMaxDecodeDepth=64`；若外部互通基线（如某些实现的 `max_nested_level=16`）存在约束差异，需要可配置。
- 影响：与对端的“最大嵌套层级”约束不同步时，可能造成互通差异；同时默认值偏大也扩大了资源暴露面（虽然栈风险本身可控）。
- 根因：深度上限为编译期常量，缺少对外配置与文档契约。
- 建议：提供可配置的 decode depth 上限，并根据互通需求选择默认值（例如对齐 16 或保持 64 但允许下调）。

### SECS-I 与 c_dump 兼容性问题（AUDIT-SECSI-***）

#### AUDIT-SECSI-001（High × High）

- 描述：block_number 高位（BlockNumber[14:8]）在 c_dump 中未编码；secs_lib 按 15 位语义写入 Header Byte5[6:0]，当 `block_number>=256` 时产生字节级差异。
- 影响：在多 Block/长消息场景下，若对端按 c_dump 的 8 位语义解析，可能出现重组异常、ACK/重传逻辑分歧、难以定位的互通故障。
- 根因：两实现对 block_number 的语义假设不同：c_dump 仅使用 8 位，secs_lib 使用 15 位并映射到 Byte5/Byte6。
- 建议：明确兼容策略（二选一）：
  - 对齐 c_dump：将 block_number 收敛为 8 位（保留 Byte5[6:0]=0），同步更新 API 契约与测试；
  - 或保持 15 位：但在互通路径强制 `block_number<=0x00FF` 并明确约束，避免“高位被对端丢弃”。

#### AUDIT-SECSI-002（Medium × Low）

- 描述：`device_id>=0x8000` 会与 header Byte1 的 reverse_bit 发生语义冲突；secs_lib 显式拒绝该输入，但 c_dump 未显式校验，可能编码出歧义字节流。
- 影响：跨实现互通/迁移时，输入域约束不一致会导致“某端可发送、另一端拒绝”；若忽略约束，可能产生无法判定 reverse_bit 的歧义消息。
- 根因：两实现对 device_id 合法域约束策略不同；API 契约层未统一固化“合法域/拒绝策略”。
- 建议：以 secs_lib 的“拒绝歧义输入”作为契约，并在文档与测试中固化边界值（特别是 0x7FFF/0x8000）。

#### AUDIT-SECSI-003（Low × Low）

- 描述：checksum 计算范围在文档口径上存在歧义（是否包含 length 字节）；现有代码证据显示两实现均为“对 payload（Header+Data）求和，不包含 length 与 checksum 字段”。
- 影响：主要影响维护者理解与未来修改风险；若误解可能引入兼容性回归。
- 根因：任务描述/dev-plan 的文字口径与代码事实不一致。
- 建议：在开发计划/对外文档/API 注释中统一表述 checksum 范围，并在字节级测试中固定该行为。

### 测试覆盖缺口（GAP-*，来自 task-4）

> 注：以下条目为“缺失测试场景”，其严重等级与 task-4 的优先级对齐，用于表达回归风险与排期优先级。

#### GAP-HSMS-001（High × Medium）

- 描述：缺失 `Select.req` 超时后迟到 `Select.rsp` 的竞态用例。
- 影响：若存在竞态副作用（错误进入 selected、generation 漂移、linktest_loop 误启动），会在生产中表现为偶发互通性故障且难复现。
- 根因：缺少“超时边界 + 延迟响应”时序测试。
- 建议：扩展 `tests/test_hsms_transport.cpp`，构造服务端延迟 `T6+δ` 发送 `Select.rsp`，断言 client 不进入 `selected` 且 generation 不递增。

#### GAP-HSMS-002（High × Medium）

- 描述：缺失 `Deselect.req` 发送/接收后仍收到 data 的场景用例（应丢弃/拒绝）。
- 影响：无法在测试期捕获“Deselect 后仍发送 data”导致的互通性断线问题（与 `AUDIT-HSMS-003` 强相关）。
- 根因：缺少“控制流切换期间 data 排队/写锁竞态”的测试覆盖。
- 建议：扩展 `tests/test_hsms_transport.cpp`，在 Deselect 流程中插入并发 data 发送，断言 data 不得出现在 Deselect 之后的线序中。

#### GAP-HSMS-003（High × Medium）

- 描述：缺失 Linktest 连续失败 N 次才断连（阈值可配）的用例。
- 影响：Linktest 策略调整后容易引入回归（例如误断线或永不断线），且与设备/Host 的互通性高度相关。
- 根因：测试未覆盖“失败计数/阈值配置”的分支。
- 建议：在实现 `linktest_max_consecutive_failures` 后补齐用例：连续失败 N-1 次仍保持，达到 N 次触发 Separate/断线。

#### GAP-HSMS-004（High × Medium）

- 描述：缺失 `Separate.req` 收到后的资源清理完整性用例。
- 影响：若 pending/inbound_data/等待者未被正确取消，可能形成内存泄漏或协程悬挂，表现为长期运行不稳定。
- 根因：缺少“断线清理路径”的系统级断言。
- 建议：扩展 `tests/test_hsms_transport.cpp`，触发 Separate.req 并断言 pending 全部被取消、状态一致且可重连。

#### GAP-SECSII-001（Medium × Low）

- 描述：缺失“嵌套深度=限制值”的边界用例（部分已有覆盖但断言可加强）。
- 影响：深度上限调整时容易出现 off-by-one 回归。
- 根因：边界断言不足（仅覆盖极深拒绝或部分边界）。
- 建议：扩展 `tests/test_secs2_codec.cpp`，补齐深度=64（或未来配置值）可通过、深度=limit+1 必拒绝的双侧断言。

#### GAP-SECSII-002（High × Low）

- 描述：缺失 Binary length 与实际 payload 不匹配（截断）的覆盖强化（部分已有覆盖但建议补齐“小 length 截断”路径）。
- 影响：截断检测回归可能导致读越界或错误解析（在不可信输入下风险更高）。
- 根因：测试偏向“超大声明截断”，对“小声明/小截断”分支覆盖不足。
- 建议：扩展 `tests/test_secs2_codec.cpp`，增加“length=3 但仅给 2 字节”的用例，断言 `errc::truncated` 且不得过量分配。

#### GAP-SECSII-003（Medium × Low）

- 描述：缺失 Boolean 非 0/1 值处理（归一化行为）的用例。
- 影响：协议未定义输入在不同实现间可能行为分歧；缺少测试会导致未来修改时行为漂移。
- 根因：编码器只输出 0/1，未覆盖“原始 bytes 输入”的解码分支。
- 建议：新增 raw bytes 用例，断言解码为 `b!=0` 的归一化语义，重编码只输出 0/1。

#### GAP-SECSII-004（Low × Low）

- 描述：缺失空 List `<L[0]>` 往返的一致性与“规范形式字节流”断言（部分已有 roundtrip 覆盖）。
- 影响：低风险，但可防止编码器输出非规范 length_bytes 形式导致互通差异。
- 根因：现有测试未对字节流规范形式做精确断言。
- 建议：扩展 roundtrip 用例，断言编码输出为 `0x01 0x00`。

#### GAP-SECSI-001（High × Medium）

- 描述：缺失多 Block 消息 block_number 非连续在 StateMachine 层的错误检测用例（Reassembler 层已有相近覆盖）。
- 影响：错误检测/恢复路径回归会导致重组状态污染、后续消息不可用或误交付半包数据。
- 根因：测试覆盖未从 Reassembler 推进到对外行为（StateMachine::async_receive）。
- 建议：扩展 `tests/test_secs1_framing.cpp`，构造 block#1 后直接 block#3，断言返回明确错误并具备错误恢复能力（随后合法消息可继续接收）。

#### GAP-SECSI-002（High × Low）

- 描述：缺失 reverse_bit 与 device_id 最高位冲突（`device_id=0x8000`）的边界拒绝用例。
- 影响：输入域校验回归会重新引入歧义字节流，影响互通性与可调试性。
- 根因：现有用例可能只覆盖远端非法值（如 0xFFFF），未覆盖“最高位冲突”这一关键边界。
- 建议：扩展 `tests/test_secs1_framing.cpp`，对 `device_id=0x8000` 断言 `invalid_argument`。

#### GAP-PROTO-001（Medium × Medium）

- 描述：缺失 system_bytes 回绕/耗尽下的冲突检测用例（32 位空间耗尽极端分支）。
- 影响：若 system_bytes 分配重复，可能造成请求-响应错配（严重协议错误）；缺少测试将使该类问题长期潜伏。
- 根因：真实 32 位空间无法穷举，缺少可测试的“小空间注入/策略”缝隙。
- 建议：引入仅测试可用的“可用空间上限/策略注入”，在小空间内覆盖 overflow/回绕/跳过在用值的分支；再在 `protocol::Session` 层做集成断言。

#### GAP-PROTO-002（High × Medium）

- 描述：缺失会话断线时“未应答请求”的自动清理用例（避免挂到 T3 超时才结束）。
- 影响：若 pending 未被及时取消，可能导致协程悬挂、system_bytes 泄漏、stop 不幂等，长期运行稳定性下降。
- 根因：现有测试偏向“显式 stop 取消 pending”，未覆盖“底层断线 -> cancel_all_pending”的路径。
- 建议：扩展 `tests/test_protocol_session.cpp`：发起挂起请求后触发底层断线，断言请求立即以 cancelled/broken_pipe 等错误返回且资源被释放。

## 3. c_dump vs secs_lib 设计差异对比（5 项）

| 维度 | c_dump（参考实现） | secs_lib（当前实现） | 与本次问题的关联 |
|---|---|---|---|
| 传输层 | 无 HSMS（仅 SECS-I） | HSMS over TCP（Asio 异步 I/O） | 引入控制消息状态机与并发写入问题（`AUDIT-HSMS-*`） |
| 内存模型 | 以定长 buffer/手工管理为主 | RAII + `std::vector/std::string` 等拥有型容器 | 宽 List/大 payload 时更易触发资源耗尽，需要显式预算（`AUDIT-SECSII-*`） |
| 错误处理与异常语义 | 错误码为主，分配失败路径依赖调用方策略 | `noexcept` 解码路径 + 可能触发 `std::terminate` | 资源耗尽在 `noexcept` 中更可能变为进程终止（`AUDIT-SECSII-001/003`） |
| SECS-I 组帧语义 | block_number 事实语义偏 8 位（高位不编码） | block_number 语义为 15 位并映射到 Byte5/Byte6 | 当 `block_number>=256` 时出现字节级差异（`AUDIT-SECSI-001`） |
| 测试基线 | 生产环境长期验证 | 单元/集成测试驱动，但存在覆盖缺口 | 竞态/清理/边界用例不足会放大回归风险（`GAP-*`） |
