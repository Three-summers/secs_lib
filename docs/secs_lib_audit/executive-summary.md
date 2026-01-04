# SECS 库综合审查执行摘要（Task-5）

- 日期：2026-01-03
- 执行者：Codex（静态汇总审查）
- 代码基线：`9b89aa3d3ea0d437fef946d9eb6e0152d79b89bd`
- 输入来源：
  - task-1：`.claude/specs/secs_lib_audit/hsms-interop-analysis.md`
  - task-2：`.claude/specs/secs_lib_audit/secsii-resource-safety.md`
  - task-3：`.claude/specs/secs_lib_audit/secs1-compat-fixes.md`
  - task-4：`.claude/specs/secs_lib_audit/test-coverage-gaps.md`

## 1. 审查范围

本次综合审查覆盖 secs_lib 的以下模块与对比基线：

- **HSMS（SEMI E37）控制消息互通性**：Select/Deselect/Linktest/Separate/Reject 的状态机语义、超时（T6）策略、控制/数据消息写入顺序与竞态。
- **SECS-II 编解码资源消耗**：递归深度上限、超大 length 声明、宽 List/大 payload 的内存与 CPU 上界、`noexcept` 下的 OOM 行为。
- **SECS-I 与 c_dump 字节级兼容性**：Header 字段与 checksum 的字节布局对齐、block_number 语义差异、输入域约束差异。
- **测试覆盖缺口**：HSMS / SECS-I / SECS-II / Protocol 四层的 12 项缺失场景设计与优先级。

审查方法为**静态代码审查 + 现有测试阅读 + 与 c_dump 行为对照**（不修改代码，不执行构建/测试）。

## 2. 关键发现（按严重等级）

### Critical

1. **HSMS Select.req 超时语义偏离 E37（断线而非回到 NOT_SELECTED）**：`AUDIT-HSMS-001`  
   典型后果：对端将其视为网络异常而非选择失败，容易触发重连抖动与互通性故障。
2. **HSMS Deselect.req 期间无法“立即停止发送 data”（控制消息无优先级，存在竞态）**：`AUDIT-HSMS-003`  
   典型后果：对端在发出 Deselect.req 后仍收到 data，常见实现会判为协议错误并 Separate/断线。
3. **SECS-II 允许超大 List 元素个数并 `reserve(length)`（仅 4 字节头即可触发巨大分配）**：`AUDIT-SECSII-001`  
   典型后果：`noexcept` 路径触发 `std::bad_alloc` 时可能 `std::terminate`，形成 DoS。
4. **SECS-II 允许 ASCII/Binary 单项 16MB 的“整块分配+拷贝”**：`AUDIT-SECSII-003`  
   典型后果：大 payload/并发解码导致内存与 CPU 峰值不可控，极端情况下同样可能 DoS。

### High

1. **HSMS Deselect 被实现为“完成后断线”，无法进入 NOT_SELECTED**：`AUDIT-HSMS-002`
2. **HSMS Reject 未实现（不发送 Reject.req，也不解析 reason code）**：`AUDIT-HSMS-004`
3. **HSMS Linktest 任一失败即断线，且连续失败阈值不可配置**：`AUDIT-HSMS-006`
4. **SECS-II 缺乏“总预算”（Total Bytes/Items）导致组合消息轻易超过 64MB**：`AUDIT-SECSII-004`
5. **SECS-I block_number 高位语义与 c_dump 不一致（block_number>=256 时字节级差异）**：`AUDIT-SECSI-001`

### Medium

- **HSMS Select.rsp 状态码过于粗粒度（仅 0/1）**：`AUDIT-HSMS-005`
- **HSMS Linktest/Separate 的 SessionID 字段语义需与 E37 严格对齐**：`AUDIT-HSMS-007`
- **SECS-II decode 深度上限默认值与外部参考（如 16）可能不一致，且需可配置**：`AUDIT-SECSII-005`
- **SECS-I 输入域约束差异（device_id>=0x8000 的歧义）需在 API 契约中固化**：`AUDIT-SECSI-002`

### Low

- **SECS-I checksum“是否包含 length 字节”的口径不一致（文档/描述层面）**：`AUDIT-SECSI-003`
- **测试覆盖空白**：12 项缺失场景（见 `GAP-*` 系列条目；其中多项为 High 优先级）。

## 3. 总体风险评级

**总体风险评级：High（偏 Critical）**  
理由：存在多项 **互通性破坏（HSMS 状态机语义与控制/数据写入顺序）** 与 **资源耗尽（SECS-II 宽 List/大 payload/总预算缺失）** 的高影响问题；其中部分问题在 `noexcept` 路径下可能直接导致进程终止，风险上限很高。

## 4. 推荐行动项（按优先级）

1. **优先修复 HSMS 的“状态机语义 + 写入优先级”**：先确保 Select/Deselect 在 E37 语义下可与主流 Host/Equipment 互通（对应 `AUDIT-HSMS-001/003/002`）。
2. **为 SECS-II 解码引入“硬上限 + 总预算”**：至少同时限制 `ListItems`、`PayloadBytes`、`TotalBytes/TotalItems`，并在 `reserve/分配` 前拒绝（对应 `AUDIT-SECSII-001/003/004`）。
3. **补齐 HSMS Reject 与 status code 语义**：实现 Reject.req reason code 与未知/非法控制消息的拒绝反馈，细化 Select.rsp status（对应 `AUDIT-HSMS-004/005/007`）。
4. **明确 SECS-I 与 c_dump 的兼容策略**：对 block_number 的 8 位/15 位语义做决策并固化（对应 `AUDIT-SECSI-001`），同时补齐关键边界测试。
5. **按 task-4 清单补齐测试**：优先覆盖竞态与清理场景（HSMS/Protocol），将风险从“运行时爆炸”转为“测试期暴露”。

