# C API 使用指南（面向库使用者 / 纯 C 工程）

> 文档更新：2026-01-14（Codex）  
> 基于源码版本：当前工作区  
> 目标读者：希望用纯 C 调用 `secs_lib`（或从其他语言走 C ABI/FFI）的工程师

本指南只讲“怎么用”。C API 的接口契约以 `include/secs/c_api.h` 为准；更完整的“可执行文档”请直接阅读 `tests/test_c_api.c` 与 `examples/*.c`。

---

## 0. 你应该选哪条使用路径？

```
┌──────────────────────────────────────────────────────────────────────────────┐
│                                   你的 C 代码                                  │
│                            #include <secs/c_api.h>                             │
└──────────────────────────────────────────────────────────────────────────────┘
                 │
                 │ ① 只想构造/解析 SECS-II payload（不做通信）
                 ▼
        ┌───────────────────┐
        │ SECS-II Item/codec │  secs_ii_item_* / secs_ii_encode / secs_ii_decode_one
        └───────────────────┘

                 │
                 │ ② 想直接做 HSMS（TCP）收发（你自己拼 request/response 逻辑）
                 ▼
        ┌───────────────────┐
        │ HSMS Session       │  secs_hsms_session_*（阻塞式）
        └───────────────────┘

                 │
                 │ ③ 想要“统一 send/request + handler + 自动回包”（推荐给业务）
                 ▼
┌──────────────────────────────────────────────────────────────────────────────┐
│ Protocol Session                                                             │
│ - secs_protocol_session_*                                                    │
│ - handler 注册：set_handler / set_default_handler                             │
│ - 自动回 secondary：F+1 / W=0 / system_bytes 回显（仅 W=1 且 handler 返回 OK） │
│ - 可选：直接挂载 SML 规则引擎做 default handler（自动条件回包）                │
└──────────────────────────────────────────────────────────────────────────────┘
```

推荐：如果你要写的是“对端行为”（Equipment/Host/仿真器），优先使用 `secs_protocol_session_*`。

---

## 0.1 集成与链接提示（CMake / C 工程）

关键点：

- 头文件：`#include <secs/c_api.h>`
- 实现是 C++20：即使你的源文件是 C，也需要用 C++ 链接器链接（CMake 里链接到 `secs::c_api` 通常会自动处理）

作为子项目集成的最小示例：

```cmake
add_subdirectory(path/to/secs_lib)
add_executable(my_c_app main.c)
target_link_libraries(my_c_app PRIVATE secs::c_api)
```

本仓库示例构建（用于“先跑起来再改造”）：

```bash
cmake -S . -B build -DSECS_BUILD_EXAMPLES=ON
cmake --build build --target examples -j
```

---

## 1. C API 的 4 条硬约定（必须遵守）

### 1.1 句柄模型（opaque handles）

所有对象都是不透明句柄，必须用对应的 `*_destroy()` 释放：

- `secs_context_t`：运行 io 线程的上下文
- `secs_ii_item_t`：SECS-II Item
- `secs_sml_runtime_t`：SML runtime
- `secs_hsms_session_t` / `secs_hsms_connection_t`
- `secs_protocol_session_t`

### 1.2 错误模型：`secs_error_t { value, category }`

- `value==0` 表示成功（`secs_error_is_ok(err)`）
- `category` 是静态字符串（例如 `"secs.c_api"` / `"secs.ii"` / `"sml.parser"`）
- 可用 `secs_error_message(err)` 生成可读文本（返回的字符串要 `secs_free()`）

### 1.3 内存模型：库分配的都用 `secs_free()`

```
库返回给你的堆内存（示例）：
  - secs_error_message() 的字符串
  - secs_ii_encode() 的 out_bytes
  - secs_sml_runtime_match_response() 的 out_name
  - secs_sml_runtime_get_message_body_by_name() 的 out_body_bytes
  - handler 回调里你分配的 out_body（库复制后会 secs_free）

统一释放函数：
  secs_free(ptr)
```

### 1.4 线程模型：必须创建 context；阻塞式 API 不能在 io 线程调用

`secs_context_create()` 内部会启动 io 线程并运行 `asio::io_context`。多数网络/会话 API 是“阻塞式桥接”：

```
你的线程（调用方）             库内部 io 线程
┌───────────────────┐          ┌───────────────────────────┐
│ secs_*_open_*()    │  调度    │ 执行协程/网络读写/状态机    │
│ secs_*_request()   │ ───────▶ │ …                         │
│   阻塞等待结果      │ ◀─────── │ 返回结果并唤醒调用方        │
└───────────────────┘          └───────────────────────────┘

handler 回调：在“库内部 io 线程”触发
```

为了防止死锁：如果你在 handler 回调（io 线程）里调用阻塞式 API，会返回：

- `SECS_C_API_WRONG_THREAD`（category=`"secs.c_api"`）

---

## 2. 快速开始：Protocol Session + handler（推荐入口）

对应可运行示例：

- `examples/c_api_protocol_server.c`、`examples/c_api_protocol_client.c`（TCP）
- `examples/c_api_protocol_loopback.c`（memory duplex，无 socket 环境也可运行）

