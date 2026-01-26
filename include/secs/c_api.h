/*
 * secs/c_api.h
 *
 * C 语言对外接口（C ABI）。
 *
 * 设计目标：
 * - 允许纯 C 工程通过 `#include <secs/c_api.h>` 调用本库能力；
 * - 所有 C++ 类型均通过不透明句柄（opaque handle）隐藏；
 * - 错误使用 `secs_error_t` 表达（value + category），兼容 std::error_code；
 * - 任何由库分配的内存都使用 `secs_free()` 释放；
 * - C API 内部不允许异常跨越 C 边界（若发生异常，将转为 `secs.c_api` 错误）。
 *
 * 注意：
 * - 本库实现基于 C++20；C 工程链接时通常需要用 C++ 链接器（例如 g++/clang++）。
 * - 部分 API 为“阻塞式”，不得在库内部回调线程（io 线程）中调用，否则会返回
 * WRONG_THREAD。
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* C API 版本（用于 ABI 变更时做兼容分支） */
#define SECS_C_API_VERSION 1

/* ----------------------------- 错误与内存 ----------------------------- */

/*
 * `secs_error_t` 对应 C++ 的 std::error_code。
 *
 * - value==0 表示成功；
 * - category 指向一个静态字符串（生命周期贯穿整个进程），典型值：
 *   - "secs.c_api"（本 C API 自身的错误域）
 *   - "secs.core" / "secs.secs1" / "secs.ii" / "sml.lexer" / "sml.parser"
 *   - "system" / "generic"
 */
typedef struct secs_error {
    int value;
    const char *category;
} secs_error_t;

static inline int secs_error_is_ok(secs_error_t err) { return err.value == 0; }

/* 本 C API 自身的错误码（category="secs.c_api"） */
typedef enum secs_c_api_errc {
    SECS_C_API_OK = 0,
    SECS_C_API_INVALID_ARGUMENT = 1,
    SECS_C_API_NOT_FOUND = 2,
    SECS_C_API_OUT_OF_MEMORY = 3,
    SECS_C_API_WRONG_THREAD = 4,
    SECS_C_API_EXCEPTION = 5
} secs_c_api_errc_t;

/* 由库分配的内存统一用 secs_free 释放（例如：secs_error_message
 * 返回的字符串、encode 输出的字节等）。 */
void *secs_malloc(size_t n);
void secs_free(void *p);

/* 生成可读错误信息（返回的字符串需用 secs_free 释放）。 */
char *secs_error_message(secs_error_t err);

/* 版本信息（静态字符串，勿释放）。 */
const char *secs_version_string(void);

/* ----------------------------- 日志 ----------------------------- */

/* 日志级别（映射到库内部 spdlog 的全局级别）。 */
typedef enum secs_log_level {
    SECS_LOG_TRACE = 0,
    SECS_LOG_DEBUG = 1,
    SECS_LOG_INFO = 2,
    SECS_LOG_WARN = 3,
    SECS_LOG_ERROR = 4,
    SECS_LOG_CRITICAL = 5,
    SECS_LOG_OFF = 6,
    /* 说明：作为 C API，调用方可能传入非法枚举值；添加上界哨兵以避免 UBSan
       将“越界 enum 值”视为未定义行为（实现侧会自行校验并返回错误码）。 */
    SECS_LOG__MAX = 2147483647
} secs_log_level_t;

secs_error_t secs_log_set_level(secs_log_level_t level);

/* ----------------------------- 指标（metrics） ----------------------------- */

/*
 * 运行时指标 hook（可观测性）：
 * - 由业务侧注入回调（例如对接 Prometheus/OpenTelemetry/自研 metrics）；
 * - 默认无 hook：库内为 no-op；
 * - 回调会在库内多个线程/协程中被调用，必须线程安全且尽量不要阻塞。
 *
 * 约定：
 * - name 为 UTF-8 的 C 字符串，生命周期至少覆盖本次回调调用；
 * - 回调如需持久化 name，请自行拷贝。
 */
typedef void (*secs_metrics_counter_fn)(void *user_data,
                                        const char *name,
                                        uint64_t delta);
typedef void (*secs_metrics_gauge_fn)(void *user_data,
                                      const char *name,
                                      int64_t value);
typedef void (*secs_metrics_histogram_fn)(void *user_data,
                                          const char *name,
                                          uint64_t value);

typedef struct secs_metrics_hook {
    secs_metrics_counter_fn counter;       /* 可为 NULL */
    secs_metrics_gauge_fn gauge;           /* 可为 NULL */
    secs_metrics_histogram_fn histogram;   /* 可为 NULL */
    void *user_data;                       /* 透传给回调 */
} secs_metrics_hook_t;

/*
 * 设置（或清除）全局 metrics hook：
 * - hook==NULL：清除 hook（恢复默认 no-op）；
 * - 成功返回 OK。
 */
secs_error_t secs_metrics_set_hook(const secs_metrics_hook_t *hook);

/* ----------------------------- 上下文（io 线程） -----------------------------
 */

typedef struct secs_context secs_context_t;

/*
 * 创建一个上下文：内部启动 1 个 io 线程，负责运行 asio::io_context。
 *
 * 说明：
 * - 本 C API 的“阻塞式”网络/会话操作会把协程调度到该 io
 * 线程执行，并在调用线程等待结果。
 */
secs_error_t secs_context_create(secs_context_t **out_ctx);

/*
 * 上下文创建参数（可选）。
 *
 * 约定：
 * - 指针为 NULL：使用默认参数；
 * - 字段为 0：表示“使用默认值”（便于 memset(0) 后只覆盖少数字段）。
 */
typedef struct secs_context_options {
    size_t io_threads; /* io 线程数（默认 1） */
} secs_context_options_t;

static inline void
secs_context_options_init_default(secs_context_options_t *out_opt) {
    if (!out_opt) {
        return;
    }
    out_opt->io_threads = 1;
}

/* 使用自定义参数创建上下文（不影响旧 API）。 */
secs_error_t secs_context_create_with_options(secs_context_t **out_ctx,
                                              const secs_context_options_t *opt);
void secs_context_destroy(secs_context_t *ctx);

/* ----------------------------- SECS-II：Item 与编解码
 * ----------------------------- */

typedef struct secs_ii_item secs_ii_item_t;

typedef enum secs_ii_item_type {
    SECS_II_ITEM_LIST = 0,
    SECS_II_ITEM_ASCII = 1,
    SECS_II_ITEM_BINARY = 2,
    SECS_II_ITEM_BOOLEAN = 3,
    SECS_II_ITEM_I1 = 4,
    SECS_II_ITEM_I2 = 5,
    SECS_II_ITEM_I4 = 6,
    SECS_II_ITEM_I8 = 7,
    SECS_II_ITEM_U1 = 8,
    SECS_II_ITEM_U2 = 9,
    SECS_II_ITEM_U4 = 10,
    SECS_II_ITEM_U8 = 11,
    SECS_II_ITEM_F4 = 12,
    SECS_II_ITEM_F8 = 13
} secs_ii_item_type_t;

/* 创建/销毁 */
secs_error_t secs_ii_item_create_list(secs_ii_item_t **out_item);
secs_error_t secs_ii_item_create_ascii(const char *bytes,
                                       size_t n,
                                       secs_ii_item_t **out_item);
/* 便捷：按 NUL 结尾字符串创建 ASCII（等价于 create_ascii(value, strlen(value), ...)） */
secs_error_t secs_ii_item_create_ascii_cstr(const char *value,
                                            secs_ii_item_t **out_item);
secs_error_t secs_ii_item_create_binary(const uint8_t *bytes,
                                        size_t n,
                                        secs_ii_item_t **out_item);
secs_error_t secs_ii_item_create_boolean(const uint8_t *values01,
                                         size_t n,
                                         secs_ii_item_t **out_item);

