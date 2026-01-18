/*
 * C API（C ABI）示例：SML + CEID 完整示例（C 语言版本）
 *
 * 目标：
 * - 使用 C API 加载同一份 `sml_ceid_complete.sml`；
 * - server（Equipment）侧：
 *   - 使用 RenderContext 注入动态变量；
 *   - 使用 match_response_with_context() 支持“期望值占位符”（==<... IDENT>）；
 *   - 使用 encode_message_body(..., ctx) 渲染并编码响应模板；
 *   - 使用 match_response_with_trace() 输出失败轨迹，便于调试条件规则。
 * - client（Host）侧：
 *   - 发送多条 S6F11(W=1) 请求（不同 CEID）；
 *   - 接收 S6F12 响应并做轻量校验 + 打印解码内容。
 *
 * 关键点：
 * - 本示例使用 `secs_hsms_connection_create_memory_duplex()`，不依赖 socket，
 *   便于在容器/沙箱环境跨平台运行。
 * - 由于 HSMS 的 open_*_connection 是阻塞式，为了在同进程中完成
 *   active/passive 同时 open，本示例使用 C11 线程（<threads.h>）。
 *
 * 用法：
 *   ./c_api_sml_ceid_complete
 *
 * 运行目录要求：
 * - 可执行文件同目录下需要有 `sml_ceid_complete.sml`（CMake 已在构建后复制）。
 */

#include <secs/c_api.h>

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if defined(__STDC_NO_THREADS__)
#error "This example requires C11 <threads.h> support (__STDC_NO_THREADS__ is defined)."
#endif
#include <threads.h>

static int ensure_ok(const char *what, secs_error_t err) {
    if (secs_error_is_ok(err)) {
        return 1;
    }

    char *msg = secs_error_message(err);
    fprintf(stderr,
            "[失败] %s: category=%s value=%d msg=%s\n",
            what,
            (err.category ? err.category : "(null)"),
            err.value,
            (msg ? msg : "(null)"));
    if (msg) {
        secs_free(msg);
    }
    return 0;
}

static secs_error_t ok(void) {
    secs_error_t e;
    e.value = 0;
    e.category = "secs.c_api";
    return e;
}

static secs_error_t invalid_argument(void) {
    secs_error_t e;
    e.value = (int)SECS_C_API_INVALID_ARGUMENT;
    e.category = "secs.c_api";
    return e;
}

static int read_file_all(const char *path, char **out_buf, size_t *out_n) {
    *out_buf = NULL;
    *out_n = 0;

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        fprintf(stderr, "[失败] fopen(%s)\n", path);
        return 0;
    }

    if (fseek(fp, 0, SEEK_END) != 0) {
        fprintf(stderr, "[失败] fseek(SEEK_END)\n");
        fclose(fp);
        return 0;
    }
    long sz = ftell(fp);
    if (sz < 0) {
        fprintf(stderr, "[失败] ftell\n");
        fclose(fp);
        return 0;
    }
    if (fseek(fp, 0, SEEK_SET) != 0) {
        fprintf(stderr, "[失败] fseek(SEEK_SET)\n");
        fclose(fp);
        return 0;
    }

    size_t n = (size_t)sz;
    char *buf = (char *)secs_malloc(n ? n : 1);
    if (!buf) {
        fprintf(stderr, "[失败] secs_malloc(%zu)\n", n);
        fclose(fp);
        return 0;
    }

    size_t got = 0;
    if (n != 0) {
        got = fread(buf, 1, n, fp);
    }
    fclose(fp);

    if (got != n) {
        fprintf(stderr,
                "[失败] fread: expected=%zu got=%zu\n",
                n,
                got);
        secs_free(buf);
        return 0;
    }

    *out_buf = buf;
    *out_n = n;
    return 1;
}

/* ========== SECS-II Item 工具：编码/解码/访问 ========== */

