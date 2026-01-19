# C API 易用性改进计划 (C API Usability Improvement Plan)

> 文档创建日期：2026-01-18
> 状态：P0/P1 已实现；P2（Sticky Error Context）仍为提案；另新增若干“对齐 C++ 易用性”的补强项

本文档基于对当前 `secs_c_api` 的审查以及历史示例 `examples/legacy/c_api_sml_ceid_complete.c` 的痛点分析，旨在提出一套切实可行的改进方案，使 C API 的开发体验接近 C++，降低使用门槛。

说明：

- 原始痛点示例已归档到 `examples/legacy/`（默认不再构建，便于对照历史问题）。
- 当前推荐从目录顶层的主示例集合 `examples/c_api_*.c` 理解最新用法与最佳实践。

## 1. 现状痛点分析

当前 C API 虽然功能完备（Raw Access），但存在以下严重影响开发效率的问题：

1.  **样板代码过多 (Boilerplate Explosion)**：
    *   完成一个简单的操作（如“设置变量”）需要 3-4 个 API 调用（创建、设置、销毁、检查错误）。
2.  **资源管理繁琐 (Manual Resource Management)**：
    *   用户必须手动追踪并 `destroy` 每一个中间产生的 `secs_ii_item_t`，极易导致内存泄漏或 Use-After-Free。
3.  **错误处理噪音 (Error Handling Noise)**：
    *   每个步骤都返回 `secs_error_t`，导致业务逻辑被淹没在 `if (!ok) goto cleanup` 中。

## 2. 改进目标

1.  **大幅减少代码行数**：将常用操作（如变量注入、列表构建）的代码量减少 50% 以上。
2.  **隐藏中间对象**：用户在做常规业务时，不应感知到临时的 `secs_ii_item_t` 的存在。
3.  **保持 ABI 稳定**：新增的便捷 API 作为 helper 存在，不破坏原有底层 API 的语义。

## 3. 详细改进方案

### 3.1 Phase 1: 上下文操作便捷化 (Context Helpers) [P0]

**痛点**：`RenderContext` 注入变量是 SML 编程最高频的操作，目前极其繁琐。

**改进方案**：引入“直通式” Setter，内部自动处理 Item 的创建与生命周期。

**已实现 API（`include/secs/c_api.h`）**：
```c
/* 便捷注入函数：成功返回 SECS_OK (0)，失败返回错误码 */
secs_error_t secs_sml_render_context_set_ascii(secs_sml_render_context_t* ctx,
                                               const char* name,
                                               const char* value);
secs_error_t secs_sml_render_context_set_binary(secs_sml_render_context_t* ctx,
                                                const char* name,
                                                const uint8_t* bytes,
                                                size_t n);
secs_error_t secs_sml_render_context_set_boolean(secs_sml_render_context_t* ctx,
                                                 const char* name,
                                                 uint8_t value01);
secs_error_t secs_sml_render_context_set_i1(secs_sml_render_context_t* ctx, const char* name, int8_t value);
secs_error_t secs_sml_render_context_set_i2(secs_sml_render_context_t* ctx, const char* name, int16_t value);
secs_error_t secs_sml_render_context_set_i4(secs_sml_render_context_t* ctx, const char* name, int32_t value);
secs_error_t secs_sml_render_context_set_i8(secs_sml_render_context_t* ctx, const char* name, int64_t value);
secs_error_t secs_sml_render_context_set_u1(secs_sml_render_context_t* ctx, const char* name, uint8_t value);
secs_error_t secs_sml_render_context_set_u2(secs_sml_render_context_t* ctx, const char* name, uint16_t value);
secs_error_t secs_sml_render_context_set_u4(secs_sml_render_context_t* ctx, const char* name, uint32_t value);
secs_error_t secs_sml_render_context_set_u8(secs_sml_render_context_t* ctx, const char* name, uint64_t value);
secs_error_t secs_sml_render_context_set_f4(secs_sml_render_context_t* ctx, const char* name, float value);
secs_error_t secs_sml_render_context_set_f8(secs_sml_render_context_t* ctx, const char* name, double value);
```

**对比效果**：
```c
// [Before]
secs_ii_item_t* tmp = NULL;
secs_ii_item_create_u2(&val, 1, &tmp);
secs_sml_render_context_set(ctx, "ID", tmp);
secs_ii_item_destroy(tmp);

// [After]
secs_sml_render_context_set_u2(ctx, "ID", val);
```

### 3.2 Phase 2: 列表构建便捷化 (List Builder Helpers) [P1]

**痛点**：手动构建嵌套 List 需要大量创建 Item 和 Append 操作，且需时刻警惕内存释放。

**改进方案**：提供“值语义”的 Append 函数。

**已实现 API（`include/secs/c_api.h`）**：
```c
/* 将 *io_elem 追加到 list（内部拷贝），随后自动 destroy 并将 *io_elem 置空 */
secs_error_t secs_ii_item_list_append_take(secs_ii_item_t* list, secs_ii_item_t** io_elem);

/* 直接向 List 追加“字面量值/数组值”，内部负责创建临时 Item 并 append */
secs_error_t secs_ii_item_list_append_ascii(secs_ii_item_t* list, const char* value);
secs_error_t secs_ii_item_list_append_ascii_n(secs_ii_item_t* list, const char* bytes, size_t n);
secs_error_t secs_ii_item_list_append_binary(secs_ii_item_t* list, const uint8_t* bytes, size_t n);
secs_error_t secs_ii_item_list_append_boolean(secs_ii_item_t* list, uint8_t value01);
secs_error_t secs_ii_item_list_append_boolean_values(secs_ii_item_t* list, const uint8_t* values01, size_t n);

secs_error_t secs_ii_item_list_append_u2(secs_ii_item_t* list, uint16_t value);
secs_error_t secs_ii_item_list_append_u2_values(secs_ii_item_t* list, const uint16_t* values, size_t n);

/* 其它标量/数组版本见 c_api.h（i1/i2/i4/i8/u1/u4/u8/f4/f8 等） */
```

