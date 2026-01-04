# SECS Library Fixes - Development Plan

## Overview
修复 SECS 库中的关键安全漏洞、协议语义错误和测试覆盖缺口，确保符合 SEMI 标准并达到 ≥90% 测试覆盖率。

## Task Breakdown

---

### Phase 1: Critical 修复（任务可并行执行）

#### Task 1.1: HSMS 状态机语义 + Deselect 竞态修复
- **ID**: task-1.1
- **问题编号**: AUDIT-HSMS-001, AUDIT-HSMS-003, AUDIT-HSMS-004
- **Description**:
  - 修复 `NOT_SELECTED` 到 `CONNECTED` 直接跳转（缺少 `SELECTED` 中间态）
  - 修复 Deselect.req 和 Deselect.rsp 竞态窗口（需在 Deselect.rsp 后才能转 `NOT_SELECTED`）
  - 修复状态检查逻辑：仅 `SELECTED` 态允许数据消息传输
- **File Scope**:
  - `src/hsms/hsms_state_machine.cpp`
  - `src/hsms/hsms_session.cpp`
  - `tests/hsms/test_hsms_state_machine.cpp`
  - `tests/hsms/test_hsms_session.cpp`
- **Dependencies**: None
- **Test Command**:
  ```bash
  cmake --build build && \
  ctest --test-dir build -R "test_hsms_(state_machine|session)" --output-on-failure && \
  gcovr -r . --filter 'src/hsms/' --print-summary --fail-under-line=90
  ```
- **Test Focus**:
  - 状态转换序列：`NOT_CONNECTED` → `CONNECTED` → `NOT_SELECTED` → `SELECTED`
  - Deselect 竞态场景：Deselect.req 发送后、Deselect.rsp 收到前收到数据消息（应保持 `SELECTED` 直到 rsp）
  - 非 `SELECTED` 态收到 data message 应拒绝
  - Select.req 重试场景（T3 超时后重发）
- **修复要点**:
  - 引入 `SELECTED` 状态枚举值
  - Select.rsp 成功后转 `SELECTED`
  - Deselect.rsp 收到后才转 `NOT_SELECTED`
  - 数据消息发送前检查 `state == SELECTED`

---

#### Task 1.2: SECS-II 解码硬上限（ListItems/PayloadBytes）
- **ID**: task-1.2
- **问题编号**: AUDIT-SECSII-001, AUDIT-SECSII-002
- **Description**:
  - 添加单个 List 的 items 数量硬上限（建议 65535）
  - 添加单个 Payload 的 bytes 硬上限（建议 16MB）
  - 解码前检查，超限返回错误而非崩溃
- **File Scope**:
  - `src/secs-ii/secs_ii_decoder.cpp`
  - `src/secs-ii/secs_ii_limits.h` (新建配置头文件)
  - `tests/secs-ii/test_secs_ii_decoder.cpp`
- **Dependencies**: None
- **Test Command**:
  ```bash
  cmake --build build && \
  ctest --test-dir build -R "test_secs_ii_decoder" --output-on-failure && \
  gcovr -r . --filter 'src/secs-ii/' --print-summary --fail-under-line=90
  ```
- **Test Focus**:
  - List items = 65535（通过）
  - List items = 65536（拒绝，返回错误）
  - Payload bytes = 16MB（通过）
  - Payload bytes = 16MB + 1（拒绝，返回错误）
  - 嵌套 List 边界测试（最大深度 + 最大 items）
- **修复要点**:
  - 定义宏 `SECSII_MAX_LIST_ITEMS` 和 `SECSII_MAX_PAYLOAD_BYTES`
  - 解码 List header 后检查 `items_count <= SECSII_MAX_LIST_ITEMS`
  - 解码 Payload header 后检查 `payload_size <= SECSII_MAX_PAYLOAD_BYTES`
  - 错误码：`ERROR_LIST_TOO_LARGE`, `ERROR_PAYLOAD_TOO_LARGE`

---

### Phase 2: High 修复（任务可并行执行，依赖 Phase 1 部分完成）

#### Task 2.1: SECS-II 总预算（TotalBytes/TotalItems）+ 深度配置
- **ID**: task-2.1
- **问题编号**: AUDIT-SECSII-003, AUDIT-SECSII-004
- **Description**:
  - 添加整个消息的总预算配置（`max_total_bytes`, `max_total_items`）
  - 添加嵌套深度配置（`max_nesting_depth`，默认 8）
  - 解码过程中累计检查，超限中止
