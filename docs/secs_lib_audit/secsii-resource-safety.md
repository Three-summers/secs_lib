# SECS-II 解码资源安全加固审查报告（资源消耗）

- 日期：2026-01-03
- 执行者：Codex（静态审查）
- 参考：`.claude/specs/secs_lib_audit/dev-plan.md`（Task 2）
- 范围：
  - `src/ii/codec.cpp`（解码主逻辑）
  - `src/ii/item.cpp`（Item 构造）
  - `include/secs/ii/codec.hpp`（解码接口）
  - `include/secs/ii/item.hpp`（Item 结构定义）
  - `tests/test_secs2_codec.cpp`（现有测试覆盖分析）
- 非目标：本报告不修改任何代码、不执行构建或测试，仅给出风险评估与建议。

## 结论摘要（TL;DR）

1. **递归深度限制存在**：`src/ii/codec.cpp` 定义 `kMaxDecodeDepth=64` 并在 `decode_item()` 开头校验（`depth > kMaxDecodeDepth` 即拒绝）。因此 **256 层嵌套 List 会被显式拒绝**，栈深度被固定在约 65 层调用以内。
2. **超大 length（0xFFFFFF=16MB）对 ASCII/Binary 会“整块分配+拷贝”**：当负载真实存在时，解码会分配约 16MB 内存并复制 payload；缺乏“总预算/速率”限制，反复消息或组合消息容易触发内存与 CPU 压力。
3. [Critical] **List 的 length=元素个数，但被允许到 0xFFFFFF（≈1677 万）且会 `reserve(length)`**：仅凭 4 字节头部即可触发超大预分配；在 `noexcept` 语境下，若触发 `std::bad_alloc` 很可能导致进程直接终止（DoS）。此外，即使不崩溃，宽 List 也会造成 O(n) 解析时间与巨大常驻内存。
4. 对齐 c_dump 的 `max_nested_level=16`：在本仓库的 `c_dump/Secs_App/secs_II.c` 中未检索到名为 `max_nested_level` 的显式限制；但若外部参考实现确实以 16 为默认上限，建议将 secs_lib 的深度限制“默认值/配置项”与之对齐，以降低互通与安全风险。

## 现状梳理：解码路径与关键约束

### 解码入口与数据结构

- 解码 API：`secs::ii::decode_one(bytes_view in, Item &out, std::size_t &consumed) noexcept`
  - 声明：`include/secs/ii/codec.hpp`
  - 实现：`src/ii/codec.cpp`（`decode_one()` 调 `decode_item(r, out, 0)`）
- `Item` 是强类型 variant（`include/secs/ii/item.hpp`），List 为 `std::vector<Item>`，ASCII 为 `std::string`，Binary 为 `std::vector<byte>`。

### 长度上限（kMaxLength）

- `include/secs/ii/types.hpp:41`：`kMaxLength = 0x00FF'FFFF`（3 字节 length 字段最大值，≈16MB）
- `src/ii/codec.cpp:718-720`：解码时若 `length > kMaxLength` 则返回 `errc::length_overflow`。
  - **注意：对 List 来说 length 表示“子元素数量”，不是字节数**（`src/ii/codec.cpp:728-744`）。因此当前实现等价于允许 **单个 List 最多含 1677 万个子 Item**。

### 递归深度上限（kMaxDecodeDepth）

- `src/ii/codec.cpp:103`：`constexpr std::size_t kMaxDecodeDepth = 64;`
- `src/ii/codec.cpp:695-700`：`if (depth > kMaxDecodeDepth) return errc::invalid_header;`
  - 根调用 `depth=0`，因此最多允许 `depth=64` 进入解析；第 65 层（`depth=65`）会被拒绝。

## 审查重点逐项回答

### 1) 当前递归深度限制是否存在？最坏栈消耗如何？

**结论：存在。** `kMaxDecodeDepth=64` 且在读取任何字节前就校验（`src/ii/codec.cpp:695-700`）。

- **当前实现的最坏栈深度**：最多约 65 层 `decode_item()` 调用（depth=0..64）。
  - `decode_item()` 栈帧包含若干小对象（`std::error_code`、`std::optional`、`bytes_view` 等）以及在 List 分支内的 `List items`（vector 对象本身在栈上，容量在堆上）。
  - 由于标准库对象大小与 ABI 相关，本报告无法给出“精确字节数”，但经验上 65 层递归的栈占用通常在 **数 KB~数十 KB** 量级，明显低于常见线程栈（例如 8MB）。

**若无深度限制的理论最坏情况（用于风险对比）**：

- 可用最小字节构造极深嵌套：每层 List 只需 2 字节头（例如 `0x01 0x01`），因此深度可随输入线性增长；当深度达到数万级时，栈溢出风险显著（取决于平台默认栈大小与编译器栈帧布局）。

