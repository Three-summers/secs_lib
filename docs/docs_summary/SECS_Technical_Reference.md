# SECS 技术参考（面向 secs_lib 集成）

> 更新时间：2026-01-19  
> 维护者：Codex  
> 说明：本文件用于“标准概念 ↔ 本仓库实现/文档”的快速索引，不替代 SEMI 原文标准。

## 推荐阅读入口

- 文档总入口：`../index.md`
- README（构建/集成/快速跑示例）：`../../README.md`
- 示例：`../../examples/README.md`
- 架构总览：`../architecture/00-overview.md`
- C++ API 使用指南：`../user_guide/01-cpp-api.md`
- C API 使用指南：`../user_guide/02-c-api.md`

## 标准与实现对照

### SECS-II（SEMI E5）

- 标准要点：`SEMI_E05_0200A_要点总结.md`
- 实现说明：`../architecture/02-secs-ii-module.md`
- 关键代码：`../../include/secs/ii/`、`../../src/ii/`
- 关键测试：`../../tests/test_secs2_codec.cpp`、`../../tests/test_struct_codec.cpp`

### HSMS-SS（SEMI E37）

- 标准要点：`SEMI_E37_总结.md`
- 实现说明：`../architecture/03-hsms-module.md`
- 关键代码：`../../include/secs/hsms/`、`../../src/hsms/`
- 关键测试：`../../tests/test_hsms_transport.cpp`
- 跨实现联调：`../../integration_tests/README.md`

### SECS-I（SEMI E4）

- 标准要点：`SECS-I标准要点总结.md`
- 实现说明：`../architecture/04-secs1-module.md`
- 关键代码：`../../include/secs/secs1/`、`../../src/secs1/`
- 关键测试：`../../tests/test_secs1_framing.cpp`

## 协议层（本仓库的统一抽象）

`secs::protocol` 不是标准的一部分，而是本仓库在 HSMS/SECS-I 之上提供的统一抽象层：

- 实现说明：`../architecture/05-protocol-module.md`
- 关键类型：`Router` / `Session` / `SystemBytes` / `TypedHandler`
- 推荐示例：`../../examples/hsms_custom.cpp`、`../../examples/secs1_custom.cpp`

## SML / SMLX（规则驱动）

- 实现说明：`../architecture/06-sml-module.md`
- 扩展提案：`../architecture/09-smlx-extension.md`
- 推荐示例：`../../examples/hsms_smlx.cpp`、`../../examples/secs1_smlx.cpp`、`../../examples/ceid_demo.sml`

## C API（C ABI）

- 使用指南：`../user_guide/02-c-api.md`
- 实现说明：`../architecture/07-c-api-module.md`
- 头文件：`../../include/secs/c_api.h`
- 推荐示例：`../../examples/c_api_*.c`

## GEM（SEMI E30）说明

本仓库不实现完整 GEM（E30）模型。当前仅提供一些“贴近厂商文档常见布局”的轻量辅助层，例如：

- `protocol::CeidDispatcher` / C API 的 `secs_ceid_dispatcher_*`：按消息体内 CEID 分发（不引入 GEM 语义）