static int encode_s6f11_body(uint16_t dataid,
                             uint16_t ceid,
                             uint8_t **out_body,
                             size_t *out_body_n) {
    *out_body = NULL;
    *out_body_n = 0;

    secs_error_t err;
    secs_ii_item_t *root = NULL;
    secs_ii_item_t *tmp = NULL;
    uint8_t *bytes = NULL;
    size_t bytes_n = 0;

    if (!secs_error_is_ok(err = secs_ii_item_create_list(&root))) {
        goto cleanup;
    }

    /* <U2 DATAID> */
    if (!secs_error_is_ok(err = secs_ii_item_create_u2(&dataid, 1, &tmp))) {
        goto cleanup;
    }
    if (!secs_error_is_ok(err = secs_ii_item_list_append(root, tmp))) {
        goto cleanup;
    }
    secs_ii_item_destroy(tmp);
    tmp = NULL;

    /* <U2 CEID> */
    if (!secs_error_is_ok(err = secs_ii_item_create_u2(&ceid, 1, &tmp))) {
        goto cleanup;
    }
    if (!secs_error_is_ok(err = secs_ii_item_list_append(root, tmp))) {
        goto cleanup;
    }
    secs_ii_item_destroy(tmp);
    tmp = NULL;

    /* <L> params（本示例为空） */
    if (!secs_error_is_ok(err = secs_ii_item_create_list(&tmp))) {
        goto cleanup;
    }
    if (!secs_error_is_ok(err = secs_ii_item_list_append(root, tmp))) {
        goto cleanup;
    }
    secs_ii_item_destroy(tmp);
    tmp = NULL;

    if (!secs_error_is_ok(err = secs_ii_encode(root, &bytes, &bytes_n))) {
        goto cleanup;
    }

    *out_body = bytes;
    *out_body_n = bytes_n;
    bytes = NULL;
    bytes_n = 0;

cleanup:
    secs_ii_item_destroy(tmp);
    secs_ii_item_destroy(root);
    secs_free(bytes);
    return ensure_ok("encode_s6f11_body", err);
}

static int decode_u2_at_list_index(const uint8_t *body,
                                   size_t body_n,
                                   size_t index,
                                   uint16_t *out_value) {
    *out_value = 0;

    if (!body && body_n != 0) {
        return 0;
    }

    size_t consumed = 0;
    secs_ii_item_t *root = NULL;
    secs_error_t err = secs_ii_decode_one(body, body_n, &consumed, &root);
    if (!ensure_ok("secs_ii_decode_one", err)) {
        secs_ii_item_destroy(root);
        return 0;
    }

    secs_ii_item_t *child = NULL;
    if (!ensure_ok("secs_ii_item_list_get",
                   secs_ii_item_list_get(root, index, &child))) {
        secs_ii_item_destroy(child);
        secs_ii_item_destroy(root);
        return 0;
    }

    const uint16_t *p = NULL;
    size_t n = 0;
    if (!ensure_ok("secs_ii_item_u2_view", secs_ii_item_u2_view(child, &p, &n))) {
        secs_ii_item_destroy(child);
        secs_ii_item_destroy(root);
        return 0;
    }
    if (!p || n != 1u) {
        fprintf(stderr,
                "[失败] decode_u2_at_list_index: expected U2 scalar at index=%zu\n",
                index);
        secs_ii_item_destroy(child);
        secs_ii_item_destroy(root);
        return 0;
    }

    *out_value = p[0];
    secs_ii_item_destroy(child);
    secs_ii_item_destroy(root);
    return 1;
}

/* ========== Pretty print：用于演示解码结果 ========== */

static void print_indent(int indent) {
    for (int i = 0; i < indent * 2; ++i) {
        putchar(' ');
    }
}