### 2.1 最小调用顺序（概念图）

```
1) 创建上下文（启动 io 线程）
   secs_context_create(&ctx)

2) 创建 HSMS 会话并连接（或注入 memory duplex）
   secs_hsms_session_create(ctx, &opt, &hsms)
   secs_hsms_session_open_active_ip(hsms, ip, port)    // client
   secs_hsms_session_open_passive_ip(hsms, ip, port)   // server

3) 创建协议层会话（推荐）
   secs_protocol_session_create_from_hsms(ctx, hsms, session_id, &proto_opt, &proto)

4) 注册 handler（只需要关心“收到什么 -> 回什么 body”）
   secs_protocol_session_set_handler(proto, stream, function, cb, user_data)
   或 secs_protocol_session_set_default_handler(proto, cb, user_data)

5) 主动发消息
   secs_protocol_session_send(proto, stream, function, body, body_n)      // W=0
   secs_protocol_session_request(proto, stream, function, body, body_n, timeout_ms, &reply) // W=1

6) 释放资源
   secs_protocol_session_stop(proto); secs_protocol_session_destroy(proto);
   secs_hsms_session_stop(hsms);     secs_hsms_session_destroy(hsms);
   secs_context_destroy(ctx);
```

### 2.2 handler 回调语义（自动回包的关键）

回调签名（见 `include/secs/c_api.h`）：

```c
typedef secs_error_t (*secs_protocol_handler_fn)(
    void *user_data,
    const secs_data_message_view_t *request,
    uint8_t **out_body,
    size_t *out_body_n);
```

规则：

- 回调在库内部 io 线程触发
- 如果 `request->w_bit==1` 且回调返回 `OK`，库会自动发送 secondary：
  - `stream = request->stream`
  - `function = request->function + 1`
  - `w_bit = 0`
  - `system_bytes` 回显
- 如果回调返回非 OK：视为“未处理”，库不会回包
- `*out_body` 必须使用 `secs_malloc()` 分配（库复制后会 `secs_free()`）

**常见建议**：

- 对 `w_bit==0` 的请求：直接返回 OK 且 `out_body=NULL/out_body_n=0`（表示“我处理了但无需回包”）
- 如果你的逻辑“只想处理部分 SxFy”：对不处理的分支返回非 OK（让库不回包）

### 2.3 Router 匹配顺序（精确 / stream-default / default）

Protocol Session 的 handler 匹配顺序与 C++ 端一致：

1) 精确匹配 `(stream,function)`
2) stream default（同一 stream 的兜底：`SxF*`）
3) default handler（全局兜底）
4) 都没有：视为未处理，不回包

对应 C API：

- 精确：`secs_protocol_session_set_handler()` / `secs_protocol_session_erase_handler()`
- stream default：`secs_protocol_session_set_stream_default_handler()` / `secs_protocol_session_clear_stream_default_handler()`
- default：`secs_protocol_session_set_default_handler()` / `secs_protocol_session_clear_default_handler()`

如果你想把 SML 自动回包只挂在某一个 stream 上，可用：

- `secs_protocol_session_set_sml_stream_default_handler(proto, stream, rt)`

### 2.4 decoded handler（自动 decode/encode，贴近 C++ TypedHandler）

如果你希望业务回调直接拿到“解码后的 SECS-II Item”，减少样板代码，可用：

- `secs_protocol_session_set_decoded_handler()`（精确）
- `secs_protocol_session_set_decoded_stream_default_handler()`（SxF* 兜底）
- `secs_protocol_session_set_decoded_default_handler()`（全局兜底）

decoded handler 回调语义要点：

- 框架先把 `request.body` 解码为 `decoded_body` 再调用回调
- 回调返回 OK 表示“已处理”；非 OK 表示“拒绝处理”（库不回包）
- 回调若返回 `out_item_body`，框架会负责 `encode + destroy`（避免泄漏）
- `decoded_body` 仅在回调期间有效；如需跨回调保存，调用 `secs_ii_item_clone()`

---

## 3. SECS-II：Item 构造 / 编码 / 解码（C 侧最常用）

对应示例：`examples/c_api_secs2_simple.c`

常见模式：

```
构造：
  secs_ii_item_create_list / create_ascii / create_ascii_cstr / create_u2 / ...

克隆：
  secs_ii_item_clone(src, &dst)

拼 List：
  secs_ii_item_list_append(list, child)
  注意：append 会拷贝 child，因此你可以立刻 destroy(child)

编码：
  secs_ii_encode(item, &bytes, &n)      // bytes 用 secs_free 释放

解码：
  secs_ii_decode_one(bytes, n, &consumed, &out_item)
  out_item 用 secs_ii_item_destroy 释放
```

解码资源限制（不可信输入建议开启）：

- `secs_ii_decode_limits_init_default(&limits)`
- `secs_ii_decode_one_with_limits(in, in_n, &limits, &consumed, &out_item)`

提取便捷 API（避免 C varargs 误用）：