- **File Scope**:
  - `src/secs-ii/secs_ii_decoder.cpp`
  - `src/secs-ii/secs_ii_limits.h`
  - `src/secs-ii/secs_ii_decoder_context.h` (新建解码上下文结构)
  - `tests/secs-ii/test_secs_ii_decoder.cpp`
- **Dependencies**: depends on task-1.2（复用限制值配置机制）
- **Test Command**:
  ```bash
  cmake --build build && \
  ctest --test-dir build -R "test_secs_ii_decoder" --output-on-failure && \
  gcovr -r . --filter 'src/secs-ii/' --print-summary --fail-under-line=90
  ```
- **Test Focus**:
  - 总 items 达到预算边界（通过）
  - 总 items 超预算（拒绝）
  - 总 bytes 达到预算边界（通过）
  - 总 bytes 超预算（拒绝）
  - 嵌套深度 = 8（通过）
  - 嵌套深度 = 9（拒绝）
- **修复要点**:
  - 引入 `DecoderContext` 结构体（成员：`total_items`, `total_bytes`, `current_depth`, 配置阈值）
  - 每解码一个 item/byte 累加计数器
  - 递归/迭代解码 List 时检查 `current_depth <= max_nesting_depth`
  - 错误码：`ERROR_TOTAL_BUDGET_EXCEEDED`, `ERROR_NESTING_TOO_DEEP`

---

#### Task 2.2: HSMS Reject + Select.rsp status + Linktest 阈值
- **ID**: task-2.2
- **问题编号**: AUDIT-HSMS-002, AUDIT-HSMS-005, AUDIT-HSMS-006
- **Description**:
  - 修复 Reject.req 消息类型（0x03 → 0x07）
  - Select.rsp 根据当前状态设置 status（已 selected → 0x01, 成功 → 0x00）
  - 添加 Linktest 连续失败阈值配置（`max_linktest_failures`，建议 3），超限触发断线
- **File Scope**:
  - `src/hsms/hsms_message_builder.cpp`
  - `src/hsms/hsms_session.cpp`
  - `src/hsms/hsms_config.h`
  - `tests/hsms/test_hsms_message_builder.cpp`
  - `tests/hsms/test_hsms_session.cpp`
- **Dependencies**: depends on task-1.1（依赖状态机修复后的 `SELECTED` 状态）
- **Test Command**:
  ```bash
  cmake --build build && \
  ctest --test-dir build -R "test_hsms_(message_builder|session)" --output-on-failure && \
  gcovr -r . --filter 'src/hsms/' --print-summary --fail-under-line=90
  ```
- **Test Focus**:
  - Reject.req 消息头 PType = 0x07
  - Select.rsp 在 `SELECTED` 态返回 status = 0x01
  - Select.rsp 在 `NOT_SELECTED` 态返回 status = 0x00
  - Linktest 连续失败 2 次（保持连接）
  - Linktest 连续失败 3 次（触发断线）
  - Linktest 成功后失败计数器清零
- **修复要点**:
  - 修改 `build_reject_req()` 中 PType 字段为 0x07
  - Select.rsp 构建前检查 `state == SELECTED`，设置对应 status
  - 新增成员变量 `linktest_failure_count`，超限调用 `close_connection()`

---

#### Task 2.3: SECS-I block_number 15 位 + c_dump 兼容模式
- **ID**: task-2.3
- **问题编号**: AUDIT-SECSI-001, AUDIT-SECSI-004
- **Description**:
  - 修复 block_number 为 15 位无符号整数（当前为 8 位）
  - 修复 `c_dump()` 输出为 0xFFFF 时的 uint16_t 类型转换错误
- **File Scope**:
  - `src/secs-i/secs_i_block.h`
  - `src/secs-i/secs_i_encoder.cpp`
  - `src/secs-i/secs_i_decoder.cpp`
  - `src/c_api/c_dump.c`
  - `tests/secs-i/test_secs_i_block.cpp`
  - `tests/c_api/test_c_dump.cpp`
- **Dependencies**: None
- **Test Command**:
  ```bash
  cmake --build build && \
  ctest --test-dir build -R "test_secs_i_block|test_c_dump" --output-on-failure && \
  gcovr -r . --filter 'src/secs-i/|src/c_api/' --print-summary --fail-under-line=90
  ```
- **Test Focus**:
  - block_number = 0（边界值）
  - block_number = 32767（15 位最大值）
  - block_number = 32768（超限，应回绕或拒绝）
  - `c_dump()` 输出 block_number = 0xFFFF 不截断
