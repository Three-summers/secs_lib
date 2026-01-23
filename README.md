# SECS Library（C++20）

[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)](https://en.cppreference.com/w/cpp/20)
[![CMake](https://img.shields.io/badge/CMake-3.20+-green.svg)](https://cmake.org/)

> 文档更新：2026-01-23（Codex）

`secs_lib` 是一个基于 C++20 与 standalone Asio 协程的 SECS 协议库，实现：

- **SECS-I（SEMI E4）**：串口半双工传输层（含 MemoryLink 便于 loopback/测试）
- **SECS-II（SEMI E5）**：消息内容层（`secs::ii::Item` + 编解码）
- **HSMS-SS（SEMI E37）**：TCP 全双工传输层

并在其上提供：

- `secs::protocol`：统一 HSMS/SECS-I 的 `send/request/run`、请求-响应匹配、消息路由
- `secs::sml`：SML 子集 + SMLX（占位符渲染、条件匹配、捕获、主动编码 body）
- `secs::c_api`：C ABI（不透明句柄 + 阻塞式门面 + 统一错误码/内存契约）

---

## 文档入口

- 总导航：`docs/index.md`
- C++ API 使用指南：`docs/user_guide/01-cpp-api.md`
- C API 使用指南：`docs/user_guide/02-c-api.md`
- 示例（可直接运行）：`examples/README.md`
- 架构与实现（按模块）：`docs/architecture/00-overview.md`
- 联调测试：`integration_tests/README.md`
- 性能基准：`benchmarks/README.md`

---

## 功能概览

- **分层模块**：`core/ii/hsms/secs1/protocol/sml/utils/c_api`（按需链接对应 CMake target）
- **统一会话**：`secs::protocol::Session` 统一 HSMS/SECS-I；`Router` 支持 “精确 / stream default / default” 三级匹配
- **强类型处理**：`protocol::TypedHandler` + `secs::ii::to_item/from_item<T>`（支持 `secs_members()` 声明式映射，减少样板代码）
- **CEID 简易分发**：`protocol::CeidDispatcher` 与 `utils::ceid_helpers`（不引入 GEM，仅做 body 解码 + CEID 提取 + 分发）
- **C API 易用层**：`secs_ii_builder`、`*_at_path/*_at_list_path` 提取、decoded handler、CEID dispatcher 等

---

## CMake Targets（按需链接）

| 模块 | CMake target | 主要用途 |
| --- | --- | --- |
| core | `secs::core` | 基础类型、错误码、事件、日志封装 |
| ii | `secs::ii` | SECS-II `Item` 与编解码 |
| hsms | `secs::hsms` | HSMS framing + 会话状态机（SELECT/LINKTEST 等） |
| secs1 | `secs::secs1` | SECS-I Link + 状态机（ENQ/EOT/ACK/NAK、分包/重组、超时） |
| protocol | `secs::protocol` | 统一会话、Router、SystemBytes、请求-响应匹配 |
| sml | `secs::sml` | SML 解析与运行时、SMLX 渲染/匹配 |
| utils | `secs::utils` | dump/hex/辅助工具（调试与示例常用） |
| c_api | `secs::c_api` | C ABI（供纯 C/FFI 调用） |

---

## 快速开始（构建/运行示例）

构建示例：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DSECS_BUILD_EXAMPLES=ON
cmake --build build --target examples -j
```

跑一组 loopback（不依赖 socket/串口）：

```bash
./build/examples/hsms_custom --role loopback
./build/examples/hsms_smlx --role loopback --sml examples/ceid_demo.sml
./build/examples/secs1_custom --role loopback --device-id 0x0001
./build/examples/secs1_smlx --role loopback --device-id 0x0001 --sml examples/ceid_demo.sml
./build/examples/c_api_hsms_custom --role loopback --session-id 0x0001
./build/examples/c_api_hsms_smlx --role loopback --session-id 0x0001 --sml examples/ceid_demo.sml
./build/examples/c_api_secs1_custom --role loopback --device-id 0x0001
./build/examples/c_api_secs1_smlx --role loopback --device-id 0x0001 --sml examples/ceid_demo.sml
```

更多 server/client/串口/虚拟串口用法见 `examples/README.md`。

---

## 构建与集成

### 依赖

- C++20 编译器：GCC ≥11 / Clang ≥14 / MSVC ≥19.30
- CMake ≥3.20
- standalone Asio（见下文获取策略）
- spdlog（仅库内部使用，不出现在 public headers；见下文获取策略）

#### Asio 获取策略

优先级：

1. vendored：`third_party/asio/`
2. 外部指定：`-DSECS_ASIO_ROOT=/path/to/asio/include`
3. 自动拉取：`-DSECS_FETCH_ASIO=ON`（需要网络；默认：顶层工程 ON，作为子项目 OFF）

#### spdlog 获取策略

优先级：

1. vendored：`third_party/spdlog/include/`
2. 外部指定：`-DSECS_SPDLOG_ROOT=/path/to/spdlog/include`
3. 系统路径：默认 include 路径下存在 `spdlog/spdlog.h`
4. 自动拉取：`-DSECS_FETCH_SPDLOG=ON`（需要网络；默认：顶层工程 ON，作为子项目 OFF）

### 作为子项目集成（推荐）

```cmake
add_subdirectory(path/to/secs_lib)

add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE secs::protocol)
```

纯 C 可执行程序也必须用 C++ 链接器（因为实现为 C++20）：

```cmake
add_executable(my_c_app main.c)
target_link_libraries(my_c_app PRIVATE secs::c_api)
set_target_properties(my_c_app PROPERTIES LINKER_LANGUAGE CXX)
```

### install/find_package（可选）

```bash
cmake -S . -B build -DSECS_ENABLE_INSTALL=ON
cmake --build build -j
cmake --install build --prefix /path/to/prefix
```

消费者：

```cmake
find_package(secs CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE secs::protocol)
```

### 常用 CMake 选项

- `SECS_ENABLE_TESTS`：单元测试（默认：顶层工程 ON，作为子项目 OFF）
- `SECS_ENABLE_INTEGRATION_TESTS`：联调测试（默认 OFF）
- `SECS_ENABLE_FUZZING`：fuzz targets（libFuzzer，可选；默认 OFF）
- `SECS_BUILD_EXAMPLES`：示例（默认：顶层工程 ON，作为子项目 OFF）
- `SECS_BUILD_BENCHMARKS`：性能基准（默认 OFF）
- `SECS_ENABLE_COVERAGE`：覆盖率辅助目标（默认 OFF）
- `SECS_ENABLE_WERROR`：警告视为错误（默认：顶层工程 ON，作为子项目 OFF）
- `SECS_ENABLE_INSTALL`：安装与 `find_package`（默认：顶层工程 ON，作为子项目 OFF）
- `SECS_STATIC_CPP_RUNTIME`：静态链接 C++ 运行库（交叉编译默认 ON）
- `SECS_FULLY_STATIC`：尽可能全静态（默认 OFF）

---

## 测试

单元测试：

```bash
cmake -S . -B build -DSECS_ENABLE_TESTS=ON -DSECS_BUILD_EXAMPLES=OFF
cmake --build build -j
ctest --test-dir build --output-on-failure
```

可选：只跑协议编解码确定性 fuzz/差分 + metrics hook 冒烟（仍走 ctest）：

```bash
ctest --test-dir build -R 'hsms_codec_fuzz|sml_fuzz|metrics_hook' --output-on-failure
```

可选：fuzz（libFuzzer）

说明：需要 Clang；构建时开启 sanitizers（ASan/UBSan）。

```bash
cmake -S . -B build_fuzz -DCMAKE_CXX_COMPILER=clang++ -DSECS_ENABLE_FUZZING=ON -DSECS_BUILD_EXAMPLES=OFF
cmake --build build_fuzz -j
./build_fuzz/fuzz/fuzz_ii_decode_one -runs=10000
./build_fuzz/fuzz/fuzz_hsms_decode_payload -runs=10000
./build_fuzz/fuzz/fuzz_sml_parse -runs=10000
```

联调测试说明与运行方式见 `integration_tests/README.md`。

---

## 标准对应

- SEMI E4: SECS-I Message Transfer (Serial)
- SEMI E5: SECS-II Message Content
- SEMI E37: HSMS - High-Speed SECS Message Services

---

## 致谢

- Asio：[chriskohlhoff/asio](https://github.com/chriskohlhoff/asio)
- spdlog：[gabime/spdlog](https://github.com/gabime/spdlog)
