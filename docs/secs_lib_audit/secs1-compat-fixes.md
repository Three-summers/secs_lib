# SECS-I c_dump 兼容性修复建议（task-3）

- 日期：2026-01-03
- 执行者：Codex（静态审查，仅输出分析报告）
- 约束遵循：不修改任何代码；不执行构建或测试
- 参考：`.claude/specs/secs_lib_audit/dev-plan.md`（Task 3）

## 1. 审查范围与证据来源

审查文件（按任务要求）：
- `src/secs1/block.cpp`：`encode_block` / `decode_block` / `checksum`
- `include/secs/secs1/block.hpp`：`Header` 布局说明与 API 契约
- `tests/test_c_dump_secs1_block_compat.cpp`：现有 c_dump 字节级对齐测试
- `tests/test_secs1_framing.cpp`：常规 SECS-I 编解码/分包/重组测试
- `c_dump/Secs_App/secs_I.c`：参考实现（`SecsI_ArrangeHead` / `SecsI_ArrangeChecksum` / `SecsI_BlockCheck`）
- `c_dump/Secs_App/secs_struct.h`：`SecsHead` 结构体字段定义

本报告所有“字节级预期”均以 c_dump 的以下实现为准：
- `SecsI_ArrangeHead(SecsHead*, uint8_t*)`
- `SecsI_ArrangeChecksum(uint8_t*)`

## 2. 关键结论（与审查重点逐项对应）

### 2.1 wait_bit=true 时 Reply_Bit 的移位逻辑

**结论**：编码层面（写入 Header Byte3）secs_lib 与 c_dump 当前实现是一致的，但存在“字段语义陷阱”，建议通过新增边界测试将其固化。

证据：
- secs_lib：`encode_block` 对 `wait_bit` 直接写入 bit7：`(wait_bit ? 0x80 : 0x00) | (stream & 0x7F)`。
- c_dump：`SecsI_ArrangeHead` 通过加法合成 Byte3：`Secs_SendBuf[3] = (ArrangeHead->Reply_Bit) + (ArrangeHead->Secs_Type.Stream_ID);`
  - 这要求 `ArrangeHead->Reply_Bit` 在调用前必须是 **“已移位值”**（0x80 或 0x00），否则若按 `SecsHead.Reply_Bit` 注释理解成 0/1，会导致 Byte3 错误。
- 现有兼容性测试 `tests/test_c_dump_secs1_block_compat.cpp` 已显式采用“已移位值”策略：`cd.Reply_Bit = hdr.wait_bit ? 0x80u : 0u;`

风险点（需测试覆盖）：
- `wait_bit=true, stream=0`（Byte3 应为 `0x80`，可直接验证“0x80 加法”语义）
- `wait_bit=true, stream=1`（Byte3 应为 `0x81`，避免只覆盖 stream=127）

### 2.2 device_id 边界值 0x0000 / 0x7FFF 编码

**结论**：在 device_id 合法域（0x0000–0x7FFF）内，secs_lib 与 c_dump 的字节布局一致；建议补齐 `0x0000` 与 `0x7FFF + reverse_bit=false` 的用例。

证据：
- c_dump：`Secs_SendBuf[1] = (Reverse_Bit<<7) + (uint8_t)(Device_ID >> 8); Secs_SendBuf[2] = (Device_ID & 0xff);`
- secs_lib：`Byte1 = (reverse_bit?0x80:0) | ((device_id>>8)&0x7F); Byte2 = device_id&0xFF;`

补充说明：
- secs_lib 对 `device_id > 0x7FFF` 会返回 `invalid_argument`（显式阻断“Reverse_Bit 与 DeviceID 最高位冲突”的不可判定编码）。
- c_dump 没有显式校验，若传入 `device_id >= 0x8000`，将产生与 `Reverse_Bit` 冲突的 Byte1（潜在协议歧义）。这属于“输入域约束差异”，建议以 secs_lib 的校验为准并在文档/API 契约中明确。

### 2.3 block_number=0 的合法性与一致性

**结论**：在 block_number=0 且 block_number 高 7 位为 0 的情况下，secs_lib 与 c_dump 编码一致；建议补齐 end_bit=true/false 两种组合用例。

证据：
- secs_lib：`Byte5 = (end_bit ? 0x80 : 0) | ((block_number>>8)&0x7F)`，`Byte6 = block_number & 0xFF`
- c_dump：`Secs_SendBuf[5] = (End_Bit << 7); Secs_SendBuf[6] = Block_num;`（仅写入低 8 位）

