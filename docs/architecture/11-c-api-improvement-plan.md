# C API 易用性改进计划 (C API Usability Improvement Plan)

> 文档创建日期：2026-01-18
> 状态：提案 (Proposal)

本文档基于对当前 `secs_c_api` 的审查以及 `examples/c_api_sml_ceid_complete.c` 的痛点分析，旨在提出一套切实可行的改进方案，使 C API 的开发体验接近 C++，降低使用门槛。

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

**新增 API 签名**：
```c
/* 便捷注入函数：成功返回 SECS_OK (0)，失败返回错误码 */
SECS_API secs_error_t secs_sml_ctx_set_ascii(secs_sml_render_context_t* ctx, const char* name, const char* value);
SECS_API secs_error_t secs_sml_ctx_set_u1(secs_sml_render_context_t* ctx, const char* name, uint8_t value);
SECS_API secs_error_t secs_sml_ctx_set_u2(secs_sml_render_context_t* ctx, const char* name, uint16_t value);
SECS_API secs_error_t secs_sml_ctx_set_u4(secs_sml_render_context_t* ctx, const char* name, uint32_t value);
SECS_API secs_error_t secs_sml_ctx_set_i4(secs_sml_render_context_t* ctx, const char* name, int32_t value);
SECS_API secs_error_t secs_sml_ctx_set_f4(secs_sml_render_context_t* ctx, const char* name, float value);
SECS_API secs_error_t secs_sml_ctx_set_f8(secs_sml_render_context_t* ctx, const char* name, double value);
// ... 其他类型同理
```

**对比效果**：
```c
// [Before]
secs_ii_item_t* tmp = NULL;
secs_ii_item_create_u2(&val, 1, &tmp);
secs_sml_render_context_set(ctx, "ID", tmp);
secs_ii_item_destroy(tmp);

// [After]
secs_sml_ctx_set_u2(ctx, "ID", val);
```

### 3.2 Phase 2: 列表构建便捷化 (List Builder Helpers) [P1]

**痛点**：手动构建嵌套 List 需要大量创建 Item 和 Append 操作，且需时刻警惕内存释放。

**改进方案**：提供“值语义”的 Append 函数。

**新增 API 签名**：
```c
/* 直接向 List 追加值，内部负责创建 Item */
SECS_API secs_error_t secs_ii_list_append_ascii(secs_ii_item_t* list, const char* value);
SECS_API secs_ii_list_append_u2(secs_ii_item_t* list, uint16_t value);
SECS_API secs_ii_list_append_f4(secs_ii_item_t* list, float value);
/* 追加一个新的空 List 并返回其指针（所有权归父 List，用户无需 destroy） */
SECS_API secs_error_t secs_ii_list_append_new_list(secs_ii_item_t* parent, secs_ii_item_t** out_child);
```

### 3.3 Phase 3: 数据提取便捷化 (Extraction Helpers) [P1]

**痛点**：从 Response 中读取数据需要 `decode` -> `get_child` -> `get_view` 三步走，且涉及繁琐的类型检查。

**改进方案**：提供基于路径/索引的强类型 Getter。

**新增 API 签名**：
```c
/* 直接获取指定路径的 U2 值
 * 参数: root, out_val, path_depth, ...indices
 * 示例: get_u2(root, &val, 2, 0, 1) // 获取 root[0][1]
 */
SECS_API secs_error_t secs_ii_get_u2_at_path(const secs_ii_item_t* root, uint16_t* out_val, size_t depth, ...);

/* 简化版：获取单层 List 下标的值 */
SECS_API secs_error_t secs_ii_get_ascii_at(const secs_ii_item_t* list, size_t index, const char** out_str, size_t* out_len);
```

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

## 4. 实施策略

建议优先实施 **Phase 1 (Context Helpers)**。
理由：
1.  **需求最迫切**：SML 的核心就是变量注入，这部分代码在业务逻辑中占比最高。
2.  **实现成本低**：只需对现有 API 进行简单的包装。
3.  **收益立竿见影**：能立即简化 `examples/c_api_sml_ceid_complete.c` 中的大部分代码。

## 5. 验收标准

以重构后的 `c_api_sml_ceid_complete.c` 为准：
*   代码行数减少 30% 以上。
*   `secs_ii_item_destroy` 的调用次数减少 80%。
*   业务逻辑（变量注入）部分不再夹杂 `secs_ii_item_create` 等底层操作。