static void print_item(const secs_ii_item_t *item, int indent) {
    secs_ii_item_type_t ty;
    if (!ensure_ok("secs_ii_item_get_type", secs_ii_item_get_type(item, &ty))) {
        return;
    }

    if (ty == SECS_II_ITEM_LIST) {
        size_t n = 0;
        if (!ensure_ok("secs_ii_item_list_size",
                       secs_ii_item_list_size(item, &n))) {
            return;
        }
        print_indent(indent);
        printf("<L> (%zu items)\n", n);
        for (size_t i = 0; i < n; ++i) {
            secs_ii_item_t *child = NULL;
            if (!ensure_ok("secs_ii_item_list_get(child)",
                           secs_ii_item_list_get(item, i, &child))) {
                secs_ii_item_destroy(child);
                return;
            }
            print_item(child, indent + 1);
            secs_ii_item_destroy(child);
        }
        return;
    }

    if (ty == SECS_II_ITEM_ASCII) {
        const char *p = NULL;
        size_t n = 0;
        if (!ensure_ok("secs_ii_item_ascii_view",
                       secs_ii_item_ascii_view(item, &p, &n))) {
            return;
        }
        print_indent(indent);
        printf("<A \"%.*s\">\n", (int)n, (p ? p : ""));
        return;
    }

    if (ty == SECS_II_ITEM_U1) {
        const uint8_t *p = NULL;
        size_t n = 0;
        if (!ensure_ok("secs_ii_item_u1_view", secs_ii_item_u1_view(item, &p, &n))) {
            return;
        }
        print_indent(indent);
        printf("<U1");
        for (size_t i = 0; i < n; ++i) {
            printf(" %u", (unsigned)p[i]);
        }
        printf(">\n");
        return;
    }

    if (ty == SECS_II_ITEM_U2) {
        const uint16_t *p = NULL;
        size_t n = 0;
        if (!ensure_ok("secs_ii_item_u2_view", secs_ii_item_u2_view(item, &p, &n))) {
            return;
        }
        print_indent(indent);
        printf("<U2");
        for (size_t i = 0; i < n; ++i) {
            printf(" %u", (unsigned)p[i]);
        }
        printf(">\n");
        return;
    }

    if (ty == SECS_II_ITEM_U4) {
        const uint32_t *p = NULL;
        size_t n = 0;
        if (!ensure_ok("secs_ii_item_u4_view", secs_ii_item_u4_view(item, &p, &n))) {
            return;
        }
        print_indent(indent);
        printf("<U4");
        for (size_t i = 0; i < n; ++i) {
            printf(" %" PRIu32, p[i]);
        }
        printf(">\n");
        return;
    }

    if (ty == SECS_II_ITEM_F4) {
        const float *p = NULL;
        size_t n = 0;
        if (!ensure_ok("secs_ii_item_f4_view", secs_ii_item_f4_view(item, &p, &n))) {
            return;
        }
        print_indent(indent);
        printf("<F4");
        for (size_t i = 0; i < n; ++i) {
            printf(" %g", (double)p[i]);
        }
        printf(">\n");
        return;
    }

    print_indent(indent);
    printf("<type=%d>\n", (int)ty);
}

/* ========== RenderContext：变量注入辅助 ========== */

static int ctx_set_u1(secs_sml_render_context_t *ctx,
                      const char *name,
                      uint8_t v) {
    secs_ii_item_t *item = NULL;
    secs_error_t err = secs_ii_item_create_u1(&v, 1, &item);
    if (!secs_error_is_ok(err)) {
        secs_ii_item_destroy(item);
        return ensure_ok("secs_ii_item_create_u1", err);
    }
    err = secs_sml_render_context_set(ctx, name, item);
    secs_ii_item_destroy(item);
    return ensure_ok("secs_sml_render_context_set(u1)", err);
}

static int ctx_set_u2(secs_sml_render_context_t *ctx,
                      const char *name,
                      uint16_t v) {
    secs_ii_item_t *item = NULL;
    secs_error_t err = secs_ii_item_create_u2(&v, 1, &item);
    if (!secs_error_is_ok(err)) {
        secs_ii_item_destroy(item);
        return ensure_ok("secs_ii_item_create_u2", err);
    }
    err = secs_sml_render_context_set(ctx, name, item);
    secs_ii_item_destroy(item);
    return ensure_ok("secs_sml_render_context_set(u2)", err);
}

static int ctx_set_u4(secs_sml_render_context_t *ctx,
                      const char *name,
                      uint32_t v) {
    secs_ii_item_t *item = NULL;
    secs_error_t err = secs_ii_item_create_u4(&v, 1, &item);
    if (!secs_error_is_ok(err)) {
        secs_ii_item_destroy(item);
        return ensure_ok("secs_ii_item_create_u4", err);
    }
    err = secs_sml_render_context_set(ctx, name, item);
    secs_ii_item_destroy(item);
    return ensure_ok("secs_sml_render_context_set(u4)", err);
}

static int ctx_set_f4(secs_sml_render_context_t *ctx,
                      const char *name,
                      float v) {
    secs_ii_item_t *item = NULL;
    secs_error_t err = secs_ii_item_create_f4(&v, 1, &item);
    if (!secs_error_is_ok(err)) {
        secs_ii_item_destroy(item);
        return ensure_ok("secs_ii_item_create_f4", err);
    }
    err = secs_sml_render_context_set(ctx, name, item);
    secs_ii_item_destroy(item);
    return ensure_ok("secs_sml_render_context_set(f4)", err);
}