### 2) ASCII/Binary 声明 length=0xFFFFFF（16MB）时的行为？

**结论：会接受（只要输入足够长），并进行一次性内存分配与拷贝。**

- ASCII：`src/ii/codec.cpp:753-758` 使用 `std::string(ptr, payload.size())`，会分配约 `length` 字节并复制。
- Binary：`src/ii/codec.cpp:759-763` 使用 `std::vector<byte>(payload.begin(), payload.end())`，会分配约 `length` 字节并复制。

**对“截断负载”的行为（正向点）**：

- `SpanReader::read_payload()` 会先检查 `remaining() < n`，不足则返回 `errc::truncated`（`src/ii/codec.cpp:173-180`）。因此当攻击者只发送“超大声明”但不给够负载时，ASCII/Binary 分支不会提前分配超大缓冲区。
- 这一点在测试中已有覆盖：
  - `tests/test_secs2_codec.cpp` 的 `test_decode_large_ascii_length_truncated()`（声明 1MB 但只给 100B）
  - `tests/test_secs2_codec.cpp` 的 `test_decode_large_binary_length_truncated()`（声明 0xFFFFFF 但只给 1B）

**仍然存在的资源风险**：当负载真实存在时，16MB 的拷贝与分配是确定发生的；若上层允许并发/频繁解码，将导致内存与 CPU 峰值非常高。

### 3) 嵌套 256 层 List 的内存分配总量？（应 <64MB 或显式拒绝）

**结论：会被显式拒绝。** 测试用例已验证 256 层会返回 `errc::invalid_header`：

- `tests/test_secs2_codec.cpp`：`test_decode_deep_list_nesting_rejected()` 构造 256 层嵌套并期望 `invalid_header`。
- 触发点：第 65 层调用 `decode_item(depth=65)` 时直接返回错误（`src/ii/codec.cpp:695-700`），在读取更深字节前就拒绝。

**对内存的粗略估算**（以测试的构造方式：每层 List `length=1`）：

- 每层会创建一个 `List items; items.reserve(1);`（`src/ii/codec.cpp:730-732`）。
- 由于只 reserve 1 个元素，单层堆分配大约是 `sizeof(Item)`（外加 allocator 元数据）。
- 在拒绝发生前最多经历约 64 层 reserve，因此临时堆分配约为 `64 * sizeof(Item)`，通常远低于 64MB（一般是 KB 级）。

**但要注意**：深度限制并不等价于“总内存 < 64MB”。宽 List / 大 payload 的组合依然可轻易超过 64MB（见下文攻击向量 #1/#3/#4）。

### 4) 与 c_dump 的 `max_nested_level=16` 对比：是否需要对齐？

本仓库内可检索到的 c_dump 相关实现位于 `c_dump/Secs_App/`：

- `c_dump/Secs_App/secs_II.h` 定义了常见格式字节常量（如 `SECS_LIST=0x01`、`SECS_BINARY=0x21`、`SECS_ASCII=0x41`）。
- `c_dump/Secs_App/secs_II.c` 的 `Secs_MessageArrange()` 是从文本（`"<L[...]"` 等）生成字节流的编码逻辑，并未找到名为 `max_nested_level` 的显式限制。

因此：

- 若 “`max_nested_level=16`” 来自你们内部/外部的另一版 c_dump 或生产约束，建议在 secs_lib 中至少提供 **可配置的 decode 深度上限**，并考虑将默认值下调至 16 以对齐（减少互通差异与攻击面）。
- 若无互通压力，64 的栈风险并不突出，但从“最小权限/最小资源暴露”角度，下调仍然更稳妥。

## 资源耗尽攻击向量（≥3）与 PoC

> 说明：以下 PoC 以“字节流（hex）”形式描述，均针对 `decode_one()` 输入。示例中的 payload 大段重复部分用“重复 N 次”表示，避免在文档中放入巨量数据。

### [Critical] 向量 1：超大 List 元素个数触发 `reserve(length)` 预分配（仅 4 字节头即可）

- 触发代码：`src/ii/codec.cpp:728-743`，关键点是 `items.reserve(length);`（`src/ii/codec.cpp:731`）。
- 根因：对 List 的 `length`（元素个数）使用 `kMaxLength=0xFFFFFF` 作为上限；并在验证子项存在前就按该数量 reserve。
- 影响：
  - 内存：尝试一次性分配 `length * sizeof(Item)` 的数组（加上 allocator 开销）。
  - 稳定性：`decode_item()` 为 `noexcept`，若 `reserve` 触发 `std::bad_alloc`，通常会导致进程 `std::terminate`（等价于 DoS）。

**PoC（最小字节流：只有头部、无子项）**

```text
03 FF FF FF
```

解释：

