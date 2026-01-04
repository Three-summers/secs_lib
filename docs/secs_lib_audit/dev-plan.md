# SECS 库 c_dump 兼容性审查报告 - 开发计划

## 概述
对 secs_lib（现代 C++20 实现）与 c_dump（生产验证的 C 参考实现）进行全面对比审查，识别协议互通性风险、资源安全缺陷与测试覆盖空白，生成详细的审查报告与修复建议文档，但不自动修复代码。

## 技术栈对比

| 维度 | c_dump（参考实现） | secs_lib（当前实现） |
|------|-------------------|---------------------|
| **语言** | C（ANSI C） | C++20 |
| **内存管理** | 手动 malloc/free | RAII + std::vector |
| **错误处理** | 错误码 | std::expected/错误码混合 |
| **并发模型** | 单线程回调 | Asio 异步 I/O |
| **测试验证** | 生产环境验证 | 单元测试 + 兼容性测试 |
| **SECS-I 组帧** | secs_I.c（定长 buffer） | secs1/block.cpp（动态 buffer） |
| **SECS-II 编解码** | secs_II.c（递归解析） | ii/codec.cpp（递归 + 深度限制） |
| **HSMS 传输** | 无（仅 SECS-I） | hsms/connection.cpp（TCP） |

## 关键发现总结

### Critical 级别（互通性破坏风险）
1. **HSMS 控制消息处理不一致**：c_dump 无 HSMS 实现，但 secs_lib 的 Select.req 超时处理可能与其他生产系统互通性冲突
2. **SECS-II 解码资源耗尽风险**：超大 List length 声明可能触发内存耗尽，c_dump 的固定 buffer 天然免疫，secs_lib 需显式限制

### High 级别（协议正确性缺陷）
3. **SECS-I Block 校验和算法差异**：wait_bit 字段的移位逻辑在两个实现中需严格对齐（已通过 test_c_dump_secs1_block_compat.cpp 验证，但边界场景未覆盖）
4. **SECS-II 编码字节序一致性**：多字节整数（U2/U4/I2/I4/F4/F8）大端序编码需与 c_dump 逐字节一致（已部分覆盖，缺失边界值测试）

### Medium 级别（健壮性不足）
5. **错误恢复路径不完整**：c_dump 在解码失败时清理状态机，secs_lib 的异常安全需补充验证
6. **定时器精度差异**：c_dump 基于轮询，secs_lib 基于 Asio，T1/T2/T3 超时边界行为需对齐

### Low 级别（工程质量）
7. **测试覆盖率空白**：12 个缺失场景（详见任务 4）

## 任务分解

### Task 1: HSMS 控制消息互通性验证审查 [Critical]
- **ID**: task-1
- **描述**: 分析 secs_lib 的 HSMS Select/Deselect/LinkTest/Separate/Reject 控制消息的超时处理、状态转换、错误码映射是否符合 SEMI E37 规范，对比业界常见实现（如 Open SECS）识别互通性风险点，生成详细的行为对比表与修复建议
- **文件范围**:
  - `src/hsms/session.cpp`（状态机实现）
  - `src/hsms/connection.cpp`（消息收发）
  - `src/hsms/message.cpp`（控制消息编解码）
  - `include/secs/hsms/session.hpp`（状态定义）
  - `tests/test_hsms_transport.cpp`（现有测试覆盖分析）
  - 输出：`.claude/specs/secs_lib_audit/hsms-interop-analysis.md`
- **依赖**: 无
- **测试命令**: `cmake --build /home/say/code/cpp/secs_lib/build && ctest --test-dir /home/say/code/cpp/secs_lib/build -R hsms --output-on-failure --verbose`
- **测试重点**:
  - 识别 Select.req 超时后的状态是否符合 E37 §8.3.2（应回到 NOT_SELECTED）
  - Deselect.req 收到后是否立即停止发送数据消息（互通性关键点）
  - Reject 消息的 reason code 映射是否与常见厂商一致
  - LinkTest 超时次数阈值是否可配置（生产环境要求）
- **验收标准**:
  - 生成包含至少 5 个互通性风险点的分析报告
  - 每个风险点标注严重等级（Critical/High/Medium）
  - 提供具体的修复建议（代码行号 + 期望行为）
  - 附带 SEMI E37 规范引用章节

---

### Task 2: SECS-II 解码资源安全加固审查 [Critical]
- **ID**: task-2
- **描述**: 审查 ii/codec.cpp 的解码器对恶意构造的 SECS-II 消息的资源消耗防护：递归深度限制、超大 length 声明处理、嵌套 List 内存分配策略，对比 c_dump 的固定 buffer 策略，识别 DoS 攻击面并提出修复方案
- **文件范围**:
  - `src/ii/codec.cpp`（解码主逻辑）
  - `src/ii/item.cpp`（Item 构造）
  - `include/secs/ii/codec.hpp`（解码接口）
  - `include/secs/ii/item.hpp`（Item 结构定义）
  - `tests/test_secs2_codec.cpp`（现有测试覆盖分析）
  - 输出：`.claude/specs/secs_lib_audit/secsii-resource-safety.md`