static int ctx_set_ascii(secs_sml_render_context_t *ctx,
                         const char *name,
                         const char *s) {
    secs_ii_item_t *item = NULL;
    secs_error_t err = secs_ii_item_create_ascii(s, strlen(s), &item);
    if (!secs_error_is_ok(err)) {
        secs_ii_item_destroy(item);
        return ensure_ok("secs_ii_item_create_ascii", err);
    }
    err = secs_sml_render_context_set(ctx, name, item);
    secs_ii_item_destroy(item);
    return ensure_ok("secs_sml_render_context_set(ascii)", err);
}

struct device_data {
    const char *device_name;
    uint8_t status_code;
    uint32_t uptime_seconds;
    float temp1;
    float temp2;
    float temp3;
    uint16_t alarm_count;
    const char *alarm_msg_1;
    const char *alarm_msg_2;
    uint32_t total_count;
    uint32_t good_count;
    uint32_t bad_count;
};

static int fill_context_for_ceid(uint16_t ceid,
                                 uint16_t dataid,
                                 const struct device_data *data,
                                 secs_sml_render_context_t *ctx) {
    /* 期望值占位符：对应 sml_ceid_complete.sml 里的规则
     * if (s6f11[1]==<U2 EXPECTED_CEID_STATUS>) status_response.
     */
    if (!ctx_set_u2(ctx, "EXPECTED_CEID_STATUS", 0x1001u)) {
        return 0;
    }

    if (!ctx_set_u2(ctx, "DATAID", dataid)) {
        return 0;
    }

    switch (ceid) {
    case 0x1001u:
        if (!ctx_set_ascii(ctx, "DEVICE_NAME", data->device_name)) {
            return 0;
        }
        if (!ctx_set_u1(ctx, "STATUS_CODE", data->status_code)) {
            return 0;
        }
        if (!ctx_set_u4(ctx, "UPTIME_SECONDS", data->uptime_seconds)) {
            return 0;
        }
        break;

    case 0x1002u: {
        if (!ctx_set_f4(ctx, "TEMP_SENSOR_1", data->temp1) ||
            !ctx_set_f4(ctx, "TEMP_SENSOR_2", data->temp2) ||
            !ctx_set_f4(ctx, "TEMP_SENSOR_3", data->temp3)) {
            return 0;
        }
        float avg = (data->temp1 + data->temp2 + data->temp3) / 3.0f;
        if (!ctx_set_f4(ctx, "TEMP_AVG", avg)) {
            return 0;
        }
        break;
    }

    case 0x1003u:
        if (!ctx_set_u2(ctx, "ALARM_COUNT", data->alarm_count)) {
            return 0;
        }
        if (!ctx_set_ascii(ctx, "ALARM_MSG_1", data->alarm_msg_1) ||
            !ctx_set_ascii(ctx, "ALARM_MSG_2", data->alarm_msg_2)) {
            return 0;
        }
        break;

    case 0x1004u: {
        if (!ctx_set_u4(ctx, "TOTAL_COUNT", data->total_count) ||
            !ctx_set_u4(ctx, "GOOD_COUNT", data->good_count) ||
            !ctx_set_u4(ctx, "BAD_COUNT", data->bad_count)) {
            return 0;
        }
        float yield = (data->total_count == 0u)
                          ? 0.0f
                          : ((float)data->good_count / (float)data->total_count) *
                                100.0f;
        if (!ctx_set_f4(ctx, "YIELD_RATE", yield)) {
            return 0;
        }
        break;
    }

    default:
        break;
    }

    return 1;
}

/* ========== match_response_with_trace：失败轨迹打印 ========== */

static void dump_match_traces(const secs_sml_match_trace_t *traces, size_t n) {
    if (!traces || n == 0) {
        printf("  (no traces)\n");
        return;
    }
    for (size_t i = 0; i < n; ++i) {
        const secs_sml_match_trace_t *t = &traces[i];
        printf("  - rule[%" PRIu64 "] if (%s",
               (uint64_t)t->rule_index,
               (t->condition_message_name ? t->condition_message_name : "(null)"));
        if (t->has_list_index) {
            printf("[%zu]", t->list_index);
        }
        if (t->has_index) {
            printf("(%zu)", t->index);
        }
        printf(" == <expected>) failed: reason=%d detail=%s\n",
               t->reason,
               (t->detail ? t->detail : "(null)"));
    }
}

