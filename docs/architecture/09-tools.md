# 工具链（CLI Tools）与编译期资源嵌入

> 文档更新：2026-01-23（Codex）  
> 对应实现：`main`（CMake：`project(secs VERSION 0.1.0)`）

本库除了协议栈模块外，还提供了一组面向工程效率的工具：

- **SML 检查器**：`secs-sml-check`（语法/引用/类型一致性）
- **HSMS 录制与回放**：`secs-recorder` / `secs-player`（JSONL）
- **编译期资源嵌入**：`secs_embed_text_as_c()`（把 `.sml` 等文本编译进可执行文件）

这些工具的目标是让你在联调、抓包回放、以及“部署环境不能持久化文件（重启清空）”的场景下，仍然能保持良好的开发体验。

---

## 1. 工具总览

```
┌──────────────────────────────────────────────────────────────────────┐
│                              Tooling                                 │
├──────────────────────────────────────────────────────────────────────┤
│  secs-sml-check  : 静态检查 SML 语法/语义（无需运行程序）            │
│  secs-recorder   : HSMS 透明代理 + 录制 DataMessage 为 JSONL         │
│  secs-player     : 读取 JSONL，连接 HSMS 并按策略回放/校验           │
│  embed_text_as_c : 构建期把文本文件转成 C 头文件（字符串常量）       │
└──────────────────────────────────────────────────────────────────────┘
```

构建目标（默认顶层工程开启）：

- `secs-sml-check`、`secs-recorder`、`secs-player`：见 `tools/CMakeLists.txt`
- 资源嵌入函数：`cmake/EmbedTextAsC.cmake` + `tools/embed_text_as_c.py`

---

## 2. secs-sml-check：SML 静态检查器

### 2.1 目的

在不编译业务程序、不运行设备联机的情况下，对 SML 做“开发期反馈”：

- 语法错误（lexer/parser）
- `if (...) xxx.` 中引用的消息名是否存在（undefined ref）
- 模板变量在不同位置的类型是否一致（type mismatch）

### 2.2 使用方式

```
secs-sml-check [options] <files...>

options:
  --verbose            输出 AST（文本）
  --stats              输出 messages/conditions/timers 统计
  --format <text|json> 输出格式（默认 text）
```

### 2.3 输出与集成

- `--format json` 适合接入 CI 或 IDE tooling。
- text 输出会按行列号打印 caret，便于定位。

相关实现：`tools/secs_sml_check.cpp` + `include/secs/tools/sml_check.hpp`。

---

## 3. secs-recorder / secs-player：录制与回放（JSONL）

### 3.1 设计目标

- **录制**：把 HSMS DataMessage（S/F/W/SystemBytes/body）写入 JSON Lines 文件（每行一条）
- **回放**：按“原速/快进/单步”读取 JSONL，并向目标 HSMS endpoint 发送或校验

典型用途：

- 线上抓一段联机流量（或者测试环境抓包），开发机本地复现。
- 对设备/Host 行为做可重复的回归测试（尤其是对时序敏感的交互）。

### 3.2 文件格式（RecordedMessage）

每行 JSON（必需字段）：

- `ts_us`：相对时间（microseconds, steady_clock 单调）
- `dir`：`TX` / `RX`（相对录制点）
- `s` / `f` / `w`：Stream/Function/W-bit
- `sb`：System Bytes
- `body_hex`：body 的 hex 字符串（不含 HSMS header）

可选字段：

- `sml`：面向人类阅读的 `SxFy[ W].` 文本（不参与回放必需字段）

相关实现：

- `include/secs/tools/recording.hpp`
- `src/tools/recording.cpp`

### 3.3 与 protocol::Session 的集成（Tap vs Dump）

建议用 `protocol::SessionOptions::tap` 做录制集成：

- dump：输出“格式化字符串”（面向日志排障）
- tap：输出“结构化 DataMessage”（面向录制/统计/指标）

录制器 `MessageRecorder::record_tx/record_rx` 为 `noexcept`，适合直接挂到 tap 回调。

---

## 4. 编译期资源嵌入：secs_embed_text_as_c()

### 4.1 场景与动机

在一些嵌入式/开发板部署环境中：

- 程序启动时会被拷贝到临时目录；
- 设备重启后该目录被清空，无法依赖外部 SML 文件持久化；
- 但开发阶段仍希望用“可编辑的 `.sml` 文件”维护脚本，而不是手写 C 字符串转义。

因此提供“构建期把 `.sml` 变成 C 常量”的方案：

- 开发时：编辑 `xxx.sml`
- 编译时：自动生成 `xxx_sml.h`
- 运行时：直接用 `const char*` 传入 `Runtime::load(...)` / C API load

### 4.2 生成内容

生成的头文件默认提供三个符号（以 `VAR` 为前缀）：

- `VAR_bytes[]`：`static const unsigned char[]`
- `VAR_size`：`static const size_t`
- `VAR_cstr`：`static const char*`（指向 `VAR_bytes`）

当启用 `NULL_TERMINATE` 时，生成器会追加 `\0`，确保 `VAR_cstr` 可当 C 字符串使用。

### 4.3 CMake 接口

本库提供的函数：

```cmake
secs_embed_text_as_c(
  TARGET <target>
  INPUT  <path/to/file>
  OUTPUT <binary_dir/.../generated_xxx.h>
  VAR    <c_identifier>
  NULL_TERMINATE
)
```

注意：

- 需要 `Python3`（仅解释器即可）。若未找到 Python3，本功能不会启用。
- `OUTPUT` 会被加入到 `TARGET` 的 sources，并自动把生成目录加入 include dirs。

### 4.4 最小使用示例

```c
#include "generated/micro_pressure_sml.h"

// micro_pressure_sml_cstr: const char*
// micro_pressure_sml_size: size_t
```

示例文件：

- 输入：`examples/micro_pressure.sml`
- 输出：`build/examples/generated/micro_pressure_sml.h`

实现位置：

- `tools/embed_text_as_c.py`
- `cmake/EmbedTextAsC.cmake`

---

## 5. 工程化建议

### 5.1 CI 建议

- 对所有 SML 文件跑一遍：`secs-sml-check --format json --stats ...`
- 对录制文件（如纳入回归测试资产）可使用 `secs-player` 做冒烟回放

### 5.2 嵌入式部署建议

当设备端文件系统不可靠（重启清空/只读/权限受限）时，优先选：

- **编译期嵌入**（`secs_embed_text_as_c`）作为默认 SML 来源
- SD 卡/外部存储作为“可选覆盖配置”（如果你的应用允许：先尝试读外部文件，失败则回退到内置字符串）
