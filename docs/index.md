# 文档导航

> 更新时间：2026-01-23（Codex）  
> 维护者：Codex  
> 适用版本：`main`（CMake：`project(secs VERSION 0.1.0)`）

本页是 `secs_lib` 的文档入口，按“上手 → 集成 → 设计细节”组织。

## 快速上手

- 示例（推荐从这里开始）：[examples/README.md](../examples/README.md)
- C++ API 使用指南：[user_guide/01-cpp-api.md](user_guide/01-cpp-api.md)
- C API 使用指南：[user_guide/02-c-api.md](user_guide/02-c-api.md)

## 架构与模块设计

- 架构总览（推荐入口）：[architecture/00-overview.md](architecture/00-overview.md)
- Core：[architecture/01-core-module.md](architecture/01-core-module.md)
- SECS-II：[architecture/02-secs-ii-module.md](architecture/02-secs-ii-module.md)
- HSMS：[architecture/03-hsms-module.md](architecture/03-hsms-module.md)
- SECS-I：[architecture/04-secs1-module.md](architecture/04-secs1-module.md)
- Protocol：[architecture/05-protocol-module.md](architecture/05-protocol-module.md)
- SML/SMLX：[architecture/06-sml-module.md](architecture/06-sml-module.md)
- C API：[architecture/07-c-api-module.md](architecture/07-c-api-module.md)
- Utils：[architecture/08-utils-module.md](architecture/08-utils-module.md)
- 工具链（CLI + 资源嵌入）：[architecture/09-tools.md](architecture/09-tools.md)

## SML/SMLX（扩展与提案）

- SMLX 扩展提案：[architecture/09-smlx-extension.md](architecture/09-smlx-extension.md)
- SML 可用性 Roadmap：[architecture/10-sml-usability-roadmap.md](architecture/10-sml-usability-roadmap.md)

## C API 易用性（计划与落地情况）

- C API 易用性改进计划：[architecture/11-c-api-improvement-plan.md](architecture/11-c-api-improvement-plan.md)

## 工程集成

- 纯 C 工程集成（tvoc_code 类项目）：[integration/tvoc_code-cmake-integration.md](integration/tvoc_code-cmake-integration.md)
- 联调测试说明：[integration_tests/README.md](../integration_tests/README.md)

## 标准要点与对照（内部整理）

- 技术参考入口：[docs_summary/SECS_Technical_Reference.md](docs_summary/SECS_Technical_Reference.md)
- SECS-I 要点：[docs_summary/SECS-I标准要点总结.md](docs_summary/SECS-I标准要点总结.md)
- SEMI E5 要点：[docs_summary/SEMI_E05_0200A_要点总结.md](docs_summary/SEMI_E05_0200A_要点总结.md)
- SEMI E37 要点：[docs_summary/SEMI_E37_总结.md](docs_summary/SEMI_E37_总结.md)

## 性能与验证

- 单元测试：`ctest --test-dir build --output-on-failure`
- 协议编解码确定性 fuzz/差分：`ctest --test-dir build -R 'hsms_codec_fuzz|sml_fuzz' --output-on-failure`
- 可观测性（metrics hook）：见 `architecture/01-core-module.md`（Metrics 小节）
- 可选：libFuzzer targets：见 `../README.md` 的 “可选：fuzz（libFuzzer）”
- 性能基准：[benchmarks/README.md](../benchmarks/README.md)