额外风险（见第 3 节差异矩阵）：
- c_dump 对 `block_number >= 256` 的高位编码缺失，会导致 Byte5[6:0] 始终为 0；与 secs_lib 的 15 位 block_number 语义不一致。

### 2.4 checksum 计算字节范围

**结论**：secs_lib 与 c_dump 的 checksum 计算范围一致，均为 **Header+Data（即 length 字节之后的 payload）**，不包含 length 字节本身，也不包含 checksum 字段。

证据：
- c_dump：`SecsI_ArrangeChecksum` / `SecsI_BlockCheck` 均为 `for array_cnt in [0..Block_Length-1] sum += buf[array_cnt + 1]`，即从 `buf[1]` 累加 `Block_Length` 个字节（payload）。
- secs_lib：`encode_block` 使用 `checksum(bytes_view{out.data()+1, length})`，`decode_block` 对 `payload=frame.subspan(1,length)` 求和比对。

备注（与任务描述的文字差异）：
- dev-plan / 任务描述中的“包含 length byte + header + data”更像是口径表述差异；以现有代码证据来看，**length 只作为“循环上界/长度字段”，并未纳入求和**。

## 3. 已识别差异矩阵（字节位置 + 根因 + 修复建议）

> 本节用于满足“若发现差异，文档化差异字节位置 + 根因 + 修复建议”的验收标准。

### 3.1 差异：block_number 高位（BlockNumber[14:8]）在 c_dump 中未编码

**复现条件**：`block_number >= 0x0100`（示例：0x0123），其高位应体现在 Header Byte5 的低 7 bit。

**字节级差异（示例输入）**：
- Header：`reverse_bit=false, device_id=0x0001, wait_bit=false, stream=0x01, function=0x01, end_bit=true, block_number=0x0123, system_bytes=0`
- Data：空

**差异位置**：
- `Header Byte5`（E + BlockNumber[14:8]）：
  - secs_lib 编码：`0x81`（E=1 + 高位=0x01）
  - c_dump 编码：`0x80`（仅 E=1，高位恒为 0）
- `Checksum` 会随 Header Byte5 的差异同步偏移（本例差值为 0x0001）。

**根因定位**：
- c_dump：`SecsI_ArrangeHead` 固定 `Secs_SendBuf[5] = (End_Bit << 7);`，未将 `Block_num` 的高位合入 Byte5。
- secs_lib：按 `include/secs/secs1/block.hpp` 的 15 位布局实现 `BlockNumber[14:8]` 写入 Byte5[6:0]。

**修复建议（按优先级）**：
1. **若目标是“对齐 c_dump（生产参考）”**：将 SECS-I 的 block_number 视为 8 位（仅 Byte6），并将 Header Byte5[6:0] 视为保留位固定为 0。
   - 影响：需要调整 `Header::block_number` 的语义/注释，以及 `encode_block/decode_block` 对 block_number 高位的处理。
   - 验证：新增一个“block_number=0x0123”用例，明确期望 secs_lib 与 c_dump 一致（Byte5[6:0]==0），并观察现有 `test_secs1_framing.cpp` 中 “block_number 高位路径” 相关用例是否需要同步调整（这可能是 Breaking Change）。
2. **若目标是“坚持当前 15 位语义，但保证与 c_dump 互通”**：在与 c_dump 风格对齐的发送路径上强制 `block_number <= 0x00FF`（或直接拒绝/掩码高位），并在文档中明确该互通约束。
   - 影响：协议能力受限，但与 c_dump 字节级对齐更直接；同时可避免隐蔽的“高位被对端丢弃”导致重组异常。

## 4. 建议追加的边界场景兼容性测试（至少 6 个）

> 建议追加到：`tests/test_c_dump_secs1_block_compat.cpp`  
> 形式：沿用现有 `expect_same_block(hdr, data)` 结构；每个用例用 c_dump 生成 expected，再对比 C++ 实现 actual。  
> 注意：按现有测试约定，c_dump 侧应继续使用 `Reply_Bit = wait_bit ? 0x80 : 0x00`（“已移位值”）。

### Case A：device_id=0x0000 + block_number=0（end_bit=true）