- `0x03`：List，lenBytes=3（`format_code::list=0x00`，因此 `(0x00<<2)|3 = 0x03`）
- `0xFF 0xFF 0xFF`：length = 0xFFFFFF（16777215 个子元素）
- 后续不提供任何子项，解析第一个子项时会因截断失败，但 **reserve 已经发生**。

**最坏情况资源消耗（内存）**

- 仅 List 容器预分配的理论值：
  - `M_list ≈ length * sizeof(Item)`
  - `length = 16,777,215`
  - 若 `sizeof(Item)=40B`，则 `M_list≈640MB`
  - 若 `sizeof(Item)=48B`，则 `M_list≈768MB`
  - 若 `sizeof(Item)=64B`，则 `M_list≈1024MB`
- 这还不包含后续子 Item 的 payload 分配。

**建议限流策略（常量 + 校验位置）**

- 新增：`kMaxDecodeListItems`（例如对齐 255/1024/4096 或按 64MB 预算推导）
- 校验位置：在 `decode_item()` 识别为 List 后、执行 `reserve` 前（`src/ii/codec.cpp` List 分支的 `items.reserve(length)` 之前）
- 额外建议：对宽 List 做最小输入长度预检查（`remaining < length*2` 则直接 `truncated`），并避免对不可信输入做超大 `reserve`。

### [Critical] 向量 2：宽 List + 最小子元素导致 O(n) CPU 消耗与巨大常驻内存

与向量 1 不同点：向量 2 假设攻击者提供“足够字节”让解析继续，从而制造 CPU 与内存双重压力。

**PoC（结构化描述）**

```text
03 FF FF FF                       ; List(lenBytes=3), count=0xFFFFFF
(21 00) repeated 0xFFFFFF times   ; 每个子元素为 Binary(lenBytes=1, length=0)
```

解释：

- `21 00` 是最小子元素之一：Binary，lenBytes=1，length=0（空 Binary）。
- 该 payload 的总输入大小约为：
  - `4 + 2 * 0xFFFFFF ≈ 32MB`

**最坏情况资源消耗**

- CPU：循环 16,777,215 次调用 `decode_item()`（`src/ii/codec.cpp:732-741`），即使每个子项为空，也会产生显著 CPU 时间。
- 内存：List 容器持有 16,777,215 个 `Item`，内存量级同向量 1（数百 MB ~ 1GB）。

**建议限流策略（常量 + 校验位置）**

- `kMaxDecodeListItems`：强制上限（例如使最坏情况内存 <64MB：`max_items <= floor(64MB / sizeof(Item))`）
- `kMaxDecodeTotalItems`：对整个树的节点总数做预算（防止“多个 List 叠加”）
- 校验位置：
  - 在 List 分支进入循环前预先检查 `length`（`src/ii/codec.cpp:728-733`）
  - 每 push 一个 child 之前/之后更新全局计数（需要给 `decode_item` 额外传参或上下文对象）

### [Critical] 向量 3：ASCII/Binary length=0xFFFFFF 的整块分配+拷贝（单项 16MB）

- 触发代码：
  - ASCII：`src/ii/codec.cpp:753-758`
  - Binary：`src/ii/codec.cpp:759-763`
- 根因：对非 List 的 payload length 允许到 `kMaxLength=0xFFFFFF`，解码后必然生成拥有独立存储的 `std::string/std::vector`（拷贝输入）。

**PoC（ASCII 最大长度）**

```text
43 FF FF FF                       ; ASCII(lenBytes=3), length=0xFFFFFF
41 repeated 0xFFFFFF times        ; payload='A' * 16,777,215
```

**PoC（Binary 最大长度）**

```text
23 FF FF FF                       ; Binary(lenBytes=3), length=0xFFFFFF
00 repeated 0xFFFFFF times        ; payload=0x00 * 16,777,215
```

**最坏情况资源消耗**

- 内存（单项）：约 16MB（ASCII 可能额外带 1B 结尾零，视实现而定）
- CPU：拷贝 16MB 字节（O(length)）
- 稳定性：若系统内存紧张，分配失败可能触发异常；在 `noexcept` 路径中通常会终止进程。

**建议限流策略（常量 + 校验位置）**

- 新增：`kMaxDecodePayloadBytes`（建议明显小于 16MB，例如 1MB/4MB，或与 HSMS/SECS-I 上层消息大小限制对齐）
- 校验位置：在 `read_payload(length, payload)` 之前或之后均可；更建议在读取 payload 之前即拒绝（`src/ii/codec.cpp:746-750` 附近）。
- 若业务必须支持大 payload：建议提供“零拷贝/流式”接口（例如 decode 输出 `bytes_view/string_view`），但这会影响 `Item` 的生命周期模型，属于接口层变更，需要整体设计评审。

### [High] 向量 4：组合型内存爆破：List 中包含多个接近上限的 Binary/ASCII（总量轻易 >64MB）