- varargs 版本：`secs_ii_item_get_u2_at_path(root, &out, depth, (size_t)0, ...)`
- list-path（array）版本：`secs_ii_item_get_u2_at_list_path(root, &out, indices, indices_n)`

---

## 4. SML：读取 + 自动回复（最省 glue 的做法）

### 4.1 你有两种用法

```
A) 你自己调用 SML runtime（match + get body），并写进你的 handler

B) 直接把 SML runtime 挂到 protocol session 作为 default handler（推荐）
   secs_protocol_session_set_sml_default_handler(proto, rt)
```

### 4.1.1 最小 SML 片段（理解语义用）

```sml
s1f1: S1F1 W <L>.
s1f2: S1F2 <L <A \"OK\">>.
if (s1f1) s1f2.
every 10 send s1f1.
```

### 4.1.2 从文件读取并加载

`secs_sml_runtime_load(rt, source, source_n)` 的输入是“文本内容”，因此你需要自己读文件：

```c
// 伪代码：read_all_text() 由你自己实现（fopen/fread）
char* text = read_all_text(\"docs/sml_sample/sample.sml\", &n);
secs_sml_runtime_load(rt, text, n);
free(text); // 注意：这里是你自己分配的内存，用你自己的 free
```

如果你手里已经是 NUL 结尾的字符串，也可以用：

- `secs_sml_runtime_load_cstr(rt, text)`

### 4.2 推荐：一行挂载 SML 自动回包

相关 API（见 `include/secs/c_api.h`）：

- `secs_sml_runtime_create/load`
- `secs_protocol_session_set_sml_default_handler(proto, rt)`

行为（库内已经封装）：

- 仅对入站 primary 且 `W=1` 的消息尝试匹配
- 命中 `if (...)` 规则后，取到响应模板并编码为 body
- 自动回 secondary：`F+1 / W=0 / SB 回显`
- 未命中 / 模板不存在 / 模板 (S,F,W) 与期望不一致 / 解码失败：视为未处理，不回包

生命周期提示：

- `secs_protocol_session_set_sml_default_handler()` 内部会拷贝 `rt` 内容  
  因此挂载成功后你可以立即 `secs_sml_runtime_destroy(rt)`（见 `tests/test_c_api.c` 的注释用例）

### 4.3 重要限制：C API 不暴露 SMLX 的变量注入（占位符模板无法直接渲染）

当前 C API 的 `secs_sml_runtime_get_message_body_by_name()` **没有 RenderContext**，
因此：

- 如果模板里含占位符（例如 `<A MDLN>`），取模板 body 会返回：
  - `category="sml.render"`、`value=1 (missing_variable)`

这不是 bug，而是能力边界：C API 只能直接使用“纯常量模板”，或由你在 C 侧自己生成 body。

（该边界有单测覆盖：`tests/test_c_api.c` 里 `test_sml_runtime_placeholders()`）

---

## 5. 受限环境（禁 socket）也能跑：memory duplex

如果你的运行环境禁止 `socket()`（沙箱/容器/某些测试环境），可用内存互联：

- `secs_hsms_connection_create_memory_duplex(ctx, &client_conn, &server_conn)`
- client：`secs_hsms_session_open_active_connection(client_hsms, &client_conn)`
- server：`secs_hsms_session_open_passive_connection(server_hsms, &server_conn)`

完整可运行示例：

- `examples/c_api_protocol_loopback.c`

---

## 6. 调试：log / dump / 错误字符串

### 6.1 日志级别

- `secs_log_set_level(SECS_LOG_DEBUG)`（示例里常用）

### 6.2 protocol 层 runtime dump（v2 options）

如果你需要像 C++ 一样在 protocol 层 dump TX/RX，可使用 v2：

- `secs_protocol_session_create_from_hsms_v2()`
- `secs_protocol_session_options_v2_t::dump_flags` + `dump_sink`

可用 flag（节选，见 `include/secs/c_api.h`）：

- `SECS_PROTOCOL_DUMP_ENABLE`
- `SECS_PROTOCOL_DUMP_TX` / `SECS_PROTOCOL_DUMP_RX`
- `SECS_PROTOCOL_DUMP_COLOR`
- `SECS_PROTOCOL_DUMP_SECS2_DECODE`

### 6.3 错误字符串

```c
secs_error_t err = ...;
if (!secs_error_is_ok(err)) {
  char* msg = secs_error_message(err);
  fprintf(stderr, "err: category=%s value=%d msg=%s\n", err.category, err.value, msg);
  secs_free(msg);
}
```

---

## 7. 进一步阅读（最像“说明书”的代码）

1) 示例（可运行）：

- `examples/README.md`
- `examples/c_api_secs2_simple.c`
- `examples/c_api_hsms_server.c` / `examples/c_api_hsms_client.c`
- `examples/c_api_protocol_server.c` / `examples/c_api_protocol_client.c`
- `examples/c_api_protocol_loopback.c`

2) 接口契约与线程/内存约定：

- `include/secs/c_api.h`

3) 单元测试（最完整的边界行为说明）：

- `tests/test_c_api.c`
