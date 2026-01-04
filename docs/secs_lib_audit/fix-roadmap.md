# SECS 库修复路线图（Task-5）

- 日期：2026-01-03
- 执行者：Codex（基于 task-1~4 的静态审查汇总）
- 代码基线：`9b89aa3d3ea0d437fef946d9eb6e0152d79b89bd`

## 0. 工作量估算方法（基于代码行数 + 复杂度分析）

### 0.1 规模基线（关键文件行数，`wc -l`）

| 模块 | 关键文件（示例） | 行数（LOC） | 复杂度特征（用于估算） |
|---|---|---:|---|
| HSMS | `src/hsms/session.cpp` / `src/hsms/connection.cpp` / `src/hsms/message.cpp` / `tests/test_hsms_transport.cpp` | 576 / 278 / 200 / 1610 | 状态机 + 并发写入时序，互通性对字节序列敏感 |
| SECS-II | `src/ii/codec.cpp` / `include/secs/ii/item.hpp` / `tests/test_secs2_codec.cpp` | 942 / 162 / 547 | 递归解析 + 资源预算模型，需避免 `noexcept` 下崩溃 |
| SECS-I | `src/secs1/block.cpp` / `tests/test_secs1_framing.cpp` / `c_dump/Secs_App/secs_I.c` | 286 / 2082 / 365 | 字节级兼容性 + 互通策略决策（语义差异可能是 Breaking Change） |
| Protocol | `src/protocol/session.cpp` / `src/protocol/system_bytes.cpp` / `tests/test_protocol_session.cpp` | 411 / 95 / 1030 | 请求-响应匹配与资源清理，极端分支测试需要“测试缝隙” |

### 0.2 复杂度系数（用于从 LOC 推导人日）

| 复杂度等级 | 系数 | 判定依据（示例） |
|---|---:|---|
| Low | 1.0 | 单文件小改、补断言、改文档/注释，风险可控 |
| Medium | 1.5 | 跨 2–4 文件，涉及配置项/错误码/边界行为调整 |
| High | 2.0 | 状态机语义、并发时序、协议字段语义或预算模型等系统性改动 |

### 0.3 粗略估算公式（用于路线图排期）

- **预计改动行数（Touched LOC）**：按模块 LOC 的 5%–20% 估算（取决于是否涉及状态机/接口语义）。
- **人日估算（单人）**：
  - `人日 ≈ (代码改动行数 / 120) * 复杂度系数 + (新增/修改测试行数 / 200) + 0.5`
  - 其中 0.5 人日用于设计评审/回归定位/文档更新等固定开销。

> 注：该方法的目的不是“精确到小时”，而是用统一口径把任务排序与拆并；最终以实际实现与测试结果为准滚动调整。

## 1. 第一阶段：Critical 修复（1–2 周）——互通性破坏问题

### 1.1 目标

- 将 HSMS 控制流从“超时即断线收敛”调整为更贴近 SEMI E37 的语义（尤其是 Select/Deselect）。
- 消除 Deselect 过程中的控制/数据消息竞态，保证线序可预测并能与主流 Host/Equipment 互通。

### 1.2 范围（Issue IDs）

- `AUDIT-HSMS-001`（Select.req 超时语义）
- `AUDIT-HSMS-003`（Deselect.req 后 data 未立即阻断 + 写入优先级）
- 建议同包处理：`AUDIT-HSMS-002`（Deselect 语义回到 NOT_SELECTED 而非断线）
- 回归用例：`GAP-HSMS-001`、`GAP-HSMS-002`、`GAP-HSMS-004`

### 1.3 可并行任务

- **P1-A（状态机语义）**：处理 `AUDIT-HSMS-001/002` 的状态转换与超时策略（主要集中在 `src/hsms/session.cpp`）。
- **P1-B（写入与竞态）**：处理 `AUDIT-HSMS-003` 的控制消息优先级/写门禁/取消等待写入 data（涉及 `src/hsms/connection.cpp` + `src/hsms/session.cpp`）。
- **P1-C（测试回归）**：实现 `GAP-HSMS-001/002/004`，把关键竞态与清理路径转为自动化回归（主要修改 `tests/test_hsms_transport.cpp`）。

> 并行注意：P1-A 与 P1-B 都会触碰 `src/hsms/session.cpp`，建议按“先设计接口/状态语义、后分支实现、最后合并回归”的方式降低冲突。

### 1.4 阻塞依赖

- **规范锚点确认**：需要确认 SEMI E37 对 NOT_SELECTED/SELECTED/SEPARATE 的精确语义（当前仓库未包含 E37 原文）。
- **线序约束定义**：需要在库内明确“控制消息是否抢占 data”“Deselect/Separate 期间 data 的处理策略（丢弃/失败/延迟）”作为可测试契约。