/* 数值类型创建：允许 v==NULL 且 n==0，表示“空数组”。 */
secs_error_t
secs_ii_item_create_i1(const int8_t *v, size_t n, secs_ii_item_t **out_item);
secs_error_t
secs_ii_item_create_i2(const int16_t *v, size_t n, secs_ii_item_t **out_item);
secs_error_t
secs_ii_item_create_i4(const int32_t *v, size_t n, secs_ii_item_t **out_item);
secs_error_t
secs_ii_item_create_i8(const int64_t *v, size_t n, secs_ii_item_t **out_item);
secs_error_t
secs_ii_item_create_u1(const uint8_t *v, size_t n, secs_ii_item_t **out_item);
secs_error_t
secs_ii_item_create_u2(const uint16_t *v, size_t n, secs_ii_item_t **out_item);
secs_error_t
secs_ii_item_create_u4(const uint32_t *v, size_t n, secs_ii_item_t **out_item);
secs_error_t
secs_ii_item_create_u8(const uint64_t *v, size_t n, secs_ii_item_t **out_item);
secs_error_t
secs_ii_item_create_f4(const float *v, size_t n, secs_ii_item_t **out_item);
secs_error_t
secs_ii_item_create_f8(const double *v, size_t n, secs_ii_item_t **out_item);
void secs_ii_item_destroy(secs_ii_item_t *item);

/* 克隆一个 Item（深拷贝，out_item 需用 secs_ii_item_destroy 释放）。 */
secs_error_t secs_ii_item_clone(const secs_ii_item_t *src,
                                secs_ii_item_t **out_item);

/* 类型与访问 */
secs_error_t secs_ii_item_get_type(const secs_ii_item_t *item,
                                   secs_ii_item_type_t *out_type);

secs_error_t secs_ii_item_list_size(const secs_ii_item_t *item, size_t *out_n);
secs_error_t secs_ii_item_list_get(const secs_ii_item_t *item,
                                   size_t index,
                                   secs_ii_item_t **out_child);
secs_error_t secs_ii_item_list_append(secs_ii_item_t *list,
                                      const secs_ii_item_t *elem);

/* ----------------------------- SECS-II：List 构建便捷 API（P1）
 * ----------------------------- */

/*
 * append_take：将 *io_elem 追加到 list（内部拷贝），随后自动 destroy 并将 *io_elem 置空。
 * 用于减少错误路径下的资源泄漏风险。
 */
secs_error_t secs_ii_item_list_append_take(secs_ii_item_t *list,
                                           secs_ii_item_t **io_elem);

/* 直接向 List 追加“字面量值/数组值”，内部负责创建临时 Item 并 append（append 会拷贝）。 */
secs_error_t secs_ii_item_list_append_ascii(secs_ii_item_t *list,
                                            const char *value);
secs_error_t secs_ii_item_list_append_ascii_n(secs_ii_item_t *list,
                                              const char *bytes,
                                              size_t n);
secs_error_t secs_ii_item_list_append_binary(secs_ii_item_t *list,
                                             const uint8_t *bytes,
                                             size_t n);
secs_error_t secs_ii_item_list_append_boolean(secs_ii_item_t *list,
                                              uint8_t value01);
secs_error_t secs_ii_item_list_append_boolean_values(secs_ii_item_t *list,
                                                     const uint8_t *values01,
                                                     size_t n);

secs_error_t secs_ii_item_list_append_i1(secs_ii_item_t *list, int8_t value);
secs_error_t secs_ii_item_list_append_i2(secs_ii_item_t *list, int16_t value);
secs_error_t secs_ii_item_list_append_i4(secs_ii_item_t *list, int32_t value);
secs_error_t secs_ii_item_list_append_i8(secs_ii_item_t *list, int64_t value);
secs_error_t secs_ii_item_list_append_u1(secs_ii_item_t *list, uint8_t value);
secs_error_t secs_ii_item_list_append_u2(secs_ii_item_t *list, uint16_t value);
secs_error_t secs_ii_item_list_append_u4(secs_ii_item_t *list, uint32_t value);
secs_error_t secs_ii_item_list_append_u8(secs_ii_item_t *list, uint64_t value);
secs_error_t secs_ii_item_list_append_f4(secs_ii_item_t *list, float value);
secs_error_t secs_ii_item_list_append_f8(secs_ii_item_t *list, double value);

secs_error_t secs_ii_item_list_append_i1_values(secs_ii_item_t *list,
                                                const int8_t *values,
                                                size_t n);
secs_error_t secs_ii_item_list_append_i2_values(secs_ii_item_t *list,
                                                const int16_t *values,
                                                size_t n);
secs_error_t secs_ii_item_list_append_i4_values(secs_ii_item_t *list,
                                                const int32_t *values,
                                                size_t n);
secs_error_t secs_ii_item_list_append_i8_values(secs_ii_item_t *list,
                                                const int64_t *values,
                                                size_t n);
secs_error_t secs_ii_item_list_append_u1_values(secs_ii_item_t *list,
                                                const uint8_t *values,
                                                size_t n);
secs_error_t secs_ii_item_list_append_u2_values(secs_ii_item_t *list,
                                                const uint16_t *values,
                                                size_t n);
secs_error_t secs_ii_item_list_append_u4_values(secs_ii_item_t *list,
                                                const uint32_t *values,
                                                size_t n);
secs_error_t secs_ii_item_list_append_u8_values(secs_ii_item_t *list,
                                                const uint64_t *values,
                                                size_t n);
secs_error_t secs_ii_item_list_append_f4_values(secs_ii_item_t *list,
                                                const float *values,
                                                size_t n);
secs_error_t secs_ii_item_list_append_f8_values(secs_ii_item_t *list,
                                                const double *values,
                                                size_t n);

/* ----------------------------- SECS-II：Item Builder（P1）
 * ----------------------------- */

/*
 * Builder：用于更顺滑地构建嵌套 List。
 *
 * 设计目标：
 * - 以 begin/end 维护层级，减少“创建子 Item / append / destroy”的手工噪音；
 * - 记忆首个错误：一旦出错，后续调用都返回同一错误且不再修改内部状态；
 * - finalize 统一检查状态并输出最终 root Item。
 *
 * 典型用法：
 *   secs_ii_builder_t *b = NULL;
 *   secs_ii_builder_create(&b);
 *   secs_ii_builder_list_begin(b);        // <L
 *     secs_ii_builder_add_u2(b, 101);     //   <U2 101>
 *     secs_ii_builder_add_ascii(b, "A");  //   <A "A">
 *   secs_ii_builder_list_end(b);          // >
 *   secs_ii_item_t *out = NULL;
 *   secs_error_t err = secs_ii_builder_finalize(b, &out);
 *   secs_ii_builder_destroy(b);
 */
typedef struct secs_ii_builder secs_ii_builder_t;

secs_error_t secs_ii_builder_create(secs_ii_builder_t **out_builder);
void secs_ii_builder_destroy(secs_ii_builder_t *builder);

secs_error_t secs_ii_builder_list_begin(secs_ii_builder_t *builder);
secs_error_t secs_ii_builder_list_end(secs_ii_builder_t *builder);

/* 追加一个已构造的 Item（append 会拷贝）。 */
secs_error_t secs_ii_builder_add_item(secs_ii_builder_t *builder,
                                      const secs_ii_item_t *item);

/* take 语义：append 后自动 destroy 并将 *io_item 置空。 */
secs_error_t secs_ii_builder_add_item_take(secs_ii_builder_t *builder,
                                           secs_ii_item_t **io_item);

/* 直接追加“字面量值/数组值”，内部负责创建临时 Item 并 append。 */
secs_error_t secs_ii_builder_add_ascii(secs_ii_builder_t *builder,
                                       const char *value);
secs_error_t secs_ii_builder_add_ascii_n(secs_ii_builder_t *builder,
                                         const char *bytes,
                                         size_t n);
secs_error_t secs_ii_builder_add_binary(secs_ii_builder_t *builder,
                                        const uint8_t *bytes,
                                        size_t n);
secs_error_t secs_ii_builder_add_boolean(secs_ii_builder_t *builder,
                                         uint8_t value01);
secs_error_t secs_ii_builder_add_boolean_values(secs_ii_builder_t *builder,
                                                const uint8_t *values01,
                                                size_t n);