- **修复要点**:
  - `Block` 结构体 `block_number` 改为 `uint16_t`，掩码 `& 0x7FFF`（15 位）
  - 编码/解码时按 15 位处理（MSB 为 EOM 标志位）
  - `c_dump()` 中使用 `uint16_t` 而非 `uint8_t` 存储 block_number

---

#### Task 2.4: Protocol 断线清理 + system_bytes exhaustion
- **ID**: task-2.4
- **问题编号**: AUDIT-PROTOCOL-002, AUDIT-PROTOCOL-003
- **Description**:
  - 添加断线时事务表清理逻辑（所有待响应事务触发超时回调）
  - 添加 `system_bytes` 耗尽处理（应拒绝新事务并返回错误）
- **File Scope**:
  - `src/protocol/protocol_session.cpp`
  - `src/protocol/transaction_manager.cpp`
  - `tests/protocol/test_protocol_session.cpp`
  - `tests/protocol/test_transaction_manager.cpp`
- **Dependencies**: None
- **Test Command**:
  ```bash
  cmake --build build && \
  ctest --test-dir build -R "test_protocol_(session|transaction_manager)" --output-on-failure && \
  gcovr -r . --filter 'src/protocol/' --print-summary --fail-under-line=90
  ```
- **Test Focus**:
  - 断线时清理 5 个待响应事务，每个触发 timeout 回调
  - `system_bytes` 从 0 分配到 4294967295（通过）
  - `system_bytes` 耗尽后拒绝新事务（返回 `ERROR_NO_SYSTEM_BYTES`）
  - `system_bytes` 释放后可重新分配
- **修复要点**:
  - `on_disconnect()` 中遍历事务表，对每个未完成事务调用 `on_timeout()`
  - 维护 `used_system_bytes` 集合（或位图），分配前检查可用性
  - 耗尽时返回错误码，外层重试或降级处理

---

### Phase 3: Medium 修复（任务可并行执行，依赖 Phase 2 完成）

#### Task 3.1: SECS-II 测试补齐（GAP-SECSII-002/003/004）
- **ID**: task-3.1
- **问题编号**: GAP-SECSII-002, GAP-SECSII-003, GAP-SECSII-004
- **Description**:
  - 补充 List items 边界测试（0, 1, 65535, 65536）
  - 补充总预算边界测试（总 items/bytes 接近阈值、超阈值）
  - 补充嵌套深度边界测试（深度 8, 9）
- **File Scope**:
  - `tests/secs-ii/test_secs_ii_decoder.cpp`
  - `tests/secs-ii/test_secs_ii_encoder.cpp`
- **Dependencies**: depends on task-2.1（依赖总预算和深度限制实现）
- **Test Command**:
  ```bash
  cmake --build build && \
  ctest --test-dir build -R "test_secs_ii_(decoder|encoder)" --output-on-failure && \
  gcovr -r . --filter 'src/secs-ii/' --print-summary --fail-under-line=90
  ```
- **Test Focus**:
  - List items 边界：0（空 List）、1（单元素）、65535（最大）、65536（拒绝）
  - 总 items 边界：总数 = 配置值（通过）、总数 = 配置值 + 1（拒绝）
  - 总 bytes 边界：总数 = 配置值（通过）、总数 = 配置值 + 1（拒绝）
  - 嵌套深度：8 层（通过）、9 层（拒绝）
- **修复要点**:
  - 无需修复代码，仅补充测试用例
  - 使用参数化测试覆盖所有边界值
  - 验证错误码和错误消息正确性

---

#### Task 3.2: SECS-I StateMachine block_number 非连续测试
- **ID**: task-3.2
- **问题编号**: GAP-SECSI-001
- **Description**:
  - 补充 block_number 非连续场景测试（接收到乱序、重复、跳号的 block）
- **File Scope**:
  - `tests/secs-i/test_secs_i_state_machine.cpp`
- **Dependencies**: depends on task-2.3（依赖 15 位 block_number 实现）
- **Test Command**:
  ```bash
  cmake --build build && \
  ctest --test-dir build -R "test_secs_i_state_machine" --output-on-failure && \
  gcovr -r . --filter 'src/secs-i/' --print-summary --fail-under-line=90
  ```
- **Test Focus**:
  - 正常序列：block 0, 1, 2（通过）
  - 跳号：block 0, 2（缺少 1，应拒绝或超时）
  - 重复：block 0, 1, 1（重复 1，应拒绝）
  - 乱序：block 0, 2, 1（应拒绝）
  - 回绕：block 32766, 32767, 0（15 位回绕，通过）