### 1.5 验收标准（可自动化）

- Select：
  - `Select.req` T6 超时后状态回到 NOT_SELECTED（库内 `connected`），并且底层连接保持可用（不应立即 `disconnected`）。
  - 迟到 `Select.rsp` 不得将会话置为 `selected`（覆盖 `GAP-HSMS-001`）。
- Deselect：
  - 收到 `Deselect.req` 后立即阻断 data：任何 `Deselect.req` 之后的 data 不得出现在 wire 上（覆盖 `GAP-HSMS-002`）。
  - 完成 Deselect 后进入 NOT_SELECTED，而非强制断线（覆盖 `AUDIT-HSMS-002`）。
- Separate：
  - 收到 `Separate.req` 后 pending/等待者被取消且资源清理完整（覆盖 `GAP-HSMS-004`）。

### 1.6 工作量估算（单人，粗略）

| 工作包 | 主要触及模块 | 预计改动（Touched LOC） | 复杂度 | 人日（估算） |
|---|---|---:|---:|---:|
| P1-A：Select/Deselect 状态机语义 | HSMS（`session.cpp`） | 120–220 | 2.0 | 3.0–5.0 |
| P1-B：控制优先级/写门禁与取消等待 data | HSMS（`session.cpp`+`connection.cpp`） | 180–320 | 2.0 | 5.0–8.0 |
| P1-C：关键竞态/清理回归测试 | HSMS tests | 180–320 | 1.5 | 2.0–3.5 |

> 日历时间：若 2–3 人并行，通常可压缩到 1–2 周；若单人串行，可能接近 2 周上限。

## 2. 第二阶段：High 修复（2–3 周）——资源安全 + 协议正确性

### 2.1 目标

- 为 SECS-II 解码引入可配置的“硬上限 + 总预算”，确保不可信输入下资源上界可预测，并避免 `noexcept` 下的进程终止。
- 完成 HSMS Reject 与 status code 语义补齐，使协议错误能“可诊断、可互通”。
- 明确 SECS-I 与 c_dump 的兼容策略，固化字节级行为与输入域契约。

### 2.2 范围（Issue IDs）

- SECS-II 资源预算：`AUDIT-SECSII-001/002/003/004/005`
- HSMS 协议正确性：`AUDIT-HSMS-004/005/006/007`（并回填 `GAP-HSMS-003`）
- SECS-I 兼容策略：`AUDIT-SECSI-001/002/003`（并回填 `GAP-SECSI-002`）
- Protocol：建议提前铺垫测试缝隙 `GAP-PROTO-001`（若需要）并实现 `GAP-PROTO-002`

### 2.3 可并行任务

- **P2-A（SECS-II 预算与配置）**：在 `src/ii/codec.cpp` 引入 `ListItems/PayloadBytes/TotalBytes/TotalItems/Depth` 限制，并提供配置入口。
- **P2-B（HSMS Reject + status）**：实现 `Reject.req` reason code，细化 `Select.rsp` status，并补齐 header 字段语义测试。
- **P2-C（SECS-I 兼容决策落地）**：对 block_number 8 位/15 位做决策并修改编解码与测试；同时固化 device_id 合法域与 checksum 口径。
- **P2-D（Protocol 清理与极端分支）**：补齐断线取消 pending（`GAP-PROTO-002`）；评估/实现 system_bytes 的“小空间注入”以覆盖 exhaustion（`GAP-PROTO-001`）。

### 2.4 阻塞依赖

- **SECS-II 上限值的产品决策**：例如 `kMaxDecodePayloadBytes` 取 1MB/4MB、`kMaxDecodeTotalBytes` 取 64MB 等，需要与上层消息大小限制与设备约束对齐。
- **SECS-I block_number 策略**：这是互通策略决策（可能是 Breaking Change），需在目标互通对象（Host/Equipment）范围内定稿。

### 2.5 验收标准（可自动化）

- SECS-II：
  - 在 `reserve/分配` 前拒绝超限 ListItems/PayloadBytes；对组合型消息执行 TotalBytes/TotalItems 预算拒绝（覆盖 `AUDIT-SECSII-001/003/004`）。
  - 深度上限可配置且边界行为有双侧断言（覆盖 `AUDIT-SECSII-005` + `GAP-SECSII-001`）。
- HSMS：
  - 能编码/解码 Reject.req reason code，并对未知/非法控制消息发送 Reject（覆盖 `AUDIT-HSMS-004`）。
  - `Select.rsp` status 可区分主要拒绝原因，且拒绝策略一致（覆盖 `AUDIT-HSMS-005`）。
  - Linktest 连续失败阈值可配置并具备回归用例（覆盖 `AUDIT-HSMS-006` + `GAP-HSMS-003`）。