secs_error_t secs_ii_builder_add_i1(secs_ii_builder_t *builder, int8_t value);
secs_error_t secs_ii_builder_add_i2(secs_ii_builder_t *builder, int16_t value);
secs_error_t secs_ii_builder_add_i4(secs_ii_builder_t *builder, int32_t value);
secs_error_t secs_ii_builder_add_i8(secs_ii_builder_t *builder, int64_t value);
secs_error_t secs_ii_builder_add_u1(secs_ii_builder_t *builder, uint8_t value);
secs_error_t secs_ii_builder_add_u2(secs_ii_builder_t *builder, uint16_t value);
secs_error_t secs_ii_builder_add_u4(secs_ii_builder_t *builder, uint32_t value);
secs_error_t secs_ii_builder_add_u8(secs_ii_builder_t *builder, uint64_t value);
secs_error_t secs_ii_builder_add_f4(secs_ii_builder_t *builder, float value);
secs_error_t secs_ii_builder_add_f8(secs_ii_builder_t *builder, double value);

/*
 * finalize：输出构建结果（成功时 out_item 需用 secs_ii_item_destroy 释放）。
 *
 * 约定：
 * - builder 内部仍有未闭合的 list（begin/end 不匹配）时返回 INVALID_ARGUMENT；
 * - 尚未生成 root（既未 begin/end 生成 List，也未 add_* 生成叶子）时返回 INVALID_ARGUMENT；
 * - finalize 成功后 builder 进入“已完成”状态，不可复用（需重新 create）。
 */
secs_error_t secs_ii_builder_finalize(secs_ii_builder_t *builder,
                                      secs_ii_item_t **out_item);

secs_error_t secs_ii_item_ascii_view(const secs_ii_item_t *item,
                                     const char **out_ptr,
                                     size_t *out_n);
secs_error_t secs_ii_item_binary_view(const secs_ii_item_t *item,
                                      const uint8_t **out_ptr,
                                      size_t *out_n);

/* 注意：Boolean 在 C++ 内部使用
 * vector<bool>，不是连续内存；这里提供“拷贝输出”。 */
secs_error_t secs_ii_item_boolean_copy(const secs_ii_item_t *item,
                                       uint8_t **out_values01,
                                       size_t *out_n);

secs_error_t secs_ii_item_i1_view(const secs_ii_item_t *item,
                                  const int8_t **out_ptr,
                                  size_t *out_n);
secs_error_t secs_ii_item_i2_view(const secs_ii_item_t *item,
                                  const int16_t **out_ptr,
                                  size_t *out_n);
secs_error_t secs_ii_item_i4_view(const secs_ii_item_t *item,
                                  const int32_t **out_ptr,
                                  size_t *out_n);
secs_error_t secs_ii_item_i8_view(const secs_ii_item_t *item,
                                  const int64_t **out_ptr,
                                  size_t *out_n);
secs_error_t secs_ii_item_u1_view(const secs_ii_item_t *item,
                                  const uint8_t **out_ptr,
                                  size_t *out_n);
secs_error_t secs_ii_item_u2_view(const secs_ii_item_t *item,
                                  const uint16_t **out_ptr,
                                  size_t *out_n);
secs_error_t secs_ii_item_u4_view(const secs_ii_item_t *item,
                                  const uint32_t **out_ptr,
                                  size_t *out_n);
secs_error_t secs_ii_item_u8_view(const secs_ii_item_t *item,
                                  const uint64_t **out_ptr,
                                  size_t *out_n);
secs_error_t secs_ii_item_f4_view(const secs_ii_item_t *item,
                                  const float **out_ptr,
                                  size_t *out_n);
secs_error_t secs_ii_item_f8_view(const secs_ii_item_t *item,
                                  const double **out_ptr,
                                  size_t *out_n);

/* ----------------------------- SECS-II：提取便捷 API（P1）
 * ----------------------------- */

/*
 * 通过 0-based List 路径索引提取数据（不创建中间 Item 句柄）：
 * - depth==0 表示选择 root 本身；
 * - depth>0 时，要求每一层都为 List，且 index 不越界。
 *
 * 重要（C varargs）：
 * - `...` 里的每个 index 必须以 `size_t` 传入（例如写成 `(size_t)0`），否则可能触发未定义行为。
 *
 * 约定：
 * - *out_ptr 指向 root 内部内存，生命周期由 root 持有；
 * - get_* 系列（返回单个值）要求目标 Item 为对应类型，且数组长度必须为 1。
 */
secs_error_t secs_ii_item_get_ascii_at(const secs_ii_item_t *list,
                                       size_t index,
                                       const char **out_ptr,
                                       size_t *out_n);
secs_error_t secs_ii_item_get_u2_at_path(const secs_ii_item_t *root,
                                        uint16_t *out_val,
                                        size_t depth,
                                        ...);

secs_error_t secs_ii_item_ascii_view_at_path(const secs_ii_item_t *root,
                                             const char **out_ptr,
                                             size_t *out_n,
                                             size_t depth,
                                             ...);
secs_error_t secs_ii_item_binary_view_at_path(const secs_ii_item_t *root,
                                              const uint8_t **out_ptr,
                                              size_t *out_n,
                                              size_t depth,
                                              ...);
secs_error_t secs_ii_item_boolean_copy_at_path(const secs_ii_item_t *root,
                                               uint8_t **out_values01,
                                               size_t *out_n,
                                               size_t depth,
                                               ...);

secs_error_t secs_ii_item_i1_view_at_path(const secs_ii_item_t *root,
                                         const int8_t **out_ptr,
                                         size_t *out_n,
                                         size_t depth,
                                         ...);
secs_error_t secs_ii_item_i2_view_at_path(const secs_ii_item_t *root,
                                         const int16_t **out_ptr,
                                         size_t *out_n,
                                         size_t depth,
                                         ...);
secs_error_t secs_ii_item_i4_view_at_path(const secs_ii_item_t *root,
                                         const int32_t **out_ptr,
                                         size_t *out_n,
                                         size_t depth,
                                         ...);
secs_error_t secs_ii_item_i8_view_at_path(const secs_ii_item_t *root,
                                         const int64_t **out_ptr,
                                         size_t *out_n,
                                         size_t depth,
                                         ...);
secs_error_t secs_ii_item_u1_view_at_path(const secs_ii_item_t *root,
                                         const uint8_t **out_ptr,
                                         size_t *out_n,
                                         size_t depth,
                                         ...);
secs_error_t secs_ii_item_u2_view_at_path(const secs_ii_item_t *root,
                                         const uint16_t **out_ptr,
                                         size_t *out_n,
                                         size_t depth,
                                         ...);
secs_error_t secs_ii_item_u4_view_at_path(const secs_ii_item_t *root,
                                         const uint32_t **out_ptr,
                                         size_t *out_n,
                                         size_t depth,
                                         ...);
secs_error_t secs_ii_item_u8_view_at_path(const secs_ii_item_t *root,
                                         const uint64_t **out_ptr,
                                         size_t *out_n,
                                         size_t depth,
                                         ...);
secs_error_t secs_ii_item_f4_view_at_path(const secs_ii_item_t *root,
                                         const float **out_ptr,
                                         size_t *out_n,
                                         size_t depth,
                                         ...);
secs_error_t secs_ii_item_f8_view_at_path(const secs_ii_item_t *root,
                                         const double **out_ptr,
                                         size_t *out_n,
                                         size_t depth,
                                         ...);

secs_error_t secs_ii_item_get_i1_at_path(const secs_ii_item_t *root,
                                        int8_t *out_val,
                                        size_t depth,
                                        ...);
secs_error_t secs_ii_item_get_i2_at_path(const secs_ii_item_t *root,
                                        int16_t *out_val,
                                        size_t depth,
                                        ...);
secs_error_t secs_ii_item_get_i4_at_path(const secs_ii_item_t *root,
                                        int32_t *out_val,
                                        size_t depth,
                                        ...);
secs_error_t secs_ii_item_get_i8_at_path(const secs_ii_item_t *root,
                                        int64_t *out_val,
                                        size_t depth,
                                        ...);