- **修复要点**:
  - 无需修复代码（假设现有逻辑已检查连续性）
  - 补充测试用例验证状态机拒绝逻辑
  - 验证错误码 `ERROR_BLOCK_NUMBER_MISMATCH`

---

#### Task 3.3: Protocol 测试补齐（exhaustion + 断线清理）
- **ID**: task-3.3
- **问题编号**: GAP-PROTOCOL-001
- **Description**:
  - 补充 `system_bytes` 耗尽边界测试（4294967295 分配完毕后拒绝）
  - 补充断线清理测试（验证所有待响应事务触发 timeout）
- **File Scope**:
  - `tests/protocol/test_transaction_manager.cpp`
  - `tests/protocol/test_protocol_session.cpp`
- **Dependencies**: depends on task-2.4（依赖 exhaustion 和断线清理实现）
- **Test Command**:
  ```bash
  cmake --build build && \
  ctest --test-dir build -R "test_protocol_(transaction_manager|session)" --output-on-failure && \
  gcovr -r . --filter 'src/protocol/' --print-summary --fail-under-line=90
  ```
- **Test Focus**:
  - 分配 2^32 个 system_bytes（耗尽）后拒绝新事务
  - 释放 1 个 system_bytes 后可再次分配
  - 断线时清理 10 个待响应事务，每个触发 timeout 回调且携带正确 transaction_id
  - 断线后事务表为空
- **修复要点**:
  - 无需修复代码，仅补充测试用例
  - 使用 mock 回调验证 timeout 调用次数和参数
  - 验证事务表状态一致性

---

## Acceptance Criteria

### 功能正确性
- [ ] HSMS 状态机严格遵循 `NOT_CONNECTED` → `CONNECTED` → `NOT_SELECTED` → `SELECTED` 转换序列
- [ ] Deselect 竞态窗口关闭（Deselect.rsp 后才转 `NOT_SELECTED`）
- [ ] SECS-II 所有限制值生效（List items、Payload bytes、总预算、嵌套深度）
- [ ] SECS-I block_number 支持 15 位（0-32767）
- [ ] HSMS Reject.req PType = 0x07
- [ ] HSMS Select.rsp status 正确反映当前状态
- [ ] HSMS Linktest 连续失败 3 次触发断线
- [ ] Protocol 断线时所有待响应事务触发 timeout 回调
- [ ] Protocol `system_bytes` 耗尽时拒绝新事务

### 测试覆盖率
- [ ] 所有模块代码覆盖率 ≥90%（gcovr 行覆盖率）
- [ ] 所有边界值场景有对应测试用例
- [ ] 所有错误路径有测试验证

### 安全性
- [ ] 所有解码器对畸形输入拒绝而非崩溃
- [ ] 所有资源耗尽场景有明确错误处理

### 向后兼容性
- [ ] C API (`c_dump`) 与现有调用方兼容
- [ ] 配置参数提供合理默认值（无需调用方显式设置）

---

## Technical Notes

### 关键技术决策
1. **状态机引入 `SELECTED` 状态**：符合 SEMI E37 标准，明确数据传输许可态
2. **SECS-II 限制值配置化**：通过 `secs_ii_limits.h` 统一管理，支持编译期覆盖
3. **SECS-I block_number 15 位**：MSB 为 EOM 标志位，有效位 0-14（0-32767）
4. **Linktest 阈值默认 3**：平衡网络抖动容忍度和故障检测速度
5. **system_bytes exhaustion 使用位图**：4GB 内存开销可接受（512MB 位图），O(1) 分配/释放

### 实现约束
- **Phase 依赖**：Phase 2 部分任务依赖 Phase 1（状态机和限制值框架），Phase 3 依赖 Phase 2（限制值实现）
- **并行执行**：同一 Phase 内任务无依赖关系，可并行开发/测试
- **测试隔离**：每个任务测试独立运行，避免共享状态污染
- **覆盖率工具**：使用 gcovr（CMake 集成），编译时需启用 `--coverage` 标志

### 风险点
- **Task 1.1 状态机重构**：可能影响现有调用方，需回归测试所有 HSMS 场景
- **Task 2.1 总预算引入**：需评估性能影响（每解码一个 item 累加计数器）
- **Task 2.4 断线清理**：需确保 timeout 回调中不触发新的网络操作（避免递归）

### 下一步行动
1. 执行 Phase 1 任务（task-1.1, task-1.2）
2. Phase 1 测试通过后，并行执行 Phase 2 任务
3. Phase 2 完成后，并行执行 Phase 3 测试补齐任务
4. 全量回归测试 + 覆盖率报告
5. 更新文档（CHANGELOG, API 文档）
