# Utils 模块详细实现原理

> 文档生成日期：2026-01-07
> 基于源码版本：当前 main 分支

## 1. 模块概述

`secs::utils` 是本库的调试工具集，目标是让你在联调/抓包/日志排查时，能快速把“十六进制字节流”转换为可读输出。

该模块 **不参与协议栈的核心收发路径**，主要用于：

- 从抓包/日志复制一段十六进制字符串 -> 解析为 bytes -> 输出字段解析与 hexdump
- 将 SECS-II Item 以可读格式输出（避免日志被二进制淹没）
- 在运行时对 `protocol::Session` 的收发报文做动态解析与打印（`SessionOptions::dump` 内部复用本模块的 dump）

```
┌─────────────────────────────────────────────────────────────────────┐
│                        secs::utils 模块                             │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │  Hex 工具：hex.hpp                                           │   │
│  │   - parse_hex(text -> bytes)                                 │   │
│  │   - hex_dump(bytes -> string)                                │   │
│  └─────────────────────────────────────────────────────────────┘   │
│                                                                     │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │  SECS-II Item dump：item_dump.hpp                            │   │
│  │   - dump_item(item -> string)                                │   │
│  └─────────────────────────────────────────────────────────────┘   │
│                                                                     │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │  HSMS dump：hsms_dump.hpp                                    │   │
│  │   - dump_hsms_frame(frame(with 4B len))                      │   │
│  │   - dump_hsms_payload(payload(10B header + body))            │   │
│  │   - 可选：将 data message body 解码为 SECS-II Item           │   │
│  └─────────────────────────────────────────────────────────────┘   │
│                                                                     │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │  SECS-I dump：secs1_dump.hpp                                 │   │
│  │   - dump_secs1_block_frame(frame(length+payload+checksum))   │   │
│  │   - dump_secs1_message(header+body，消息级别)                │   │
│  │   - Secs1MessageReassembler：多 block 重组                   │   │
│  │   - 可选：将 message body 解码为 SECS-II Item                │   │
│  └─────────────────────────────────────────────────────────────┘   │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
```

---

## 2. Hex 工具（hex.hpp）

### 2.1 parse_hex：把“字符串”转为 bytes

API：

- `std::error_code parse_hex(std::string_view text, std::vector<byte>& out) noexcept;`

输入格式特性：

- 支持大小写十六进制
- 支持多种分隔符：空白、逗号、冒号、连字符、下划线、方括号等
- 支持可选 `0x` / `0X` 前缀（会被忽略）

失败语义：

- 若出现非十六进制字符，或 nibble 个数为奇数（无法拼成完整 byte），返回 `core::errc::invalid_argument`

典型用途：

- 从 Wireshark/串口日志复制 `00 00 00 0A ...`，用于后续 HSMS/SECS-I dump

### 2.2 hex_dump：把 bytes 转为 hexdump 字符串

API：

- `std::string hex_dump(bytes_view bytes, HexDumpOptions options = {});`

常用选项：

- `bytes_per_line`：每行字节数（典型 16/32）
- `max_bytes`：输出截断上限（避免日志过长）
- `show_offset`：行首偏移（0000:）
- `show_ascii`：ASCII 侧栏（非可打印字符用 '.'）
- `enable_color`：ANSI 颜色（写日志/文件时建议关闭）

---

## 3. SECS-II Item dump（item_dump.hpp）

`dump_item()` 将 `secs::ii::Item` 输出为“可读字符串”，面向调试与日志：

- 输出格式尽量贴近 SECS 领域习惯，但 **不是严格 SML**
- 默认会做多种截断（payload / list 元素数 / 深度），避免巨量内容刷屏
- 支持 `multiline` 缩进格式

常见用途：

- 在 handler 里快速确认“解码出来的 Item 长什么样”
- 在协议层 dump 中把 `body`（on-wire bytes）解码成 Item 并打印