- SECS-I：
  - block_number 策略与 c_dump 对齐方案被测试固化（覆盖 `AUDIT-SECSI-001`）。
  - device_id 边界值（0x7FFF/0x8000）行为由测试固化（覆盖 `AUDIT-SECSI-002` + `GAP-SECSI-002`）。
- Protocol：
  - 断线时 pending 请求被立即取消且 system_bytes 释放（覆盖 `GAP-PROTO-002`）。

### 2.6 工作量估算（单人，粗略）

| 工作包 | 主要触及模块 | 预计改动（Touched LOC） | 复杂度 | 人日（估算） |
|---|---|---:|---:|---:|
| P2-A：SECS-II 硬上限 + 总预算 + 配置 | SECS-II（`codec.cpp`+headers） | 220–420 | 1.5–2.0 | 4.5–8.0 |
| P2-B：HSMS Reject + status + header 语义 | HSMS（`message.cpp`/`session.cpp`） | 180–320 | 1.5 | 3.5–6.0 |
| P2-C：SECS-I block_number 策略落地 + 边界测试 | SECS-I（`block.cpp`+tests） | 160–320 | 1.5–2.0 | 3.5–7.0 |
| P2-D：Protocol 断线清理 +（可选）exhaustion 测试缝隙 | Protocol（`session.cpp`+tests） | 160–360 | 1.5–2.0 | 3.0–7.5 |

> 日历时间：若 3 人并行（A/B/C），D 作为穿插任务，可落在 2–3 周区间内；若单人推进，可能超过 3 周。

## 3. 第三阶段：Medium 修复（1–2 周）——测试补齐 + 工程质量

### 3.1 目标

- 按 task-4 将剩余缺口用例落地，覆盖正常流程、边界条件与错误恢复。
- 建立可持续的覆盖率基线记录与回填机制，避免“修一次、坏一次”。

### 3.2 范围（以 GAP-* 为主）

优先完成（High 优先级）：

- HSMS：`GAP-HSMS-001/002/003/004`
- SECS-I：`GAP-SECSI-001/002`
- SECS-II：`GAP-SECSII-002`
- Protocol：`GAP-PROTO-002`

随后补齐（Medium/Low）：

- `GAP-SECSII-001/003/004`
- `GAP-PROTO-001`（若第二阶段未完成）

### 3.3 可并行任务

- **P3-A（HSMS 测试包）**：集中补齐 HSMS 四个 GAP 用例，覆盖竞态、优先级、清理。
- **P3-B（SECS-II 测试包）**：补齐截断、小边界与规范化编码断言。
- **P3-C（SECS-I 测试包）**：将“block_number 非连续”从 Reassembler 推进到 StateMachine，并固化输入域边界。
- **P3-D（Protocol 测试包）**：补齐断线自动取消 pending，并在具备测试缝隙后覆盖 system_bytes exhaustion。

### 3.4 阻塞依赖

- 部分 GAP 用例依赖第二阶段提供的“可配置项/测试缝隙”（例如 Linktest 连续失败阈值、system_bytes 小空间注入）。

### 3.5 验收标准（可自动化）

- 所有新增用例通过，并对关键行为给出明确断言（状态、错误码、线序、资源释放）。
- 覆盖率基线可记录并可回填（建议用 `gcovr --branches` 与 JSON/HTML 输出），关注分支覆盖而非仅行覆盖。
- 针对 Critical/High 区域（HSMS/SECS-II decode/Protocol pending）至少具备“回归触发即失败”的强断言，避免 silent regression。

### 3.6 工作量估算（单人，粗略）

| 工作包 | 主要触及模块 | 预计改动（Touched LOC） | 复杂度 | 人日（估算） |
|---|---|---:|---:|---:|
| P3-A：HSMS 缺口用例（4） | HSMS tests | 240–420 | 1.5 | 2.5–4.0 |
| P3-B：SECS-II 缺口用例（3–4） | SECS-II tests | 160–280 | 1.0–1.5 | 1.5–3.0 |
| P3-C：SECS-I 缺口用例（2） | SECS-I tests | 160–260 | 1.5 | 2.0–3.5 |
| P3-D：Protocol 缺口用例（1–2） | Protocol tests | 160–320 | 1.5–2.0 | 2.0–4.5 |

## 4. 建议的执行顺序（总结）

1. 先落地 HSMS 的关键语义与线序（阶段 1），否则后续 Reject/status/测试都容易反复返工。
2. 同步推进 SECS-II 的资源预算（阶段 2），避免在任何“可被外部输入触达”的场景中暴露 DoS 风险。
3. 兼容策略（SECS-I block_number）应尽早决策并固化，否则测试与对端联调会持续摇摆。
4. 用阶段 3 把所有高风险路径变成“可重复、可回归”的测试契约，降低长期维护成本。