secs_error_t secs_ii_item_get_u1_at_path(const secs_ii_item_t *root,
                                        uint8_t *out_val,
                                        size_t depth,
                                        ...);
secs_error_t secs_ii_item_get_u4_at_path(const secs_ii_item_t *root,
                                        uint32_t *out_val,
                                        size_t depth,
                                        ...);
secs_error_t secs_ii_item_get_u8_at_path(const secs_ii_item_t *root,
                                        uint64_t *out_val,
                                        size_t depth,
                                        ...);
secs_error_t secs_ii_item_get_f4_at_path(const secs_ii_item_t *root,
                                        float *out_val,
                                        size_t depth,
                                        ...);
secs_error_t secs_ii_item_get_f8_at_path(const secs_ii_item_t *root,
                                        double *out_val,
                                        size_t depth,
                                        ...);
secs_error_t secs_ii_item_get_boolean_at_path(const secs_ii_item_t *root,
                                             uint8_t *out_val01,
                                             size_t depth,
                                             ...);

/*
 * List-path（array）版本：用 indices[] 替代 C varargs，避免 “未以 size_t 传入”
 * 导致的未定义行为；也便于动态路径（例如从配置/脚本读取）。
 *
 * 约定：
 * - indices_n==0：表示选择 root 本身（此时 indices 可为 NULL）；
 * - indices_n!=0：indices 不得为 NULL；
 * - *out_ptr 指向 root 内部内存（与 *_at_path 保持一致），生命周期由 root 持有；
 * - get_* 系列（返回单个值）要求目标 Item 为对应类型，且数组长度必须为 1。
 */
secs_error_t secs_ii_item_ascii_view_at_list_path(const secs_ii_item_t *root,
                                                  const char **out_ptr,
                                                  size_t *out_n,
                                                  const size_t *indices,
                                                  size_t indices_n);
secs_error_t secs_ii_item_binary_view_at_list_path(const secs_ii_item_t *root,
                                                   const uint8_t **out_ptr,
                                                   size_t *out_n,
                                                   const size_t *indices,
                                                   size_t indices_n);
secs_error_t secs_ii_item_boolean_copy_at_list_path(const secs_ii_item_t *root,
                                                    uint8_t **out_values01,
                                                    size_t *out_n,
                                                    const size_t *indices,
                                                    size_t indices_n);

secs_error_t secs_ii_item_i1_view_at_list_path(const secs_ii_item_t *root,
                                               const int8_t **out_ptr,
                                               size_t *out_n,
                                               const size_t *indices,
                                               size_t indices_n);
secs_error_t secs_ii_item_i2_view_at_list_path(const secs_ii_item_t *root,
                                               const int16_t **out_ptr,
                                               size_t *out_n,
                                               const size_t *indices,
                                               size_t indices_n);
secs_error_t secs_ii_item_i4_view_at_list_path(const secs_ii_item_t *root,
                                               const int32_t **out_ptr,
                                               size_t *out_n,
                                               const size_t *indices,
                                               size_t indices_n);
secs_error_t secs_ii_item_i8_view_at_list_path(const secs_ii_item_t *root,
                                               const int64_t **out_ptr,
                                               size_t *out_n,
                                               const size_t *indices,
                                               size_t indices_n);
secs_error_t secs_ii_item_u1_view_at_list_path(const secs_ii_item_t *root,
                                               const uint8_t **out_ptr,
                                               size_t *out_n,
                                               const size_t *indices,
                                               size_t indices_n);
secs_error_t secs_ii_item_u2_view_at_list_path(const secs_ii_item_t *root,
                                               const uint16_t **out_ptr,
                                               size_t *out_n,
                                               const size_t *indices,
                                               size_t indices_n);
secs_error_t secs_ii_item_u4_view_at_list_path(const secs_ii_item_t *root,
                                               const uint32_t **out_ptr,
                                               size_t *out_n,
                                               const size_t *indices,
                                               size_t indices_n);
secs_error_t secs_ii_item_u8_view_at_list_path(const secs_ii_item_t *root,
                                               const uint64_t **out_ptr,
                                               size_t *out_n,
                                               const size_t *indices,
                                               size_t indices_n);
secs_error_t secs_ii_item_f4_view_at_list_path(const secs_ii_item_t *root,
                                               const float **out_ptr,
                                               size_t *out_n,
                                               const size_t *indices,
                                               size_t indices_n);
secs_error_t secs_ii_item_f8_view_at_list_path(const secs_ii_item_t *root,
                                               const double **out_ptr,
                                               size_t *out_n,
                                               const size_t *indices,
                                               size_t indices_n);

secs_error_t secs_ii_item_get_u2_at_list_path(const secs_ii_item_t *root,
                                             uint16_t *out_val,
                                             const size_t *indices,
                                             size_t indices_n);
secs_error_t secs_ii_item_get_i1_at_list_path(const secs_ii_item_t *root,
                                             int8_t *out_val,
                                             const size_t *indices,
                                             size_t indices_n);
secs_error_t secs_ii_item_get_i2_at_list_path(const secs_ii_item_t *root,
                                             int16_t *out_val,
                                             const size_t *indices,
                                             size_t indices_n);
secs_error_t secs_ii_item_get_i4_at_list_path(const secs_ii_item_t *root,
                                             int32_t *out_val,
                                             const size_t *indices,
                                             size_t indices_n);
secs_error_t secs_ii_item_get_i8_at_list_path(const secs_ii_item_t *root,
                                             int64_t *out_val,
                                             const size_t *indices,
                                             size_t indices_n);
secs_error_t secs_ii_item_get_u1_at_list_path(const secs_ii_item_t *root,
                                             uint8_t *out_val,
                                             const size_t *indices,
                                             size_t indices_n);
secs_error_t secs_ii_item_get_u4_at_list_path(const secs_ii_item_t *root,
                                             uint32_t *out_val,
                                             const size_t *indices,
                                             size_t indices_n);
secs_error_t secs_ii_item_get_u8_at_list_path(const secs_ii_item_t *root,
                                             uint64_t *out_val,
                                             const size_t *indices,
                                             size_t indices_n);
secs_error_t secs_ii_item_get_f4_at_list_path(const secs_ii_item_t *root,
                                             float *out_val,
                                             const size_t *indices,
                                             size_t indices_n);
secs_error_t secs_ii_item_get_f8_at_list_path(const secs_ii_item_t *root,
                                             double *out_val,
                                             const size_t *indices,
                                             size_t indices_n);
secs_error_t secs_ii_item_get_boolean_at_list_path(const secs_ii_item_t *root,
                                                  uint8_t *out_val01,
                                                  const size_t *indices,
                                                  size_t indices_n);

/* 编解码（返回的 out_bytes 需用 secs_free 释放） */
secs_error_t
secs_ii_encode(const secs_ii_item_t *item, uint8_t **out_bytes, size_t *out_n);
secs_error_t secs_ii_decode_one(const uint8_t *in_bytes,
                                size_t in_n,
                                size_t *out_consumed,
                                secs_ii_item_t **out_item);

/*
 * 解码资源限制（对应 C++：secs::ii::DecodeLimits）。
 *
 * 约定：
 * - 若传入指针为 NULL：使用库默认限制；
 * - 若传入字段值为 0：表示“使用库默认值”（便于 memset(0) 后只覆盖少数字段）。
 */
typedef struct secs_ii_decode_limits {
    size_t max_depth;
    uint32_t max_list_items;
    uint32_t max_payload_bytes;
    size_t max_total_items;
    size_t max_total_bytes;
} secs_ii_decode_limits_t;

/* 获取库默认的 DecodeLimits（out_limits 由调用方提供）。 */
void secs_ii_decode_limits_init_default(secs_ii_decode_limits_t *out_limits);

/* 带资源限制的解码（out_item 需用 secs_ii_item_destroy 释放）。 */
secs_error_t secs_ii_decode_one_with_limits(const uint8_t *in_bytes,
                                            size_t in_n,
                                            const secs_ii_decode_limits_t *limits,
                                            size_t *out_consumed,
                                            secs_ii_item_t **out_item);

/* ----------------------------- SML：加载/匹配/取模板
 * ----------------------------- */