/* ========== HSMS open：同进程 active/passive 并行打开 ========== */

struct open_args {
    secs_hsms_session_t *sess;
    secs_hsms_connection_t **io_conn;
    secs_error_t out_err;
};

static int open_passive_thread(void *p) {
    struct open_args *args = (struct open_args *)p;
    args->out_err = secs_hsms_session_open_passive_connection(args->sess,
                                                              args->io_conn);
    return 0;
}

/* ========== server handler：用 SML 条件 + ctx 注入自动回包 ========== */

struct server_state {
    const secs_sml_runtime_t *rt;
    struct device_data data;
};

static secs_error_t server_s6f11_handler(void *user_data,
                                        const secs_data_message_view_t *req,
                                        uint8_t **out_body,
                                        size_t *out_body_n) {
    struct server_state *st = (struct server_state *)user_data;
    *out_body = NULL;
    *out_body_n = 0;

    printf("[Equipment] recv S%uF%u W=%d SB=0x%08" PRIX32 " body=%zu\n",
           req->stream,
           req->function,
           req->w_bit,
           req->system_bytes,
           req->body_n);

    if (!req->w_bit) {
        /* W=0：无需回包，返回 OK 即可。 */
        return ok();
    }

    uint16_t dataid = 0;
    uint16_t ceid = 0;
    if (!decode_u2_at_list_index(req->body, req->body_n, 0, &dataid) ||
        !decode_u2_at_list_index(req->body, req->body_n, 1, &ceid)) {
        fprintf(stderr, "[Equipment] invalid S6F11 body\n");
        return invalid_argument();
    }

    printf("[Equipment] DATAID=%u CEID=0x%04X\n",
           (unsigned)dataid,
           (unsigned)ceid);

    /* 1) 构造 RenderContext（用于响应模板渲染 + 条件期望值占位符渲染） */
    secs_sml_render_context_t *ctx = NULL;
    if (!ensure_ok("secs_sml_render_context_create",
                   secs_sml_render_context_create(&ctx))) {
        return invalid_argument();
    }
    if (!fill_context_for_ceid(ceid, dataid, &st->data, ctx)) {
        secs_sml_render_context_destroy(ctx);
        return invalid_argument();
    }

    /* 2) 仅用于演示：如果 ctx 缺失占位符变量，可用 trace 定位失败原因。
     * - 本示例第一条规则的期望 CEID 使用了占位符 EXPECTED_CEID_STATUS；
     * - 因此这里用 ctx=NULL 做一次 trace，展示“缺失变量”时的 detail（不影响真实匹配）。
     */
    static int trace_demo_printed = 0;
    if (!trace_demo_printed) {
        trace_demo_printed = 1;
        char *demo_name = NULL;
        secs_sml_match_trace_t *traces = NULL;
        size_t trace_n = 0;
        if (ensure_ok("secs_sml_runtime_match_response_with_trace(demo)",
                      secs_sml_runtime_match_response_with_trace(
                          st->rt,
                          req->stream,
                          req->function,
                          req->body,
                          req->body_n,
                          NULL,
                          &demo_name,
                          &traces,
                          &trace_n))) {
            if (!demo_name) {
                printf("[Equipment][Debug] match_response_with_trace() demo "
                       "(missing EXPECTED_CEID_STATUS):\n");
                dump_match_traces(traces, trace_n);
                printf("[Equipment][Debug] end demo\n\n");
            } else {
                secs_free(demo_name);
            }
        }
        secs_sml_match_traces_free(traces, trace_n);
    }

    /* 3) 使用 match_response_with_context：支持期望值占位符（==<U2 IDENT>） */
    char *matched = NULL;
    secs_error_t m = secs_sml_runtime_match_response_with_context(st->rt,
                                                                  req->stream,
                                                                  req->function,
                                                                  req->body,
                                                                  req->body_n,
                                                                  ctx,
                                                                  &matched);
    if (!secs_error_is_ok(m)) {
        ensure_ok("secs_sml_runtime_match_response_with_context", m);
        secs_sml_render_context_destroy(ctx);
        if (matched) {
            secs_free(matched);
        }
        return m;
    }

    if (!matched) {
        /* 未命中：输出 trace 便于调试（例如索引越界/期望值不等/缺失变量等）。 */
        fprintf(stderr, "[Equipment] no matching response\n");
        char *out_name = NULL;
        secs_sml_match_trace_t *traces = NULL;
        size_t trace_n = 0;
        if (ensure_ok("secs_sml_runtime_match_response_with_trace",
                      secs_sml_runtime_match_response_with_trace(
                          st->rt,
                          req->stream,
                          req->function,
                          req->body,
                          req->body_n,
                          ctx,
                          &out_name,
                          &traces,
                          &trace_n))) {
            dump_match_traces(traces, trace_n);
            if (out_name) {
                secs_free(out_name);
            }
        }
        secs_sml_match_traces_free(traces, trace_n);
        secs_sml_render_context_destroy(ctx);
        return invalid_argument();
    }

    printf("[Equipment] matched response: %s\n", matched);

    /* 4) 渲染并编码响应模板（encode_message_body 会使用 ctx 注入变量） */
    uint8_t *rsp_body = NULL;
    size_t rsp_body_n = 0;
    uint8_t rsp_stream = 0;
    uint8_t rsp_function = 0;
    int rsp_w = 0;

    secs_error_t e = secs_sml_runtime_encode_message_body(st->rt,
                                                         matched,
                                                         ctx,
                                                         &rsp_body,
                                                         &rsp_body_n,
                                                         &rsp_stream,
                                                         &rsp_function,
                                                         &rsp_w);
    secs_sml_render_context_destroy(ctx);
    secs_free(matched);

    if (!secs_error_is_ok(e)) {
        ensure_ok("secs_sml_runtime_encode_message_body", e);
        secs_free(rsp_body);
        return e;
    }

    /* 额外校验：确保模板声明的 (S,F,W) 与协议层期望一致（S6F12 W=0）。 */
    uint8_t expected_function = (uint8_t)(req->function + 1u);
    if (rsp_stream != req->stream || rsp_function != expected_function ||
        rsp_w != 0) {
        fprintf(stderr,
                "[Equipment] response SF/W mismatch: expected S%uF%u W=0 but got "
                "S%uF%u W=%d\n",
                req->stream,
                expected_function,
                rsp_stream,
                rsp_function,
                rsp_w);
        secs_free(rsp_body);
        return invalid_argument();
    }

    *out_body = rsp_body;   /* encode_message_body 返回的内存由库分配（secs_malloc） */
    *out_body_n = rsp_body_n;
    printf("[Equipment] reply ready: body=%zu bytes\n\n", rsp_body_n);
    return ok();
}