- **依赖**: 无
- **测试命令**: `cmake --build /home/say/code/cpp/secs_lib/build && ctest --test-dir /home/say/code/cpp/secs_lib/build -R secs2_codec --output-on-failure && gcovr -r /home/say/code/cpp/secs_lib --filter 'src/ii/' --branches --print-summary`
- **测试重点**:
  - 当前递归深度限制是否存在（若无，评估最坏情况栈消耗）
  - ASCII/Binary 类型声明 length=0xFFFFFF（16MB）时的行为（应拒绝或流式处理）
  - 嵌套 256 层 List 时的内存分配总量（应 < 64MB 或显式拒绝）
  - 对比 c_dump 的 `max_nested_level=16` 限制，评估是否需要对齐
- **验收标准**:
  - 识别至少 3 个资源耗尽攻击向量
  - 每个向量提供 PoC 构造方法（字节流示例）
  - 计算当前实现的最坏情况资源消耗（内存 + 栈深度）
  - 提供限流策略建议（常量定义 + 校验逻辑位置）
  - 覆盖率报告显示解码错误路径覆盖率 ≥85%

---

### Task 3: SECS-I c_dump 兼容性修复建议 [High]
- **ID**: task-3
- **描述**: 基于 test_c_dump_secs1_block_compat.cpp 的现有覆盖，扩展边界场景测试（device_id=0x7FFF、block_number=0 等），识别 wait_bit/reverse_bit/checksum 计算中的潜在差异，生成字节级对齐的修复清单
- **文件范围**:
  - `src/secs1/block.cpp`（encode_block/decode_block 实现）
  - `include/secs/secs1/block.hpp`（Header 结构定义）
  - `tests/test_c_dump_secs1_block_compat.cpp`（兼容性测试）
  - `tests/test_secs1_framing.cpp`（常规功能测试）
  - 输出：`.claude/specs/secs_lib_audit/secs1-compat-fixes.md`
- **依赖**: 无
- **测试命令**: `cmake --build /home/say/code/cpp/secs_lib/build && ctest --test-dir /home/say/code/cpp/secs_lib/build -R "secs1_.*compat|secs1_framing" --output-on-failure`
- **测试重点**:
  - wait_bit=true 时 Reply_Bit 的移位逻辑（c_dump 按 0x80 移位值相加）
  - device_id 边界值 0x0000/0x7FFF 的编码正确性
  - block_number=0 的合法性（协议允许但罕见）
  - checksum 计算的字节范围（c_dump 包含 length byte + header + data，不含 checksum 字段）
- **验收标准**:
  - 补充至少 6 个边界场景测试用例
  - 所有新增用例与 c_dump 编码字节级一致
  - 若发现差异，文档化差异字节位置 + 根因 + 修复建议
  - 测试通过率 100%（无回归）

---

### Task 4: 测试覆盖补充建议清单 [High]
- **ID**: task-4
- **描述**: 枚举审查中发现的 12 个缺失测试场景（跨 HSMS/SECS-I/SECS-II/Protocol 层），为每个场景生成测试用例设计文档（输入构造 + 期望输出 + 断言要点），但不执行代码编写
- **文件范围**:
  - 所有 `tests/*.cpp` 文件（现有覆盖分析）
  - 输出：`.claude/specs/secs_lib_audit/test-coverage-gaps.md`
- **依赖**: task-1、task-2、task-3（需先完成各模块审查）
- **测试命令**: `gcovr -r /home/say/code/cpp/secs_lib --filter 'src/' --filter 'include/secs/' --html-details /home/say/code/cpp/secs_lib/build/coverage.html --branches --print-summary`
- **测试重点**:
  - **HSMS 缺失场景**（4 个）：
    1. Select.req 超时后立即收到 Select.rsp（竞态处理）
    2. Deselect.req 发送后收到数据消息（应丢弃）
    3. LinkTest 连续超时 3 次触发断连（阈值配置）
    4. Separate.req 收到后的资源清理完整性
  - **SECS-II 缺失场景**（4 个）：
    5. 嵌套深度 = 当前限制值（边界值测试）
    6. Binary length 声明与实际 payload 不匹配（截断检测）
    7. Boolean 类型的非 0/1 值处理（协议未定义行为）
    8. 空 List <L[0]> 的编解码往返一致性
  - **SECS-I 缺失场景**（2 个）：
    9. 多 Block 消息的 block_number 非连续（错误检测）
    10. reverse_bit 与 device_id 最高位冲突场景
  - **Protocol 层缺失场景**（2 个）：
    11. system_bytes 回环重用的冲突检测（32位空间耗尽）
    12. 会话关闭时未应答请求的清理（内存泄漏风险）