typedef struct secs_sml_runtime secs_sml_runtime_t;

secs_error_t secs_sml_runtime_create(secs_sml_runtime_t **out_rt);
void secs_sml_runtime_destroy(secs_sml_runtime_t *rt);

secs_error_t secs_sml_runtime_load(secs_sml_runtime_t *rt,
                                   const char *source,
                                   size_t source_n);
/* 便捷：按 NUL 结尾字符串加载（等价于 load(rt, source, strlen(source))） */
secs_error_t secs_sml_runtime_load_cstr(secs_sml_runtime_t *rt,
                                        const char *source);

/*
 * 匹配条件响应：
 * - 输入：stream/function + SECS-II 消息体（编码后的 bytes）
 * - 输出：若命中规则，返回响应“消息名”（NUL 结尾字符串，需
 * secs_free）；未命中则 out_name=NULL 且返回 OK。
 */
secs_error_t secs_sml_runtime_match_response(const secs_sml_runtime_t *rt,
                                             uint8_t stream,
                                             uint8_t function,
                                             const uint8_t *body_bytes,
                                             size_t body_n,
                                             char **out_name);

/*
 * 获取消息模板（按消息名；也支持直接传入 "SxFy"）并输出其 SECS-II 消息体（编码 bytes）。
 * 若不存在返回 NOT_FOUND。
 */
secs_error_t
secs_sml_runtime_get_message_body_by_name(const secs_sml_runtime_t *rt,
                                          const char *name,
                                          uint8_t **out_body_bytes,
                                          size_t *out_body_n,
                                          uint8_t *out_stream,
                                          uint8_t *out_function,
                                          int *out_w_bit);

/* ----------------------------- SML RenderContext
 * ----------------------------- */

typedef struct secs_sml_render_context secs_sml_render_context_t;

/* 创建/销毁/复用（clear 后可重复使用） */
secs_error_t secs_sml_render_context_create(secs_sml_render_context_t **out_ctx);
void secs_sml_render_context_destroy(secs_sml_render_context_t *ctx);
void secs_sml_render_context_clear(secs_sml_render_context_t *ctx);

/*
 * Sticky Error Context（可选，默认关闭）：
 * - begin 后：set/set_* 会记忆首个错误，并在后续调用中短路返回该错误；
 * - end：返回首错（或 OK），并关闭该模式。
 */
secs_error_t secs_sml_render_context_begin(secs_sml_render_context_t *ctx);
secs_error_t secs_sml_render_context_end(secs_sml_render_context_t *ctx);

/* 设置变量：name -> SECS-II Item（value 由调用方持有，本函数会拷贝）。 */
secs_error_t secs_sml_render_context_set(secs_sml_render_context_t *ctx,
                                         const char *name,
                                         const secs_ii_item_t *value);

/*
 * 便捷设置变量：内部创建一个临时 SECS-II Item 并 set，随后自动释放临时 Item。
 * - 数值/Boolean 类型：长度均为 1
 * - ASCII：长度为 strlen(value)
 * - Binary：长度为 n
 * 用于减少调用方样板代码与内存管理负担。
 */
secs_error_t secs_sml_render_context_set_ascii(secs_sml_render_context_t *ctx,
                                               const char *name,
                                               const char *value);
secs_error_t secs_sml_render_context_set_binary(secs_sml_render_context_t *ctx,
                                                const char *name,
                                                const uint8_t *bytes,
                                                size_t n);
secs_error_t secs_sml_render_context_set_boolean(secs_sml_render_context_t *ctx,
                                                 const char *name,
                                                 uint8_t value01);
secs_error_t secs_sml_render_context_set_i1(secs_sml_render_context_t *ctx,
                                            const char *name,
                                            int8_t value);
secs_error_t secs_sml_render_context_set_i2(secs_sml_render_context_t *ctx,
                                            const char *name,
                                            int16_t value);
secs_error_t secs_sml_render_context_set_i4(secs_sml_render_context_t *ctx,
                                            const char *name,
                                            int32_t value);
secs_error_t secs_sml_render_context_set_i8(secs_sml_render_context_t *ctx,
                                            const char *name,
                                            int64_t value);
secs_error_t secs_sml_render_context_set_u1(secs_sml_render_context_t *ctx,
                                            const char *name,
                                            uint8_t value);
secs_error_t secs_sml_render_context_set_u2(secs_sml_render_context_t *ctx,
                                            const char *name,
                                            uint16_t value);
secs_error_t secs_sml_render_context_set_u4(secs_sml_render_context_t *ctx,
                                            const char *name,
                                            uint32_t value);
secs_error_t secs_sml_render_context_set_u8(secs_sml_render_context_t *ctx,
                                            const char *name,
                                            uint64_t value);
secs_error_t secs_sml_render_context_set_f4(secs_sml_render_context_t *ctx,
                                            const char *name,
                                            float value);
secs_error_t secs_sml_render_context_set_f8(secs_sml_render_context_t *ctx,
                                            const char *name,
                                            double value);

/*
 * 获取变量：name -> SECS-II Item（out_value 需用 secs_ii_item_destroy 释放）。
 * 若不存在返回 NOT_FOUND。
 */
secs_error_t secs_sml_render_context_get(const secs_sml_render_context_t *ctx,
                                         const char *name,
                                         secs_ii_item_t **out_value);

/*
 * 便捷读取变量（用于减少调用方的样板代码）：
 * - 数值/Boolean 类型：要求数组长度必须为 1，输出标量值；
 * - ASCII/Binary：输出 view（指向 ctx 内部内存），生命周期随 ctx；
 * - 若不存在返回 NOT_FOUND；
 * - 类型不匹配/长度不符合约定返回 INVALID_ARGUMENT。
 */
secs_error_t secs_sml_render_context_get_ascii_view(const secs_sml_render_context_t *ctx,
                                                    const char *name,
                                                    const char **out_ptr,
                                                    size_t *out_n);
secs_error_t secs_sml_render_context_get_binary_view(const secs_sml_render_context_t *ctx,
                                                     const char *name,
                                                     const uint8_t **out_ptr,
                                                     size_t *out_n);
secs_error_t secs_sml_render_context_get_boolean(const secs_sml_render_context_t *ctx,
                                                 const char *name,
                                                 uint8_t *out_value01);
secs_error_t secs_sml_render_context_get_i1(const secs_sml_render_context_t *ctx,
                                            const char *name,
                                            int8_t *out_value);
secs_error_t secs_sml_render_context_get_i2(const secs_sml_render_context_t *ctx,
                                            const char *name,
                                            int16_t *out_value);
secs_error_t secs_sml_render_context_get_i4(const secs_sml_render_context_t *ctx,
                                            const char *name,
                                            int32_t *out_value);
secs_error_t secs_sml_render_context_get_i8(const secs_sml_render_context_t *ctx,
                                            const char *name,
                                            int64_t *out_value);
secs_error_t secs_sml_render_context_get_u1(const secs_sml_render_context_t *ctx,
                                            const char *name,
                                            uint8_t *out_value);
secs_error_t secs_sml_render_context_get_u2(const secs_sml_render_context_t *ctx,
                                            const char *name,
                                            uint16_t *out_value);
secs_error_t secs_sml_render_context_get_u4(const secs_sml_render_context_t *ctx,
                                            const char *name,
                                            uint32_t *out_value);
secs_error_t secs_sml_render_context_get_u8(const secs_sml_render_context_t *ctx,
                                            const char *name,
                                            uint64_t *out_value);
secs_error_t secs_sml_render_context_get_f4(const secs_sml_render_context_t *ctx,
                                            const char *name,
                                            float *out_value);
secs_error_t secs_sml_render_context_get_f8(const secs_sml_render_context_t *ctx,
                                            const char *name,
                                            double *out_value);

/* ----------------------------- SML Runtime（Context-Aware）
 * ----------------------------- */

/*
 * 渲染并编码消息模板（支持占位符变量注入）：
 * - name_or_sf：消息名；也支持 "SxFy"（例如 "S2F22"）
 * - ctx：渲染上下文（可为 NULL，表示空上下文）
 *
 * 失败场景：
 * - 找不到消息：secs.core/invalid_argument
 * - 渲染失败：sml.render
 * - 编码失败：secs.ii
 */
