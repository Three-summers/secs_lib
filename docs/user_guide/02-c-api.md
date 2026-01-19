# C API 使用指南

> 文档更新：2026-01-19  
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
    
    // 3. 编码响应 (重要: 必须用 secs_free 释放 out_body)
    // 库会自动负责 secs_free，你只需要分配
    secs_ii_encode(rsp_item, out_body, out_body_n);
    
    // 4. 销毁中间 Item
    secs_ii_item_destroy(rsp_item);
    
    return (secs_error_t){0, NULL}; // OK
}
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
    "s1f2: S1F2 <L <A $MDLN> >."
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

如果 SML 模板中包含变量（如 `$MDLN`），你需要创建一个 Context 并注入值，这在 C API 中也是支持的。

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
3.  **编码输出**：`secs_ii_encode` 返回的字节数组必须用 `secs_free`。
4.  **Handler 输出**：赋值给 `*out_body` 的指针必须是 `secs_malloc` 分配的（或由库函数如 `encode` 分配的）。严禁使用系统 `malloc`。