- **验收标准**:
  - 生成 12 个测试场景的详细设计文档
  - 每个场景包含：触发条件、输入构造方法、期望行为、断言要点、优先级
  - 标注每个场景对应的覆盖率提升预期（如"新增分支覆盖 +3%"）
  - 提供测试文件放置建议（新增文件 vs 扩展现有文件）

---

### Task 5: 综合审查报告与修复路线图 [Medium]
- **ID**: task-5
- **描述**: 汇总 task-1 至 task-4 的审查结果，生成执行摘要（2 页）+ 详细报告（含风险矩阵、修复优先级排序、工作量估算），提供分阶段修复路线图
- **文件范围**:
  - 输出：`.claude/specs/secs_lib_audit/executive-summary.md`
  - 输出：`.claude/specs/secs_lib_audit/detailed-report.md`
  - 输出：`.claude/specs/secs_lib_audit/fix-roadmap.md`
- **依赖**: task-1、task-2、task-3、task-4
- **测试命令**: 无（文档汇总任务）
- **测试重点**: 不适用
- **验收标准**:
  - 执行摘要包含：审查范围、关键发现（按严重等级）、总体风险评级、推荐行动项
  - 详细报告包含：
    - 风险矩阵（严重等级 × 修复难度，标注每个问题的象限）
    - 问题清单（至少 15 个问题，每个含：ID、描述、影响、根因、建议）
    - 对比表（c_dump vs secs_lib 的设计差异 5 项）
  - 修复路线图包含：
    - 第一阶段（Critical 修复，1-2 周）：互通性破坏问题
    - 第二阶段（High 修复，2-3 周）：资源安全 + 协议正确性
    - 第三阶段（Medium 修复，1-2 周）：测试补齐 + 工程质量
    - 每阶段标注可并行任务、阻塞依赖、验收标准
  - 文档格式符合技术报告规范（标题层级、术语一致性、引用规范）

## 验收标准

- [ ] Task 1-4 的审查报告全部生成，无缺失章节
- [ ] 识别的 Critical 问题 ≥2 个，High 问题 ≥4 个
- [ ] 每个问题提供可执行的修复建议（不含模糊描述如"需要改进"）
- [ ] 补充的测试场景设计 ≥12 个，覆盖 HSMS/SECS-I/SECS-II/Protocol 全层
- [ ] 现有测试通过率 100%（审查过程不破坏已有功能）
- [ ] 代码覆盖率基线记录：`gcovr -r /home/say/code/cpp/secs_lib --filter 'src/' --branches --json -o .claude/specs/secs_lib_audit/coverage-baseline.json`
- [ ] 综合报告的风险矩阵包含 ≥15 个问题项，按象限分类
- [ ] 修复路线图的工作量估算误差 ≤20%（基于代码行数 + 复杂度分析）
- [ ] 所有文档使用中文撰写，技术术语保持英文原文（如 Select.req、HSMS、SECS-II）
- [ ] 文档放置于 `/home/say/code/cpp/secs_lib/.claude/specs/secs_lib_audit/` 目录

## 技术要点

### 审查方法论
- **静态分析优先**：代码审查 + 架构对比，减少运行时测试依赖
- **字节级一致性验证**：利用现有 c_dump 兼容性测试作为基线，扩展边界场景
- **威胁建模**：STRIDE 模型识别 DoS/资源耗尽/互通性破坏风险
- **差异分析矩阵**：逐模块对比 c_dump 与 secs_lib 的设计决策

### 文档输出规范
- **Markdown 格式**：使用标准 GitHub Flavored Markdown
- **代码引用**：使用 \`\`\`cpp 代码块 + 行号注释
- **风险等级标注**：🔴 Critical / 🟠 High / 🟡 Medium / 🟢 Low
- **可追溯性**：每个问题分配唯一 ID（如 AUDIT-HSMS-001）

### 工具链
- **覆盖率工具**：gcovr（支持分支覆盖 + HTML 报告）
- **构建系统**：CMake 3.20+，支持 -DSECS_ENABLE_COVERAGE=ON
- **测试框架**：自研轻量框架（test_main.hpp）
- **参考实现集成**：通过 extern "C" 链接 c_dump 的 .o 文件进行字节级对比

### 关键约束
- **不自动修复代码**：审查报告仅提供建议，所有代码修改需人工 review 后执行
- **保持向后兼容**：修复建议不得破坏现有 API（除非标注为 Breaking Change）
- **性能考量**：新增限制（如递归深度）需评估对合法用例的性能影响
- **可测试性**：所有建议的修复必须可通过自动化测试验证

### 审查深度参考
参照 MISRA C++、CERT C++ Coding Standard、CWE Top 25 的安全检查项，但聚焦于协议实现的领域特定风险（如 SEMI 标准合规性、字节序一致性、状态机正确性）。