secs_error_t secs_sml_runtime_encode_message_body(
    const secs_sml_runtime_t *rt,
    const char *name_or_sf,
    const secs_sml_render_context_t *ctx,
    uint8_t **out_body_bytes,
    size_t *out_body_n,
    uint8_t *out_stream,
    uint8_t *out_function,
    int *out_w_bit);

/*
 * 匹配条件响应（支持期望值占位符）：
 * - ctx：渲染上下文（可为 NULL，表示空上下文）
 */
secs_error_t secs_sml_runtime_match_response_with_context(
    const secs_sml_runtime_t *rt,
    uint8_t stream,
    uint8_t function,
    const uint8_t *body_bytes,
    size_t body_n,
    const secs_sml_render_context_t *ctx,
    char **out_name);

/*
 * 匹配条件响应并捕获 Data Capture 变量（$NAME）：
 * - ctx：渲染上下文（可为 NULL，表示空上下文；用于旧的 `==<Item>` 期望值占位符渲染）
 * - 命中：out_name 返回响应名；若 out_captures 非 NULL，则返回捕获到的 RenderContext
 *  （需用 secs_sml_render_context_destroy 释放）
 * - 未命中：out_name=NULL；out_captures（若非 NULL）返回 NULL；并返回 OK
 */
secs_error_t secs_sml_runtime_match_response_with_capture(
    const secs_sml_runtime_t *rt,
    uint8_t stream,
    uint8_t function,
    const uint8_t *body_bytes,
    size_t body_n,
    const secs_sml_render_context_t *ctx,
    char **out_name,
    secs_sml_render_context_t **out_captures);

/* 条件匹配失败轨迹（用于调试/错误提示）。 */
typedef struct secs_sml_match_trace {
    size_t rule_index;
    const char *condition_message_name;
    int has_index;
    size_t index;
    int has_list_index;
    size_t list_index;
    int reason; /* secs::sml::MatchFailureReason（C++ enum class） */
    const char *detail;
} secs_sml_match_trace_t;

/*
 * 匹配条件响应（返回详细失败轨迹）：
 * - 命中规则：out_name 为响应名；out_traces=NULL 且 out_trace_count==0；
 * - 未命中：out_name=NULL；out_traces/out_trace_count 返回每条规则失败原因。
 *
 * 备注：out_traces 指向的内存需要用 secs_sml_match_traces_free 释放。
 */
secs_error_t secs_sml_runtime_match_response_with_trace(
    const secs_sml_runtime_t *rt,
    uint8_t stream,
    uint8_t function,
    const uint8_t *body_bytes,
    size_t body_n,
    const secs_sml_render_context_t *ctx,
    char **out_name,
    secs_sml_match_trace_t **out_traces,
    size_t *out_trace_count);

void secs_sml_match_traces_free(secs_sml_match_trace_t *traces, size_t count);

/* ----------------------------- HSMS：连接/会话（用于协议层）
 * ----------------------------- */

typedef struct secs_hsms_connection secs_hsms_connection_t;
typedef struct secs_hsms_session secs_hsms_session_t;

typedef struct secs_hsms_session_options {
    uint16_t session_id;
    uint32_t t3_ms;
    uint32_t t5_ms;
    uint32_t t6_ms;
    uint32_t t7_ms;
    uint32_t t8_ms;
    uint32_t linktest_interval_ms; /* 0 表示不自动发送 LINKTEST */
    /*
     * Linktest 连续失败阈值：达到阈值后断线。
     * - 0：使用库默认值（默认 1：一次失败即断线）
     * - 其他：会被 clamp 到 [1, +inf)
     */
    uint32_t linktest_max_consecutive_failures;
    int auto_reconnect;
    int passive_accept_select;
} secs_hsms_session_options_t;

typedef struct secs_hsms_data_message {
    uint16_t session_id;
    uint8_t stream;
    uint8_t function;
    int w_bit;
    uint32_t system_bytes;
    uint8_t *body;
    size_t body_n;
} secs_hsms_data_message_t;

void secs_hsms_data_message_free(secs_hsms_data_message_t *msg);

/* 仅用于本地测试/无 socket 环境：创建一对“内存互联”的 HSMS Connection。 */
secs_error_t
secs_hsms_connection_create_memory_duplex(secs_context_t *ctx,
                                          secs_hsms_connection_t **out_client,
                                          secs_hsms_connection_t **out_server);
void secs_hsms_connection_destroy(secs_hsms_connection_t *c);

secs_error_t
secs_hsms_session_create(secs_context_t *ctx,
                         const secs_hsms_session_options_t *options,
                         secs_hsms_session_t **out_sess);

/*
 * 打开连接（阻塞式）。
 *
 * - open_active_ip：主动端，要求 `ip` 为数字 IP（避免 DNS 依赖）；成功后进入
 * selected。
 * - open_active/passive_connection：注入 Connection（例如
 * memory_duplex），用于测试。
 *
 * 注意：这些函数为阻塞式，不得在 io 线程中调用。
 */
secs_error_t secs_hsms_session_open_active_ip(secs_hsms_session_t *sess,
                                              const char *ip,
                                              uint16_t port);

/*
 * 打开被动端（阻塞式）：
 * - 监听 `ip:port` 并接受 1 个连接；
 * - 完成 SELECT 流程后返回（成功进入 selected）。
 *
 * 约定：
 * - `ip` 需为数字 IP（避免 DNS 依赖），可用 "0.0.0.0"/"::" 监听所有地址；
 * - 不得在库内部 io 线程调用，否则返回 WRONG_THREAD。
 */
secs_error_t secs_hsms_session_open_passive_ip(secs_hsms_session_t *sess,
                                               const char *ip,
                                               uint16_t port);

/*
 * 自动重连主循环（阻塞式；不得在库内部 io 线程调用）：
 * - run_active_ip：主动端连接并进入 selected；断线后按 t5_ms 退避并重连。
 * - run_passive_ip：被动端反复 listen/accept；每次连接进入 selected，断线后按 t5_ms 退避并等待下一次连接。
 *
 * 退出条件：
 * - 调用 secs_hsms_session_stop()；
 * - auto_reconnect==0 时：首次断线/失败后返回。
 */
secs_error_t secs_hsms_session_run_active_ip(secs_hsms_session_t *sess,
                                             const char *ip,
                                             uint16_t port);
secs_error_t secs_hsms_session_run_passive_ip(secs_hsms_session_t *sess,
                                              const char *ip,
                                              uint16_t port);
secs_error_t
secs_hsms_session_open_active_connection(secs_hsms_session_t *sess,
                                         secs_hsms_connection_t **io_conn);
secs_error_t
secs_hsms_session_open_passive_connection(secs_hsms_session_t *sess,
                                          secs_hsms_connection_t **io_conn);

secs_error_t secs_hsms_session_is_selected(const secs_hsms_session_t *sess,
                                           int *out_selected);
secs_error_t secs_hsms_session_stop(
    secs_hsms_session_t *sess); /* 非阻塞：可在任意线程调用 */
void secs_hsms_session_destroy(secs_hsms_session_t *sess);

secs_error_t secs_hsms_session_linktest(secs_hsms_session_t *sess);

/* 发送数据消息：若 out_system_bytes!=NULL，会输出本次使用的 system_bytes。 */
secs_error_t
secs_hsms_session_send_data_auto_system_bytes(secs_hsms_session_t *sess,
                                              uint8_t stream,
                                              uint8_t function,
                                              int w_bit,
                                              const uint8_t *body_bytes,
                                              size_t body_n,
                                              uint32_t *out_system_bytes);

/* 发送数据消息（显式指定 system_bytes）：用于回复对端请求。 */
secs_error_t
secs_hsms_session_send_data_with_system_bytes(secs_hsms_session_t *sess,
                                              uint8_t stream,
                                              uint8_t function,
                                              int w_bit,
                                              uint32_t system_bytes,
                                              const uint8_t *body_bytes,
                                              size_t body_n);