int main(void) {
    printf("=== C API SML + CEID Complete Example ===\n\n");
    printf("secs version: %s\n\n", secs_version_string());
    (void)secs_log_set_level(SECS_LOG_INFO);

    int exit_code = 1;

    secs_context_t *ctx = NULL;
    secs_hsms_connection_t *client_conn = NULL;
    secs_hsms_connection_t *server_conn = NULL;
    secs_hsms_session_t *client_hsms = NULL;
    secs_hsms_session_t *server_hsms = NULL;
    secs_protocol_session_t *client_proto = NULL;
    secs_protocol_session_t *server_proto = NULL;

    secs_sml_runtime_t *rt = NULL;
    char *sml_src = NULL;
    size_t sml_n = 0;

    if (!ensure_ok("secs_context_create", secs_context_create(&ctx))) {
        goto cleanup;
    }

    if (!ensure_ok("secs_hsms_connection_create_memory_duplex",
                   secs_hsms_connection_create_memory_duplex(
                       ctx, &client_conn, &server_conn))) {
        goto cleanup;
    }

    /* HSMS options：此处使用与其他示例一致的默认超时配置。 */
    secs_hsms_session_options_t hsms_opt;
    memset(&hsms_opt, 0, sizeof(hsms_opt));
    hsms_opt.session_id = 0x0001;
    hsms_opt.t3_ms = 3000;
    hsms_opt.t5_ms = 200;
    hsms_opt.t6_ms = 3000;
    hsms_opt.t7_ms = 3000;
    hsms_opt.t8_ms = 3000;
    hsms_opt.linktest_interval_ms = 0;
    hsms_opt.auto_reconnect = 0;
    hsms_opt.passive_accept_select = 1;

    if (!ensure_ok("secs_hsms_session_create(client)",
                   secs_hsms_session_create(ctx, &hsms_opt, &client_hsms))) {
        goto cleanup;
    }
    if (!ensure_ok("secs_hsms_session_create(server)",
                   secs_hsms_session_create(ctx, &hsms_opt, &server_hsms))) {
        goto cleanup;
    }

    /* 同进程内 active/passive 打开连接：用 C11 线程并行执行。 */
    thrd_t th;
    struct open_args args;
    memset(&args, 0, sizeof(args));
    args.sess = server_hsms;
    args.io_conn = &server_conn;

    if (thrd_create(&th, open_passive_thread, &args) != thrd_success) {
        fprintf(stderr, "[失败] thrd_create(open_passive_thread)\n");
        goto cleanup;
    }

    if (!ensure_ok("secs_hsms_session_open_active_connection",
                   secs_hsms_session_open_active_connection(
                       client_hsms, &client_conn))) {
        (void)thrd_join(th, NULL);
        goto cleanup;
    }

    (void)thrd_join(th, NULL);
    if (!ensure_ok("secs_hsms_session_open_passive_connection", args.out_err)) {
        goto cleanup;
    }

    secs_protocol_session_options_t proto_opt;
    memset(&proto_opt, 0, sizeof(proto_opt));
    proto_opt.t3_ms = 3000;
    proto_opt.poll_interval_ms = 5;

    if (!ensure_ok("secs_protocol_session_create_from_hsms(client)",
                   secs_protocol_session_create_from_hsms(ctx,
                                                          client_hsms,
                                                          hsms_opt.session_id,
                                                          &proto_opt,
                                                          &client_proto))) {
        goto cleanup;
    }
    if (!ensure_ok("secs_protocol_session_create_from_hsms(server)",
                   secs_protocol_session_create_from_hsms(ctx,
                                                          server_hsms,
                                                          hsms_opt.session_id,
                                                          &proto_opt,
                                                          &server_proto))) {
        goto cleanup;
    }

    /* 加载 SML：与 C++ 版本共用同一份 sml_ceid_complete.sml */
    if (!read_file_all("sml_ceid_complete.sml", &sml_src, &sml_n)) {
        fprintf(stderr,
                "ERROR: Cannot open sml_ceid_complete.sml\n"
                "Please ensure the .sml file is in the same directory as the executable.\n");
        goto cleanup;
    }

    if (!ensure_ok("secs_sml_runtime_create", secs_sml_runtime_create(&rt))) {
        goto cleanup;
    }
    if (!ensure_ok("secs_sml_runtime_load",
                   secs_sml_runtime_load(rt, sml_src, sml_n))) {
        goto cleanup;
    }
    printf("SML loaded successfully.\n\n");

    /* server：注册 S6F11 handler（基于 SML + ctx 自动回包） */
    struct server_state st;
    memset(&st, 0, sizeof(st));
    st.rt = rt;
    st.data.device_name = "EQUIPMENT-001";
    st.data.status_code = 1;
    st.data.uptime_seconds = 12345;
    st.data.temp1 = 25.5f;
    st.data.temp2 = 26.3f;
    st.data.temp3 = 24.8f;
    st.data.alarm_count = 2;
    st.data.alarm_msg_1 = "High temperature warning";
    st.data.alarm_msg_2 = "Low pressure alert";
    st.data.total_count = 10000;
    st.data.good_count = 9850;
    st.data.bad_count = 150;

    if (!ensure_ok("secs_protocol_session_set_handler(server S6F11)",
                   secs_protocol_session_set_handler(
                       server_proto, 6, 11, server_s6f11_handler, &st))) {
        goto cleanup;
    }

    /* client：发送多条不同 CEID 的 S6F11 请求 */
    const uint16_t ceids[] = {0x1001u, 0x1002u, 0x1003u, 0x1004u};
    int failures = 0;
    for (size_t i = 0; i < sizeof(ceids) / sizeof(ceids[0]); ++i) {
        uint16_t dataid = (uint16_t)(i + 1u);
        uint16_t ceid = ceids[i];

        printf("=== Test %zu: CEID=0x%04X ===\n", i + 1u, (unsigned)ceid);

        uint8_t *req_body = NULL;
        size_t req_body_n = 0;
        if (!encode_s6f11_body(dataid, ceid, &req_body, &req_body_n)) {
            ++failures;
            secs_free(req_body);
            continue;
        }

        printf("[Host] request S6F11(W=1), DATAID=%u CEID=0x%04X body=%zu\n",
               (unsigned)dataid,
               (unsigned)ceid,
               req_body_n);

        secs_data_message_t reply;
        memset(&reply, 0, sizeof(reply));
        secs_error_t r = secs_protocol_session_request(client_proto,
                                                       6,
                                                       11,
                                                       req_body,
                                                       req_body_n,
                                                       3000,
                                                       &reply);
        secs_free(req_body);

        if (!ensure_ok("secs_protocol_session_request", r)) {
            ++failures;
            secs_data_message_free(&reply);
            continue;
        }

        printf("[Host] got reply: S%uF%u W=%d SB=0x%08" PRIX32 " body=%zu\n",
               reply.stream,
               reply.function,
               reply.w_bit,
               reply.system_bytes,
               reply.body_n);

        if (reply.stream != 6u || reply.function != 12u || reply.w_bit != 0) {
            fprintf(stderr,
                    "[Host] ERROR: Expected S6F12 W=0 but got S%uF%u W=%d\n",
                    reply.stream,
                    reply.function,
                    reply.w_bit);
            ++failures;
        }

        /* 轻量校验：<L <U2 DATAID> <U2 CEID> <...>> */
        {
            uint16_t got_dataid = 0;
            uint16_t got_ceid = 0;
            if (!decode_u2_at_list_index(reply.body, reply.body_n, 0, &got_dataid) ||
                !decode_u2_at_list_index(reply.body, reply.body_n, 1, &got_ceid)) {
                fprintf(stderr, "[Host] ERROR: cannot extract DATAID/CEID from reply\n");
                ++failures;
            } else {
                if (got_dataid != dataid) {
                    fprintf(stderr,
                            "[Host] ERROR: DATAID mismatch: expected=%u got=%u\n",
                            (unsigned)dataid,
                            (unsigned)got_dataid);
                    ++failures;
                }
                if (got_ceid != ceid) {
                    fprintf(stderr,
                            "[Host] ERROR: CEID mismatch: expected=0x%04X got=0x%04X\n",
                            (unsigned)ceid,
                            (unsigned)got_ceid);
                    ++failures;
                }
            }
        }

        /* 打印解码内容（演示） */
        if (reply.body && reply.body_n != 0) {
            size_t consumed = 0;
            secs_ii_item_t *decoded = NULL;
            if (ensure_ok("secs_ii_decode_one(reply)",
                          secs_ii_decode_one(reply.body,
                                             reply.body_n,
                                             &consumed,
                                             &decoded))) {
                printf("[Host] Response body:\n");
                print_item(decoded, 1);
                secs_ii_item_destroy(decoded);
            } else {
                ++failures;
            }
        }
        printf("\n");
        secs_data_message_free(&reply);
    }

    printf("=== All tests completed ===\n");
    if (failures != 0) {
        printf("Failures: %d\n", failures);
        exit_code = 1;
    } else {
        exit_code = 0;
    }

cleanup:
    secs_free(sml_src);
    if (client_proto) {
        (void)secs_protocol_session_stop(client_proto);
        secs_protocol_session_destroy(client_proto);
    }
    if (server_proto) {
        (void)secs_protocol_session_stop(server_proto);
        secs_protocol_session_destroy(server_proto);
    }
    if (client_hsms) {
        (void)secs_hsms_session_stop(client_hsms);
        secs_hsms_session_destroy(client_hsms);
    }
    if (server_hsms) {
        (void)secs_hsms_session_stop(server_hsms);
        secs_hsms_session_destroy(server_hsms);
    }
    secs_hsms_connection_destroy(client_conn);
    secs_hsms_connection_destroy(server_conn);
    if (rt) {
        secs_sml_runtime_destroy(rt);
    }
    if (ctx) {
        secs_context_destroy(ctx);
    }
    return exit_code;
}

