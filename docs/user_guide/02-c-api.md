# C API 使用指南

> 文档更新：2026-01-23（Codex）  
> 目标读者：纯 C 工程或使用 FFI 调用的开发者

C API 同样遵循双轨制设计：**编程模式** (Code-First) 和 **SMLX 模式** (Rule-Based)。合理选择能极大降低 C 语言开发的复杂度。

## 推荐示例（可直接运行）

本仓库 `examples/` 提供了一套“功能对应”的 C / C++ 示例（HSMS + SECS-I；custom + SMLX；均含 loopback）：

- C++：`examples/hsms_custom.cpp`、`examples/hsms_smlx.cpp`、`examples/secs1_custom.cpp`、`examples/secs1_smlx.cpp`
- C：`examples/c_api_hsms_custom.c`、`examples/c_api_hsms_smlx.c`、`examples/c_api_secs1_custom.c`、`examples/c_api_secs1_smlx.c`

SECS-I（半双工）在 C API 侧的关键点：

- client/host：直接调用 `secs_protocol_session_request()`（内部会驱动收发）
- server/equipment：主循环调用 `secs_protocol_session_poll_once()` 驱动入站处理与回包

---

## 上下文与线程模型（阻塞式 API）

C API 的“阻塞式”网络/会话 API 依赖 `secs_context_t`（内部运行 `asio::io_context`）：

- 默认启动 **1 个 io 线程**；可通过 `secs_context_create_with_options()` 配置 `io_threads`。
- 阻塞式 API 禁止在 io 线程调用，否则会死锁；库会返回 `SECS_C_API_WRONG_THREAD`。

```c
secs_context_t *ctx = NULL;

secs_context_options_t opt;
secs_context_options_init_default(&opt);
opt.io_threads = 2; /* 可选：>1 表示多 io 线程 */

secs_error_t err = secs_context_create_with_options(&ctx, &opt);
if (!secs_error_is_ok(err)) {
    /* TODO: 处理错误 */
}

/* ... 使用 ctx ... */

secs_context_destroy(ctx);
```

## 可观测性（metrics hook）

库内关键路径（SECS-II 编解码、HSMS frame、protocol request、C API run_blocking、SML parse 等）
提供轻量指标 hook，可对接 Prometheus/OpenTelemetry/自研 metrics。

- `secs_metrics_set_hook()` 是**进程级全局**设置（默认 no-op）。
- 回调可能在多个线程/协程中触发：必须线程安全、尽量不阻塞、不抛异常。

```c
static void on_counter(void *ud, const char *name, uint64_t delta) {
    (void)ud;
    (void)name;
    (void)delta;
    /* TODO: 计数器累加 */
}

static void on_histogram(void *ud, const char *name, uint64_t value) {
    (void)ud;
    (void)name;
    (void)value;
    /* TODO: 直方图/分位数采样 */
}

void enable_metrics(void) {
    secs_metrics_hook_t hook;
    hook.counter = &on_counter;
    hook.gauge = NULL;
    hook.histogram = &on_histogram;
    hook.user_data = NULL;
    secs_metrics_set_hook(&hook);
}

void disable_metrics(void) { secs_metrics_set_hook(NULL); }
```

指标名与约定见 `docs/architecture/01-core-module.md`（Metrics 小节）。

---

## 路线一：编程模式 (Code-First)

这是基础模式。你需要手动处理请求并构造响应。为了减轻内存管理负担，本库引入了 **Builder** 模式。

### 1. 构建响应消息 (Builder 模式)

`secs_ii_builder` 是一个栈式构建器，用于安全地创建复杂的嵌套 List。

**目标**：构建 `<L <A "MDLN"> <L <U4 1001> <U4 1002>> >`

```c
#include <secs/c_api.h>

// 1. 创建构建器
secs_ii_builder_t *b = NULL;
secs_ii_builder_create(&b);

// 2. 开始构建 (注意缩进表示层级)
secs_ii_builder_list_begin(b);                 // <L
    secs_ii_builder_add_ascii(b, "MDLN");      //   <A ...>
    
    secs_ii_builder_list_begin(b);             //   <L
        secs_ii_builder_add_u4(b, 1001);       //     <U4 ...>
        secs_ii_builder_add_u4(b, 1002);       //     <U4 ...>
    secs_ii_builder_list_end(b);               //   >
secs_ii_builder_list_end(b);                   // >

// 3. 终结并生成 Item
secs_ii_item_t *root = NULL;
// 这一步会检查所有中间步骤是否出错 (Error Sinking)
secs_error_t err = secs_ii_builder_finalize(b, &root);

// 4. 销毁构建器 (root 依然有效)
secs_ii_builder_destroy(b);

if (!secs_error_is_ok(err)) {
    // 处理错误 (如内存不足)
}
```