该向量更贴近“总内存应 <64MB”的目标：即使单个 payload 16MB 不算离谱，**缺乏总预算**会使组合消息直接越过 64MB。

**PoC（示例：List[5] + 5 个 16MB Binary）**

```text
01 05                            ; List(lenBytes=1), count=5
(23 FF FF FF + <16MB payload>) x5
```

**最坏情况资源消耗**

- 仅 payload 常驻内存就约：`5 * 16MB = 80MB`（不含 List/Item 结构与 allocator 开销）
- 若嵌入更复杂结构（List 包 List），总量进一步上升。

**建议限流策略（常量 + 校验位置）**

- 新增：`kMaxDecodeTotalBytes`（例如 64MB）
- 校验位置：需要在 `decode_item()` 递归过程中累计“预计分配量/实际分配量”，在创建 string/vector 或者进入子项循环前扣减预算。

## 当前实现的最坏情况资源消耗（汇总）

> 由于未执行构建/运行，`sizeof(Item)` 等 ABI 相关数据无法精确测量；下列为“基于源码逻辑 + 常见 64 位标准库实现”的量级估算，并给出公式以便落地时做精确计算。

### 栈深度（Stack）

- 上限：约 65 层 `decode_item()` 调用（由 `kMaxDecodeDepth=64` 决定）。
- 量级：通常为 **数 KB ~ 数十 KB**（远小于常见线程栈）。

### 堆内存（Heap）

1. **单个最大 ASCII/Binary**：约 16MB（外加少量对象/allocator 元数据）
2. **宽 List 预分配**：`length * sizeof(Item)`
   - 当 `length=0xFFFFFF` 时，量级为 **数百 MB ~ 1GB**
3. **组合型**：`sum(payload_bytes) + sum(list_vector_cap * sizeof(Item))`
   - 在缺乏 `kMaxDecodeTotalBytes` 的情况下，上限只受输入大小与系统内存约束。

## 测试覆盖现状（与资源安全相关）

`tests/test_secs2_codec.cpp` 已覆盖：

- 深度拒绝：`test_decode_deep_list_nesting_rejected()`（256 层）
- 深度边界：`test_decode_depth_limit_boundary()`（深度=64 可通过）
- 超大 length 截断：
  - ASCII：声明 1MB 但负载不足 -> `truncated`
  - Binary：声明 0xFFFFFF 但负载不足 -> `truncated`

尚未覆盖（合理，因为会消耗大量资源）：

- List `length` 超大导致 `reserve` 的资源消耗/崩溃路径（向量 1）
- 真实 16MB payload 的成功解码路径（向量 3）
- “总预算 >64MB”的组合型场景（向量 4）

## 建议的限流策略（落地指引：常量 + 校验位置）

> 目标：在不引入复杂组件的前提下，为不可信输入提供“硬上限 + 总预算”两道闸，满足：
> - 单条消息/单个 Item 的资源占用可预测
> - 256 层嵌套明确拒绝（已满足）
> - 组合情况下总分配 <64MB（当前未满足）

### 建议新增常量（示例值需结合业务与互通要求确定）

- `kMaxDecodeDepth`：建议默认 **16**（若需对齐外部实现），或保持 64 但提供可配置项。
- `kMaxDecodeListItems`：List 元素个数上限（建议远小于 0xFFFFFF）。
  - 若目标是“仅 List 容器 <64MB”，可用 `floor(64MB / sizeof(Item))` 推导。
- `kMaxDecodePayloadBytes`：ASCII/Binary 等 payload 字节上限（建议 1MB/4MB 级）。
- `kMaxDecodeTotalBytes`：整个 `Item` 树的“总分配预算”（建议 64MB）。
- `kMaxDecodeTotalItems`：整个树的节点总数预算（用于抑制宽 List/多 List 叠加的 CPU 与内存）。

### 建议校验逻辑插入点（源码位置）

- `src/ii/codec.cpp:695-700`：深度校验已存在。
- `src/ii/codec.cpp:713-720`：读取 length 后统一做“上限/预算”判定：
  - 若 `fmt==list`：在 `items.reserve(length)`（`src/ii/codec.cpp:731`）之前检查 `length <= kMaxDecodeListItems`，并做最小输入长度预检查（`remaining >= length*2`）。
  - 若 `fmt!=list`：在 `read_payload(length, payload)`（`src/ii/codec.cpp:746-750`）前检查 `length <= kMaxDecodePayloadBytes`，并扣减 `kMaxDecodeTotalBytes` 预算。
- `src/ii/codec.cpp:732-741`：List 循环中维护 `kMaxDecodeTotalItems`（每解析一个 child 递增）。

---

如需我基于本报告再输出“问题清单（带唯一 ID）+ 修复优先级 + 预估改动点清单”，我可以在不改代码的前提下补一份更贴近修复落地的路线图。