/* 接收下一条 data message（阻塞式，timeout_ms=0 表示无限等待）。 */
secs_error_t secs_hsms_session_receive_data(secs_hsms_session_t *sess,
                                            uint32_t timeout_ms,
                                            secs_hsms_data_message_t *out_msg);

/* 发送主消息（W=1）并等待回应（阻塞式，timeout_ms=0 表示使用会话默认 T3）。 */
secs_error_t
secs_hsms_session_request_data(secs_hsms_session_t *sess,
                               uint8_t stream,
                               uint8_t function,
                               const uint8_t *body_bytes,
                               size_t body_n,
                               uint32_t timeout_ms,
                               secs_hsms_data_message_t *out_reply);

/* ----------------------------- 协议层：统一 send/request + handler
 * ----------------------------- */

typedef struct secs_protocol_session secs_protocol_session_t;

/*
 * 协议层会话参数：
 * - 新增：max_pending_requests（对应 C++：protocol::SessionOptions::max_pending_requests）
 * - 新增：dump_flags/dump_sink（对应 C++：protocol::SessionOptions::dump）
 *
 * 约定：
 * - 数值字段为 0：表示使用库默认值；
 * - dump_flags 未设置 SECS_PROTOCOL_DUMP_ENABLE：表示关闭 dump；
 * - 若开启 dump 且未显式指定 TX/RX（即 flags 中既无 TX 也无 RX）：默认同时开启 TX 与 RX。
 */
typedef void (*secs_protocol_dump_sink_fn)(void *user_data,
                                           const char *data,
                                           size_t size);

typedef enum secs_protocol_dump_flags {
    SECS_PROTOCOL_DUMP_ENABLE = 1u << 0,
    SECS_PROTOCOL_DUMP_TX = 1u << 1,
    SECS_PROTOCOL_DUMP_RX = 1u << 2,
    SECS_PROTOCOL_DUMP_COLOR = 1u << 3,
    SECS_PROTOCOL_DUMP_SECS2_DECODE = 1u << 4
} secs_protocol_dump_flags_t;

typedef struct secs_protocol_session_options {
    uint32_t t3_ms;
    uint32_t poll_interval_ms;
    size_t max_pending_requests;
    uint32_t dump_flags;
    secs_protocol_dump_sink_fn dump_sink; /* NULL 表示使用库内 spdlog 输出 */
    void *dump_sink_user;
} secs_protocol_session_options_t;

typedef struct secs_data_message_view {
    uint8_t stream;
    uint8_t function;
    int w_bit;
    uint32_t system_bytes;
    const uint8_t *body;
    size_t body_n;
} secs_data_message_view_t;

typedef struct secs_data_message {
    uint8_t stream;
    uint8_t function;
    int w_bit;
    uint32_t system_bytes;
    uint8_t *body;
    size_t body_n;
} secs_data_message_t;

void secs_data_message_free(secs_data_message_t *msg);

/*
 * handler 回调：
 * - 在库内部 io 线程调用；
 * - 如果 request.w_bit==1，库会自动把回调返回的 body 作为 secondary body
 * 回给对端；
 * - 回调返回 OK 表示成功；非 OK 表示拒绝处理（库将不回包）。
 *
 * 重要：out_body 必须使用 `secs_malloc()` 分配（库会在复制后调用 secs_free
 * 释放）。
 */
typedef secs_error_t (*secs_protocol_handler_fn)(
    void *user_data,
    const secs_data_message_view_t *request,
    uint8_t **out_body,
    size_t *out_body_n);

/*
 * decoded handler 回调（更贴近 C++ TypedHandler）：
 * - 框架先把 request.body 解码为 SECS-II Item（decoded_body），再调用回调；
 * - 回调返回 OK 表示“已处理”，非 OK 表示“拒绝处理”（库不回包）；
 * - out_item_body（可选）由回调创建并返回，框架会负责 encode 并销毁该 Item；
 * - decoded_body 生命周期仅在回调期间有效；如需跨回调保存，请调用 secs_ii_item_clone。
 */
typedef secs_error_t (*secs_protocol_decoded_handler_fn)(
    void *user_data,
    const secs_data_message_view_t *request,
    const secs_ii_item_t *decoded_body,
    secs_ii_item_t **out_item_body);

/* ----------------------------- CEID：简易处理层（不引入 GEM）
 * ----------------------------- */

typedef struct secs_ceid_dispatcher secs_ceid_dispatcher_t;

/*
 * CEID handler 回调：
 * - 在库内部 io 线程调用；
 * - ceid 为库从 request body 解码并按“路径”提取出的值；
 * - 如果 request.w_bit==1，库会自动把回调返回的 body 作为 secondary body 回给对端；
 * - 回调返回 OK 表示成功；非 OK 表示拒绝处理（库将不回包）。
 *
 * 重要：out_body 必须使用 `secs_malloc()` 分配（库会在复制后调用 secs_free 释放）。
 */
typedef secs_error_t (*secs_ceid_handler_fn)(void *user_data,
                                            uint32_t ceid,
                                            const secs_data_message_view_t *request,
                                            uint8_t **out_body,
                                            size_t *out_body_n);

/*
 * 创建一个 CEID dispatcher（基于 list path 提取 CEID）。
 *
 * 提取规则：
 * - 先把 request.body 解码为一个 SECS-II Item；
 * - 从 root 开始，按 indices[0..n) 逐级向下取 List 的子元素；
 * - 取到目标 Item 后，要求其为 U1/U2/U4/U8 的“单值标量”（U8 需不溢出 uint32）。
 *
 * 参数：
 * - indices/indices_n：CEID 在 List 中的位置路径；例如 S6F11-like
 *   的 <L <DATAID> <CEID> ...> 可传 indices={1}, indices_n=1。
 *   若 indices_n==0，则表示 root 本身就是 CEID 标量。
 * - decode_limits：用于 decode_one 的资源限制；NULL 表示使用库默认限制。
 * - strict_consumed：非 0 时要求 consumed==body_n，否则返回 invalid_argument。
 */
secs_error_t secs_ceid_dispatcher_create_list_path(
    const size_t *indices,
    size_t indices_n,
    const secs_ii_decode_limits_t *decode_limits,
    int strict_consumed,
    secs_ceid_dispatcher_t **out_disp);

void secs_ceid_dispatcher_destroy(secs_ceid_dispatcher_t *disp);

secs_error_t secs_ceid_dispatcher_set_handler(secs_ceid_dispatcher_t *disp,
                                              uint32_t ceid,
                                              secs_ceid_handler_fn cb,
                                              void *user_data);

secs_error_t secs_ceid_dispatcher_set_default_handler(secs_ceid_dispatcher_t *disp,
                                                      secs_ceid_handler_fn cb,
                                                      void *user_data);

secs_error_t
secs_ceid_dispatcher_clear_default_handler(secs_ceid_dispatcher_t *disp);

secs_error_t secs_ceid_dispatcher_erase_handler(secs_ceid_dispatcher_t *disp,
                                                uint32_t ceid);

/*
 * 将 CEID dispatcher 挂到 protocol session 的 (stream,function) 上。
 *
 * 注意：该函数等价于为 (stream,function) 设置一个 handler；
 * 如需移除可调用 secs_protocol_session_erase_handler(sess, stream, function)。
 */
secs_error_t secs_protocol_session_set_ceid_dispatcher(secs_protocol_session_t *sess,
                                                       uint8_t stream,
                                                       uint8_t function,
                                                       secs_ceid_dispatcher_t *disp);

/*
 * 阻塞式 helper：request/reply 均带 CEID 时，提取并（可选）校验一致性。
 *
 * 行为：
 * - 发送 request（W=1）并等待 reply（secondary）；
 * - 若 body 非空，则解码为 Item 并按 list path 提取 CEID；
 * - verify_equal!=0 时：要求 request/reply 的 CEID 均存在且相等，否则返回 invalid_argument；
 * - 即便校验失败，只要收包成功，out_reply 仍会被填充（便于排查）。
 */