**关键特性：错误吸收 (Error Sinking)**
你不需要在每行 `add_*` 后检查错误。如果中间某一步失败（如 OOM），后续操作会静默失败，直到 `finalize` 时统一返回错误码。

### 2. 解析请求消息 (Path Access)

不要一层层剥洋葱。使用 `_at_path` 系列 API 直接提取深层数据。

**场景**：从 `<L <A CMD> <L <U4 PAR1> <U4 PAR2>>>` 中提取 PAR2。

```c
// 假设 root 是收到的 Item
uint32_t par2 = 0;

// 路径: [1] (内层List) -> [1] (PAR2)
// 参数说明: root, out_val, depth, index0, index1...
// 注意: 变长参数中的索引必须强转为 (size_t)
secs_error_t err = secs_ii_item_get_u4_at_path(root, &par2, 2, (size_t)1, (size_t)1);

if (secs_error_is_ok(err)) {
    printf("Got PAR2: %u\n", par2);
}
```

### 3. 编写 Handler

```c
secs_error_t my_handler(void *user_data,
                        const secs_data_message_view_t *req,
                        uint8_t **out_body,
                        size_t *out_body_n) {
    // 1. 解析请求 (略) 
    
    // 2. 构造响应 (使用 Builder)
    secs_ii_builder_t *b = NULL; 
    secs_ii_builder_create(&b);
    secs_ii_builder_list_begin(b);
    secs_ii_builder_add_ascii(b, "ACK");
    secs_ii_builder_list_end(b);
    
    secs_ii_item_t *rsp_item = NULL;
    secs_ii_builder_finalize(b, &rsp_item);
    secs_ii_builder_destroy(b); // 记得销毁 builder
    
    // 3. 编码响应
    // - secs_ii_encode 会用 secs_malloc 分配 out_body；
    // - 框架会在复制后调用 secs_free 释放 out_body（回调里不要释放）。
    secs_ii_encode(rsp_item, out_body, out_body_n);
    
    // 4. 销毁中间 Item
    secs_ii_item_destroy(rsp_item);
    
    return (secs_error_t){0, NULL}; // OK
}
```

### 4. Decoded Handler（推荐：自动 decode/encode）

当你希望像 C++ 的 `TypedHandler` 一样“业务逻辑只处理 Item”，可以使用 decoded handler：

- 框架先把 `request.body` 解码为 `decoded_body` 再回调；
- 你只需要构造 `out_item_body`（一个 `secs_ii_item_t*`）；
- 框架会负责 `encode + destroy(out_item_body)`。

对应 API：`secs_protocol_session_set_decoded_handler()` / `secs_protocol_session_set_decoded_stream_default_handler()` / `secs_protocol_session_set_decoded_default_handler()`。

> 注意：`decoded_body` 仅在回调期间有效；如需跨回调保存，请调用 `secs_ii_item_clone()`。

```c
static secs_error_t on_decoded(void* user,
                              const secs_data_message_view_t* req,
                              const secs_ii_item_t* decoded_body,
                              secs_ii_item_t** out_item_body) {
    (void)user;
    (void)req;
    (void)decoded_body;

    // 业务：构造一个要回复的 Item（框架会负责 encode + destroy）
    return secs_ii_item_create_list(out_item_body);
}

// 注册（示例：精确匹配 S6F11）
secs_protocol_session_set_decoded_handler(proto, 6, 11, on_decoded, NULL);
```

### 5. CEID dispatcher（按 CEID 分发，不引入 GEM）

对于 `S6F11` 这类“body 中包含 CEID”的消息，可以用 C API 的 CEID dispatcher 收敛 decode + CEID 提取 + 分发：

- 创建：`secs_ceid_dispatcher_create_list_path()`（用 list path 指定 CEID 位置）
- 挂载：`secs_protocol_session_set_ceid_dispatcher(sess, stream, function, disp)`

典型布局（S6F11-like）：`<L <DATAID> <CEID> <...>>`，CEID 位于索引 1，因此 `indices={1}`。