- 目的：覆盖 device_id 最小值与 block_number=0 的组合（协议允许但罕见）
- Header：
  - reverse_bit=false, device_id=0x0000
  - wait_bit=false, stream=0x01, function=0x01
  - end_bit=true, block_number=0x0000
  - system_bytes=0x00000000
- Data：空
- 期望 frame（hex）：
  - `0A 00 00 01 01 80 00 00 00 00 00 00 82`
  - Checksum：`0x0082`

### Case B：reverse_bit=true（device_id=0x0000）+ data=1B

- 目的：覆盖 Reverse_Bit 在 device_id 最小边界下的编码；同时验证 checksum 覆盖 data
- Header：
  - reverse_bit=true, device_id=0x0000
  - wait_bit=false, stream=0x01, function=0x02
  - end_bit=true, block_number=0x0001
  - system_bytes=0x00000000
- Data：`[00]`
- 期望 frame（hex）：
  - `0B 80 00 01 02 80 01 00 00 00 00 00 01 04`
  - Checksum：`0x0104`

### Case C：device_id=0x7FFF（reverse_bit=false）+ system_bytes=0xFFFFFFFF

- 目的：补齐 device_id 最大值在 reverse_bit=false 情况下的编码；覆盖 system_bytes 全 1
- Header：
  - reverse_bit=false, device_id=0x7FFF
  - wait_bit=false, stream=0x7F, function=0x00
  - end_bit=true, block_number=0x00FF
  - system_bytes=0xFFFFFFFF
- Data：空
- 期望 frame（hex）：
  - `0A 7F FF 7F 00 80 FF FF FF FF FF 07 78`
  - Checksum：`0x0778`

### Case D：wait_bit=true + stream=0（Reply_Bit“已移位值”语义固化）

- 目的：覆盖 wait_bit=true 的最小 stream，确保 c_dump 的 `Reply_Bit(0x80)+Stream(0)` 与 C++ 的位或规则一致
- Header：
  - reverse_bit=false, device_id=0x1234
  - wait_bit=true, stream=0x00, function=0x02
  - end_bit=true, block_number=0x0001
  - system_bytes=0x01020304
- Data：`[AB]`
- 期望 frame（hex）：
  - `0B 12 34 80 02 80 01 01 02 03 04 AB 01 FE`
  - Checksum：`0x01FE`

### Case E：wait_bit=true + reverse_bit=true + end_bit=false + block_number=0

- 目的：覆盖多标志位同时出现时的字节合成；覆盖 end_bit=false 与 block_number=0 组合
- Header：
  - reverse_bit=true, device_id=0x1234
  - wait_bit=true, stream=0x01, function=0x02
  - end_bit=false, block_number=0x0000
  - system_bytes=0xAABBCCDD
- Data：`[55 AA]`
- 期望 frame（hex）：
  - `0C 92 34 81 02 00 00 AA BB CC DD 55 AA 05 56`
  - Checksum：`0x0556`

### Case F：最大 payload（data=243B，frame=256B）

- 目的：覆盖 SECS-I 单帧最大长度（Len=253, frame size=256），同时固化 checksum 范围为 payload（不包含 Len 字节）
- Header：
  - reverse_bit=false, device_id=0x0001
  - wait_bit=false, stream=0x01, function=0x01
  - end_bit=true, block_number=0x0001
  - system_bytes=0x00000000
- Data：243B，定义为 `data[i] = i (0..242)`
- 期望：
  - `Len = 0xFD`（253）
  - Header(10B) = `00 01 01 01 80 01 00 00 00 00`
  - Data 前 8B = `00 01 02 03 04 05 06 07`
  - Data 末 8B = `EB EC ED EE EF F0 F1 F2`
  - Checksum = `0x735F`

## 5. 建议的“差异定位用例”（可选，不计入 6 个一致性用例）

> 该用例用于直观呈现第 3.1 节的差异：当 block_number 含高位时，c_dump 与 secs_lib 的 Header Byte5 低 7 bit 不一致。  
> 是否将其纳入自动化测试，取决于最终决定的“兼容策略”（对齐 c_dump 还是坚持 15 位语义）。

- Header：`reverse_bit=false, device_id=0x0001, wait_bit=false, stream=0x01, function=0x01, end_bit=true, block_number=0x0123, system_bytes=0`
- Data：空
- 观察点：
  - secs_lib：`Byte5 = 0x81`，checksum `0x00A7`
  - c_dump：`Byte5 = 0x80`，checksum `0x00A6`