secs_error_t secs_protocol_session_request_with_ceid_list_path(
    secs_protocol_session_t *sess,
    uint8_t stream,
    uint8_t function,
    const uint8_t *body_bytes,
    size_t body_n,
    uint32_t timeout_ms,
    const size_t *ceid_indices,
    size_t ceid_indices_n,
    const secs_ii_decode_limits_t *decode_limits,
    int verify_equal,
    secs_data_message_t *out_reply,
    int *out_has_request_ceid,
    uint32_t *out_request_ceid,
    int *out_has_reply_ceid,
    uint32_t *out_reply_ceid);

/*
 * 从 HSMS 创建协议层会话。
 *
 * 注意：ctx 必须与 hsms_sess 创建时使用的 ctx 完全一致，否则会返回
 * INVALID_ARGUMENT。
 */
secs_error_t secs_protocol_session_create_from_hsms(
    secs_context_t *ctx,
    secs_hsms_session_t *hsms_sess,
    uint16_t session_id,
    const secs_protocol_session_options_t *options,
    secs_protocol_session_t **out_sess);

/*
 * 从 SECS-I（串口）创建协议层会话。
 *
 * 说明：
 * - 函数内部会打开串口并创建底层 `secs::secs1::StateMachine`；
 * - 与 HSMS 不同：SECS-I 是半双工，库不会自动启动后台 async_run。
 *   - Host 侧通常直接调用 secs_protocol_session_request（内部会驱动收发）。
 *   - Equipment 侧通常在主循环里调用 secs_protocol_session_poll_once 处理入站消息。
 *
 * 参数：
 * - serial_path：串口名（Windows: "COM5"；Linux: "/dev/ttyUSB0"）
 * - baud：波特率（<=0 表示不设置，保留系统默认）
 * - device_id：SECS-I DeviceID（双方一致）
 * - reverse_bit：R-bit 方向位（Host->Equipment: 0；Equipment->Host: 1）
 */
secs_error_t secs_protocol_session_create_from_secs1_serial(
    secs_context_t *ctx,
    const char *serial_path,
    int baud,
    uint16_t device_id,
    int reverse_bit,
    const secs_protocol_session_options_t *options,
    secs_protocol_session_t **out_sess);

/*
 * 创建一对“内存互联”的 SECS-I protocol session（用于 loopback，无需真实串口）。
 *
 * 约定：
 * - out_host：reverse_bit=0（Host -> Equipment）
 * - out_equipment：reverse_bit=1（Equipment -> Host）
 */
secs_error_t secs_protocol_session_create_from_secs1_memory_duplex(
    secs_context_t *ctx,
    uint16_t device_id,
    const secs_protocol_session_options_t *options,
    secs_protocol_session_t **out_host,
    secs_protocol_session_t **out_equipment);

secs_error_t secs_protocol_session_stop(
    secs_protocol_session_t *sess); /* 非阻塞：可在任意线程调用 */
void secs_protocol_session_destroy(secs_protocol_session_t *sess);

/*
 * 单步轮询：接收并处理一条入站消息（阻塞式）。
 *
 * - out_handled=1：成功处理到一条消息；
 * - out_handled=0：timeout_ms 内未收到消息（返回 OK）；
 * - 其他：返回错误码（例如 cancelled/invalid_argument）。
 *
 * 注意：
 * - 若 session 内部 run loop 已在运行（HSMS create_from_hsms 默认如此），该函数
 *   可能返回 invalid_argument；
 * - 对 SECS-I，建议使用较小的 timeout_ms 以便及时响应 stop。
 */
secs_error_t secs_protocol_session_poll_once(secs_protocol_session_t *sess,
                                             uint32_t timeout_ms,
                                             int *out_handled);

secs_error_t secs_protocol_session_set_handler(secs_protocol_session_t *sess,
                                               uint8_t stream,
                                               uint8_t function,
                                               secs_protocol_handler_fn cb,
                                               void *user_data);

/* 设置 stream default handler：当未找到精确 (stream,function) handler 时回退到该 stream 的默认 handler。 */
secs_error_t secs_protocol_session_set_stream_default_handler(
    secs_protocol_session_t *sess,
    uint8_t stream,
    secs_protocol_handler_fn cb,
    void *user_data);

secs_error_t secs_protocol_session_clear_stream_default_handler(
    secs_protocol_session_t *sess,
    uint8_t stream);

/* 设置 default handler：当未找到精确 (stream,function) handler 时回退到 default。 */
secs_error_t
secs_protocol_session_set_default_handler(secs_protocol_session_t *sess,
                                          secs_protocol_handler_fn cb,
                                          void *user_data);

secs_error_t
secs_protocol_session_clear_default_handler(secs_protocol_session_t *sess);

/*
 * 使用 SML runtime 设置 default handler（自动条件回包）。
 *
 * 行为：
 * - 仅对入站 primary 且 request.w_bit==1 的消息尝试匹配；
 * - 若 SML 条件规则命中，库会把匹配到的响应消息模板编码为 SECS-II body，并作为
 *   secondary（SxF(y+1), W=0）回给对端；
 * - 若未命中/模板不存在/模板的 (S,F,W) 与期望不一致/解码失败：视为“未处理”，不回包。
 *
 * 说明：
 * - 由于 protocol::Session 的 auto-reply 会固定使用 (request.stream,
 *   request.function+1)，因此这里要求 SML 响应模板的 (S,F,W) 必须等于
 *   (request.stream, request.function+1, 0)。
 * - 函数内部会拷贝 `rt` 的内容，因此调用后 `rt` 可以被销毁。
 * - 该函数等价于设置一个 default handler；如需清除可调用
 *   secs_protocol_session_clear_default_handler()。
 */
secs_error_t
secs_protocol_session_set_sml_default_handler(secs_protocol_session_t *sess,
                                              const secs_sml_runtime_t *rt);

/*
 * 使用 SML runtime 设置 stream default handler（仅对指定 stream 生效）。
 * 语义与 secs_protocol_session_set_sml_default_handler 一致，但注册位置从 “global default” 变为 “stream default”。
 */
secs_error_t secs_protocol_session_set_sml_stream_default_handler(
    secs_protocol_session_t *sess,
    uint8_t stream,
    const secs_sml_runtime_t *rt);

/*
 * decoded handler 注册：框架负责 decode/encode；业务只处理 Item。
 *
 * - decode_limits：NULL 表示使用库默认限制；
 * - strict_consumed!=0：要求 consumed==body_n，否则视为 invalid_argument 并不回包；
 * - out_item_body：回调创建的响应 body（可为 NULL 表示空 body），框架会在 encode 后 destroy。
 */
secs_error_t secs_protocol_session_set_decoded_handler(
    secs_protocol_session_t *sess,
    uint8_t stream,
    uint8_t function,
    const secs_ii_decode_limits_t *decode_limits,
    int strict_consumed,
    secs_protocol_decoded_handler_fn cb,
    void *user_data);

secs_error_t secs_protocol_session_set_decoded_stream_default_handler(
    secs_protocol_session_t *sess,
    uint8_t stream,
    const secs_ii_decode_limits_t *decode_limits,
    int strict_consumed,
    secs_protocol_decoded_handler_fn cb,
    void *user_data);

secs_error_t secs_protocol_session_set_decoded_default_handler(
    secs_protocol_session_t *sess,
    const secs_ii_decode_limits_t *decode_limits,
    int strict_consumed,
    secs_protocol_decoded_handler_fn cb,
    void *user_data);

secs_error_t secs_protocol_session_erase_handler(secs_protocol_session_t *sess,
                                                 uint8_t stream,
                                                 uint8_t function);

secs_error_t secs_protocol_session_send(secs_protocol_session_t *sess,
                                        uint8_t stream,
                                        uint8_t function,
                                        const uint8_t *body_bytes,
                                        size_t body_n);

secs_error_t secs_protocol_session_request(secs_protocol_session_t *sess,
                                           uint8_t stream,
                                           uint8_t function,
                                           const uint8_t *body_bytes,
                                           size_t body_n,
                                           uint32_t timeout_ms,
                                           secs_data_message_t *out_reply);

#ifdef __cplusplus
} /* extern "C" */
#endif