---

## 4. HSMS dump（hsms_dump.hpp）

### 4.1 输入形态

HSMS dump 支持两种输入：

- `dump_hsms_frame(frame)`：输入为完整 TCP 帧（含 4B Length）
- `dump_hsms_payload(payload)`：输入为 payload（10B Header + body，不含 4B Length）

输出要点：

- 解析并打印 header 字段：`session_id/p_type/s_type/system_bytes` 等
- 若为 data message，会额外输出 `SxFy` 与 `W-bit`
- 可选：对 data message body 执行 `secs::ii::decode_one()` 并用 `dump_item()` 输出

### 4.2 资源限制与截断

当开启 `enable_secs2_decode` 时：

- 通过 `secs::ii::DecodeLimits` 限制解码资源消耗（避免超大/恶意输入导致巨大分配）
- `HexDumpOptions::max_bytes` 与 `ItemDumpOptions::max_payload_bytes` 可进一步控制输出规模

---

## 5. SECS-I dump（secs1_dump.hpp）

### 5.1 dump_secs1_block_frame：block 级别

输入为完整 block frame（Length + Header + Data + Checksum），输出包含：

- SECS-I header 字段：`device_id/reverse_bit/stream/function/wait_bit/end_bit/block_number/system_bytes`
- Data 长度
- 可选 hexdump
- 可选（仅单 block 且 end_bit=1 且 block_number=1）：将 data 作为 SECS-II body 解码并输出 Item

### 5.2 dump_secs1_message：消息级别（header + body）

当你手里已经是“完整消息”（例如 SECS-I 多 block 重组之后），推荐使用消息级 dump：

- 输入：`secs1::Header + message body`
- 输出：消息头字段 + body 长度 + 可选 hexdump + 可选 SECS-II 解码

说明：

- 消息级 dump 不包含 ENQ/EOT/ACK/NAK 等链路控制字节

### 5.3 Secs1MessageReassembler：多 block 重组

当你从链路侧接收的是连续的 block frame，可使用重组器：

- `accept_frame(frame, message_ready)`
- 当 `message_ready=true` 时，通过 `message_header()/message_body()` 读取完整消息
- `dump_message()`：直接输出该条完整消息的 dump（内部会在 ready 时自动 reset）
- `decode_message_body_as_secs2()`：只做 SECS-II 解码，便于业务侧拿到 `ii::Item`

---

## 6. 推荐用法与注意事项

### 6.1 联调期：优先用 protocol 运行时 dump

当你的业务入口是 `secs::protocol::Session`，推荐使用：

- `SessionOptions::dump.enable = true`
- 可选设置 `dump_tx/dump_rx` 与 `sink`
- HSMS/SECS-I 的 dump 细节通过 `dump.hsms` / `dump.secs1` 调整

这样你无需在业务逻辑里手动插入“解析并打印”的代码。

### 6.2 输出规模控制

当 payload 较大时，建议同时设置：

- `dump.hsms.hex.max_bytes` / `dump.secs1.hex.max_bytes`：限制 hexdump
- `dump.hsms.item.max_payload_bytes` / `dump.secs1.item.max_payload_bytes`：限制 Item 输出
- `dump.hsms.secs2_limits` / `dump.secs1.secs2_limits`：限制解码资源

---

## 7. 参考实现与示例

源码入口：

- Hex：`include/secs/utils/hex.hpp`、`src/utils/hex.cpp`
- Item dump：`include/secs/utils/item_dump.hpp`、`src/utils/item_dump.cpp`
- HSMS dump：`include/secs/utils/hsms_dump.hpp`、`src/utils/hsms_dump.cpp`
- SECS-I dump：`include/secs/utils/secs1_dump.hpp`、`src/utils/secs1_dump.cpp`

可运行示例：

- `examples/utils_dump_example.cpp`

单元测试：

- `tests/test_utils_dump.cpp`