### 3.3 Phase 3: 数据提取便捷化 (Extraction Helpers) [P1]

**痛点**：从 Response 中读取数据需要 `decode` -> `get_child` -> `get_view` 三步走，且涉及繁琐的类型检查。

**改进方案**：提供基于路径/索引的强类型 Getter。

**已实现 API（`include/secs/c_api.h`）**：
```c
/* 简化版：获取单层 List 下标的 ASCII 值（指针生命周期由 list 持有） */
secs_error_t secs_ii_item_get_ascii_at(const secs_ii_item_t* list,
                                       size_t index,
                                       const char** out_ptr,
                                       size_t* out_n);

/* 基于 0-based List 路径索引提取数据（不创建中间 Item 句柄） */
secs_error_t secs_ii_item_get_u2_at_path(const secs_ii_item_t* root,
                                        uint16_t* out_val,
                                        size_t depth,
                                        ...);

secs_error_t secs_ii_item_u2_view_at_path(const secs_ii_item_t* root,
                                         const uint16_t** out_ptr,
                                         size_t* out_n,
                                         size_t depth,
                                         ...);

/* 其它类型的 view/get 版本见 c_api.h（ascii/binary/boolean/i1/i2/i4/i8/u1/u4/u8/f4/f8 等） */
```

注意：

- at_path 系列的 `...indices` 约定为 `size_t`（例如常量建议写成 `(size_t)0`），否则在 C 的 varargs 下可能触发未定义行为。

### 3.4 Phase 4: 粘性错误上下文 (Sticky Error Context) [P2]

**痛点**：连续的一组 `set` 操作需要写大量的 `if` 检查。

**改进方案**：引入类似 OpenGL/Cairo 的错误状态管理模式（可选开启）。
如果上下文处于错误状态，后续的 `set` 操作立即返回，直到用户主动清除错误或检查错误。

**API 设想**：
```c
// 假设这些函数在内部检查 ctx->error
secs_sml_ctx_set_u2(ctx, "ID", 1);
secs_sml_ctx_set_ascii(ctx, "NAME", "A");
secs_sml_ctx_set_f4(ctx, "VAL", 3.14);

// 最后检查一次
if (secs_sml_ctx_get_last_error(ctx) != SECS_OK) {
    // 处理错误
}
```

### 3.5 Phase 5: Protocol Router 易用性对齐（stream default）[P0]

**动机**：C++ 端 `secs::protocol::Router` 支持 3 级匹配（精确 / stream default / default），C 端此前只能设置“精确 + default”，导致大量 handler 被迫重复注册。

**新增 C API（`include/secs/c_api.h`）**：

- `secs_protocol_session_set_stream_default_handler()`
- `secs_protocol_session_clear_stream_default_handler()`
- `secs_protocol_session_set_sml_stream_default_handler()`（仅对某个 stream 使用 SML 自动回包）

### 3.6 Phase 6: decoded handler（自动 decode/encode，贴近 C++ TypedHandler）[P1]

**动机**：C++ 端 `TypedHandler` 自动处理“decode request body / encode reply body”，业务逻辑只需处理强类型/结构化数据。C 端此前需要在回调内手动调用 `secs_ii_decode_one/secs_ii_encode`，样板代码与内存管理噪音较高。

**新增 C API（`include/secs/c_api.h`）**：

- `secs_protocol_session_set_decoded_handler()`
- `secs_protocol_session_set_decoded_stream_default_handler()`
- `secs_protocol_session_set_decoded_default_handler()`

**配套**：

- `secs_ii_item_clone()`：如需在回调外保留 `decoded_body`，可克隆一份再持有。

### 3.7 Phase 7: 提取 API 去 varargs（at_list_path）[P1]

**动机**：`...` 的 `at_path` 系列要求每个 index 必须以 `size_t` 传入，否则可能触发未定义行为；同时动态路径也不适合 varargs。

**新增 C API（`include/secs/c_api.h`）**：

- `secs_ii_item_*_at_list_path()`：用 `indices[] + indices_n` 替代 varargs，语义与 `*_at_path` 对齐。

## 4. 实施策略

建议优先实施 **Phase 1 (Context Helpers)**。
理由：
1.  **需求最迫切**：SML 的核心就是变量注入，这部分代码在业务逻辑中占比最高。
2.  **实现成本低**：只需对现有 API 进行简单的包装。
3.  **收益立竿见影**：能立即简化 `examples/legacy/c_api_sml_ceid_complete.c` 中的大部分代码。

已完成落地：

- `secs_sml_render_context_set_*()` 便捷注入函数已在 C API 中提供，并用于主示例集合与历史示例 `examples/legacy/c_api_sml_ceid_complete.c`。
- Phase 2（List Builder）与 Phase 3（Extraction Helpers）已在 C API 中提供，并用于主示例集合与历史示例 `examples/legacy/c_api_sml_ceid_complete.c`。

## 5. 验收标准

以主示例集合（`examples/c_api_*.c`）与历史示例 `examples/legacy/c_api_sml_ceid_complete.c` 为准：
*   代码行数减少 30% 以上。
*   `secs_ii_item_destroy` 的调用次数减少 80%。
*   业务逻辑（变量注入）部分不再夹杂 `secs_ii_item_create` 等底层操作。