```c
static secs_error_t on_ceid(void* user,
                           uint32_t ceid,
                           const secs_data_message_view_t* req,
                           uint8_t** out_body,
                           size_t* out_body_n) {
    (void)user;
    (void)req;
    // TODO: 按 ceid 构造不同回复（out_body 必须由 secs_malloc 分配，框架会负责释放）
    // 这里示意：回一个空 body 的 secondary
    (void)ceid;
    *out_body = NULL;
    *out_body_n = 0;
    return (secs_error_t){0, NULL}; // OK
}

size_t ceid_indices[] = {(size_t)1};
secs_ceid_dispatcher_t* disp = NULL;
secs_ceid_dispatcher_create_list_path(ceid_indices, 1, NULL, 1, &disp);
secs_ceid_dispatcher_set_handler(disp, 0x1001, on_ceid, NULL);
secs_protocol_session_set_ceid_dispatcher(proto, 6, 11, disp);
```

---

## 路线二：SMLX 模式 (Rule-Based)

对于 C 语言用户，这是**极力推荐**的模式。用 SML 文件代替繁琐的 C 代码来定义消息和回复规则。

### 1. 加载 SML

```c
secs_sml_runtime_t *rt;
secs_sml_runtime_create(&rt);

// 可以从文件读，也可以直接加载字符串
const char* sml_src = 
    "s1f1: S1F1 W <L>."
    "s1f2: S1F2 <L <A MDLN> >."
    "if (s1f1) s1f2.";
    
secs_sml_runtime_load_cstr(rt, sml_src);
```

### 2. 挂载自动回复

一行代码即可实现基础的 S1F1 -> S1F2 自动应答。

```c
// 设为 Default Handler
secs_protocol_session_set_sml_default_handler(proto, rt);

// 销毁 rt (Session 内部已拷贝)
secs_sml_runtime_destroy(rt);
```

### 3. SMLX 变量注入 (RenderContext)

如果 SML 模板中包含变量（如 `MDLN`），你需要创建一个 Context 并注入值，这在 C API 中也是支持的；ASCII 也支持在字符串内使用 `${MDLN}` 插值。

**注意：** 目前 `set_sml_default_handler` 使用的是无状态自动回复。如果你需要注入变量，建议使用 `secs_sml_runtime_match_response` 手动匹配并渲染。

**手动处理流程 (支持变量注入):**

```c
// 1. 在 Handler 中匹配规则
char* rsp_name = NULL;
// 假设已从 req 解码出 item
secs_sml_runtime_match_response(rt, req->stream, req->function, body_bytes, body_n, &rsp_name);

if (rsp_name) {
    // 2. 准备变量
    secs_sml_render_context_t *ctx;
    secs_sml_render_context_create(&ctx);
    secs_sml_render_context_set_ascii(ctx, "MDLN", "MyDevice");
    
    // 3. 渲染消息体
    uint8_t *rsp_body = NULL;
    size_t rsp_len = 0;
    // ... 其他出参 ...
    secs_sml_runtime_encode_message_body(rt, rsp_name, ctx, &rsp_body, &rsp_len, ...);
    
    // 4. 清理
    secs_sml_render_context_destroy(ctx);
    secs_free(rsp_name);
    
    // 5. 返回给 Handler 输出
    *out_body = rsp_body;
    *out_body_n = rsp_len;
}
```

---

## 内存管理铁律

1.  **句柄销毁**：所有 `_create` 出来的对象 (`ctx`, `sess`, `item`, `builder`, `rt`) 必须调用对应的 `_destroy`。
2.  **库分配字符串**：`secs_error_message`, `match_response` 返回的 char* 必须用 `secs_free`。
3.  **编码输出**：在“普通调用场景”下（例如你主动发起 request），`secs_ii_encode` 返回的字节数组必须用 `secs_free`；在 protocol handler 回调里则由框架负责释放（回调里不要释放）。
4.  **Handler 输出**：赋值给 `*out_body` 的指针必须是 `secs_malloc` 分配的（或由库函数如 `secs_ii_encode` / `secs_sml_runtime_encode_message_body` 分配的）。严禁使用系统 `malloc`。

---

## 测试与 fuzz（库自带）

- 单元测试：`ctest --test-dir build --output-on-failure`
- 协议编解码确定性 fuzz/差分：`ctest --test-dir build -R 'hsms_codec_fuzz|sml_fuzz' --output-on-failure`
- 指标 hook 冒烟：`ctest --test-dir build -R metrics_hook --output-on-failure`
- 可选：libFuzzer targets（见 `README.md` 的 “可选：fuzz（libFuzzer）”）
