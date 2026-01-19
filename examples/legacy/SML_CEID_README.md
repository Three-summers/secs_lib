# （Legacy）SML + CEID 示例说明

> 更新时间：2026-01-19  
> 状态：Legacy（本目录示例默认不再构建）

## 推荐替代（主示例集合）

当前仓库目录顶层的主示例已覆盖 “SMLX + Data Capture + 变量注入 + 自动回包” 的完整链路，且默认构建：

- C++：`examples/hsms_smlx.cpp`、`examples/secs1_smlx.cpp`
- C API：`examples/c_api_hsms_smlx.c`、`examples/c_api_secs1_smlx.c`
- SMLX 模板：`examples/ceid_demo.sml`

如果你只是想跑通与联调，请优先使用这些示例（见 `examples/README.md`）。

---

## 本文档定位

本文件用于解释 `examples/legacy/` 下的 “SML + CEID” 历史示例思路，便于你在阅读旧实现/旧接口时快速对照：

- SMLX 模板：定义请求/响应消息体结构与条件规则
- CEID：作为“业务分支键”（按厂商文档定义，通常位于 `S6F11` body 中）
- RenderContext：把动态数据注入到模板占位符

注意：本示例不引入 GEM（E30）语义，仅用于演示“按 CEID 分支的事件/请求-响应”类型消息。

---

## 当前实现中的 SMLX 写法（与 `ceid_demo.sml` 一致）

### 1) 响应模板（带占位符）

```sml
status_response: S6F12
<L
  <U2 DATAID>
  <U2 0x1001>
  <L
    <A DEVICE_NAME>
    <U1 STATUS_CODE>
    <U4 UPTIME_SECONDS>
  >
>.
```

### 2) 条件规则（自动选择响应 + 捕获变量）

这里使用 **Data Capture**（`$NAME`）在匹配时捕获请求中的字段（例如 `$DATAID`、`$PARAMS`）：

```sml
if (S6F11 <L [3] <U2 $DATAID> <U2 0x1001> <L $PARAMS>>) status_response.
```

匹配成功后：

- `match_response_with_capture(...)` 返回 `status_response`；
- 捕获到的 `$DATAID/$PARAMS` 会写入一个 `RenderContext`（变量名不包含 `$`）。

### 3) 运行时变量注入（示意）

```cpp
secs::sml::RenderContext ctx;

// 捕获变量：例如 ctx["DATAID"] 已存在（由 match_response_with_capture 写入）

// 业务注入：按 CEID/响应模板补齐其它占位符
ctx.set("DEVICE_NAME", secs::ii::Item::ascii("EQUIPMENT-001"));
ctx.set("STATUS_CODE", secs::ii::Item::u1({1}));
ctx.set("UPTIME_SECONDS", secs::ii::Item::u4({12345}));
```

### 4) 自动渲染并回包（关键流程）

典型 handler 流程（C++）：

1. `decode_one(body)` 解码请求 body -> `secs::ii::Item`
2. `match_response_with_capture(stream, function, item, captures)` 匹配并捕获
3. 在 `captures` 上继续注入业务变量
4. `encode_message_body(response_name, captures, out_body, &s, &f, &w)` 得到响应 body bytes
5. 返回 `HandlerResult{ec, body}`，由 `protocol::Session` 自动回 secondary

对应可运行参考：`examples/hsms_smlx.cpp` 与 `examples/secs1_smlx.cpp`。

---

## 本目录相关文件

- `examples/legacy/sml_ceid_complete.cpp`：历史 C++ 示例
- `examples/legacy/sml_ceid_complete.sml`：历史 SML 模板（与 `examples/ceid_demo.sml` 不同）
- `examples/legacy/c_api_sml_ceid_complete.c`：历史 C API 示例
- `examples/legacy/SML_CEID_README.md`：本文档

---

## 运行说明（如需）

本目录示例默认不再构建；如需运行，可以：

1. 参考 `examples/CMakeLists.txt` 的主示例写法，将某个 legacy 源文件临时加入 build 目标；
2. 或直接把 legacy 源码当作“可读参考”，迁移关键逻辑到主示例集合。

---

## 扩展阅读

- SML 模块实现：`docs/architecture/06-sml-module.md`
- SMLX 扩展提案：`docs/architecture/09-smlx-extension.md`
- CEID 分发（C++）：`include/secs/protocol/ceid_dispatcher.hpp`
- CEID 辅助（C++）：`include/secs/utils/ceid_helpers.hpp`
