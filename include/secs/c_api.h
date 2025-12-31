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

/* 类型与访问 */
secs_error_t secs_ii_item_get_type(const secs_ii_item_t *item,
                                   secs_ii_item_type_t *out_type);

secs_error_t secs_ii_item_list_size(const secs_ii_item_t *item, size_t *out_n);
secs_error_t secs_ii_item_list_get(const secs_ii_item_t *item,
                                   size_t index,
                                   secs_ii_item_t **out_child);
secs_error_t secs_ii_item_list_append(secs_ii_item_t *list,
                                      const secs_ii_item_t *elem);

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

/* 编解码（返回的 out_bytes 需用 secs_free 释放） */
secs_error_t
secs_ii_encode(const secs_ii_item_t *item, uint8_t **out_bytes, size_t *out_n);
secs_error_t secs_ii_decode_one(const uint8_t *in_bytes,
                                size_t in_n,
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
 * 获取消息模板（按消息名）并输出其 SECS-II 消息体（编码 bytes）。
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

typedef struct secs_protocol_session_options {
    uint32_t t3_ms;
    uint32_t poll_interval_ms;
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

secs_error_t secs_protocol_session_stop(
    secs_protocol_session_t *sess); /* 非阻塞：可在任意线程调用 */
void secs_protocol_session_destroy(secs_protocol_session_t *sess);

secs_error_t secs_protocol_session_set_handler(secs_protocol_session_t *sess,
                                               uint8_t stream,
                                               uint8_t function,
                                               secs_protocol_handler_fn cb,
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
