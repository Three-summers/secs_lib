/*
 * C API（C ABI）示例：CEID（仿 TVOC_Secs_App 的消息体结构）
 *
 * 目标：
 * - 不引入 GEM(E30)，仅按厂商文档“消息体携带 CEID”的形式做最小处理层：
 *   1) 入站：解码 body -> 提取 CEID -> 按 CEID 分发 handler
 *   2) 出站：request/reply 都带 CEID 时，可选校验一致性
 *
 * TVOC 现有实现参考（仅对齐“消息体形式”）：
 * - `/home/say/code_bak/tvoc_code/src/TVOC_App/TVOC_Secs_App/tvoc_secs_app.c`
 *   中的 S6F11 体结构形如：
 *   <L[3] <U2 DATAID> <U2 CEID> <...>>
 * - tvoc_secs_app.h：#define CEID 0x5000（U2）
 *
 * 本示例的要点：
 * - server（Equipment）侧使用：
 *   - secs_ceid_dispatcher_create_list_path(indices={1})：从 <L ... <CEID> ...> 提取 CEID
 *   - secs_protocol_session_set_ceid_dispatcher(server, 6, 11, disp)：挂到 S6F11
 * - client（Host）侧使用：
 *   - secs_protocol_session_request_with_ceid_list_path(... verify_equal=1 ...)：
 *     发送 S6F11(W=1) 并校验 reply 中提取出的 CEID 与 request 一致
 *
 * 说明：
 * - 为了演示“请求/响应都带 CEID”的场景，这里让 S6F11 使用 W=1，并返回 S6F12。
 * - 若你的厂商协议 secondary 不携带 CEID：把 verify_equal 改成 0 即可。
 *
 * 用法：
 *   ./c_api_ceid_tvoc_style
 *
 * 备注：
 * - 该示例使用 `secs_hsms_connection_create_memory_duplex()`，不依赖 socket；
 * - 需要 pthread（因此在 CMake 中仅 UNIX 构建）。
 */

#include <secs/c_api.h>

#include <inttypes.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

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

struct open_args {
    secs_hsms_session_t *sess;
    secs_hsms_connection_t **io_conn;
    secs_error_t out_err;
};

static void *open_passive_thread(void *p) {
    struct open_args *args = (struct open_args *)p;
    args->out_err = secs_hsms_session_open_passive_connection(args->sess,
                                                              args->io_conn);
    return NULL;
}

static secs_error_t encode_tvoc_like_s6f11_body(uint16_t dataid,
                                                uint16_t ceid_u2,
                                                uint8_t **out_body,
                                                size_t *out_body_n) {
    *out_body = NULL;
    *out_body_n = 0;

    secs_error_t err;
    secs_ii_item_t *root = NULL;
    secs_ii_item_t *item = NULL;
    uint8_t *bytes = NULL;
    size_t bytes_n = 0;

    const char *desc = "valve1: PID|FM|PM|PE MAX|AVG";
    const char *vals[] = {"12.34", "56.78", "90.12", "34.56",
                          "78.90", "12.30", "45.60", "78.00"};

    if (!secs_error_is_ok(err = secs_ii_item_create_list(&root))) {
        goto cleanup;
    }

    /* <U2 DATAID> */
    if (!secs_error_is_ok(err = secs_ii_item_create_u2(&dataid, 1, &item))) {
        goto cleanup;
    }
    if (!secs_error_is_ok(err = secs_ii_item_list_append(root, item))) {
        goto cleanup;
    }
    secs_ii_item_destroy(item);
    item = NULL;

    /* <U2 CEID> */
    if (!secs_error_is_ok(err = secs_ii_item_create_u2(&ceid_u2, 1, &item))) {
        goto cleanup;
    }
    if (!secs_error_is_ok(err = secs_ii_item_list_append(root, item))) {
        goto cleanup;
    }
    secs_ii_item_destroy(item);
    item = NULL;

    /* <L <A desc> <L <A v1> ... >> */
    secs_ii_item_t *report = NULL;
    secs_ii_item_t *values = NULL;

    if (!secs_error_is_ok(err = secs_ii_item_create_list(&report))) {
        goto cleanup;
    }
    if (!secs_error_is_ok(
            err = secs_ii_item_create_ascii(desc, strlen(desc), &item))) {
        goto cleanup;
    }
    if (!secs_error_is_ok(err = secs_ii_item_list_append(report, item))) {
        goto cleanup;
    }
    secs_ii_item_destroy(item);
    item = NULL;

    if (!secs_error_is_ok(err = secs_ii_item_create_list(&values))) {
        goto cleanup;
    }
    for (size_t i = 0; i < sizeof(vals) / sizeof(vals[0]); ++i) {
        if (!secs_error_is_ok(err = secs_ii_item_create_ascii(vals[i],
                                                             strlen(vals[i]),
                                                             &item))) {
            goto cleanup;
        }
        if (!secs_error_is_ok(err = secs_ii_item_list_append(values, item))) {
            goto cleanup;
        }
        secs_ii_item_destroy(item);
        item = NULL;
    }

    if (!secs_error_is_ok(err = secs_ii_item_list_append(report, values))) {
        goto cleanup;
    }
    secs_ii_item_destroy(values);
    values = NULL;

    if (!secs_error_is_ok(err = secs_ii_item_list_append(root, report))) {
        goto cleanup;
    }
    secs_ii_item_destroy(report);
    report = NULL;

    if (!secs_error_is_ok(err = secs_ii_encode(root, &bytes, &bytes_n))) {
        goto cleanup;
    }

    *out_body = bytes;
    *out_body_n = bytes_n;
    bytes = NULL;
    bytes_n = 0;

cleanup:
    secs_ii_item_destroy(item);
    secs_ii_item_destroy(root);
    secs_free(bytes);
    return err;
}

static uint16_t try_extract_u2_at_list_index(const uint8_t *body,
                                             size_t body_n,
                                             size_t index,
                                             uint16_t fallback) {
    if (!body || body_n == 0) {
        return fallback;
    }

    size_t consumed = 0;
    secs_ii_item_t *root = NULL;
    if (!ensure_ok("secs_ii_decode_one",
                   secs_ii_decode_one(body, body_n, &consumed, &root))) {
        secs_ii_item_destroy(root);
        return fallback;
    }

    secs_ii_item_t *child = NULL;
    if (!ensure_ok("secs_ii_item_list_get",
                   secs_ii_item_list_get(root, index, &child))) {
        secs_ii_item_destroy(root);
        secs_ii_item_destroy(child);
        return fallback;
    }

    const uint16_t *p = NULL;
    size_t n = 0;
    uint16_t out = fallback;
    if (ensure_ok("secs_ii_item_u2_view", secs_ii_item_u2_view(child, &p, &n))) {
        if (p && n == 1) {
            out = p[0];
        }
    }

    secs_ii_item_destroy(child);
    secs_ii_item_destroy(root);
    return out;
}

static secs_error_t encode_simple_ack_body(uint16_t dataid,
                                           uint16_t ceid_u2,
                                           const char *text,
                                           uint8_t **out_body,
                                           size_t *out_body_n) {
    *out_body = NULL;
    *out_body_n = 0;

    secs_error_t err;
    secs_ii_item_t *root = NULL;
    secs_ii_item_t *item = NULL;
    uint8_t *bytes = NULL;
    size_t bytes_n = 0;

    if (!secs_error_is_ok(err = secs_ii_item_create_list(&root))) {
        goto cleanup;
    }
    if (!secs_error_is_ok(err = secs_ii_item_create_u2(&dataid, 1, &item))) {
        goto cleanup;
    }
    if (!secs_error_is_ok(err = secs_ii_item_list_append(root, item))) {
        goto cleanup;
    }
    secs_ii_item_destroy(item);
    item = NULL;

    if (!secs_error_is_ok(err = secs_ii_item_create_u2(&ceid_u2, 1, &item))) {
        goto cleanup;
    }
    if (!secs_error_is_ok(err = secs_ii_item_list_append(root, item))) {
        goto cleanup;
    }
    secs_ii_item_destroy(item);
    item = NULL;

    if (!secs_error_is_ok(
            err = secs_ii_item_create_ascii(text, strlen(text), &item))) {
        goto cleanup;
    }
    if (!secs_error_is_ok(err = secs_ii_item_list_append(root, item))) {
        goto cleanup;
    }
    secs_ii_item_destroy(item);
    item = NULL;

    if (!secs_error_is_ok(err = secs_ii_encode(root, &bytes, &bytes_n))) {
        goto cleanup;
    }

    *out_body = bytes;
    *out_body_n = bytes_n;
    bytes = NULL;
    bytes_n = 0;

cleanup:
    secs_ii_item_destroy(item);
    secs_ii_item_destroy(root);
    secs_free(bytes);
    return err;
}

static secs_error_t server_ceid_handler(void *user_data,
                                        uint32_t ceid,
                                        const secs_data_message_view_t *req,
                                        uint8_t **out_body,
                                        size_t *out_body_n) {
    (void)user_data;

    const uint16_t dataid =
        try_extract_u2_at_list_index(req->body, req->body_n, 0, 0);

    printf("[server][CEID handler] recv S%uF%u W=%d CEID=0x%04" PRIX32
           " DATAID=%u body=%zu\n",
           req->stream,
           req->function,
           req->w_bit,
           ceid,
           (unsigned)dataid,
           req->body_n);

    /* 回包体也携带同 CEID，便于 client 侧 verify_equal。 */
    return encode_simple_ack_body(dataid,
                                  (uint16_t)ceid,
                                  "ACK(from c_api_ceid_tvoc_style)",
                                  out_body,
                                  out_body_n);
}

int main(void) {
    const uint16_t kSessionId = 0x0001;
    const uint16_t kCeid = 0x5000; /* 对齐 tvoc_secs_app.h */

    printf("=== C API CEID 示例（tvoc style）===\n\n");
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
    secs_ceid_dispatcher_t *disp = NULL;

    uint8_t *req_body = NULL;
    size_t req_body_n = 0;

    if (!ensure_ok("secs_context_create", secs_context_create(&ctx))) {
        goto cleanup;
    }

    if (!ensure_ok("secs_hsms_connection_create_memory_duplex",
                   secs_hsms_connection_create_memory_duplex(
                       ctx, &client_conn, &server_conn))) {
        goto cleanup;
    }

    secs_hsms_session_options_t hsms_opt;
    memset(&hsms_opt, 0, sizeof(hsms_opt));
    hsms_opt.session_id = kSessionId;
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

    pthread_t th;
    struct open_args args;
    memset(&args, 0, sizeof(args));
    args.sess = server_hsms;
    args.io_conn = &server_conn;
    if (pthread_create(&th, NULL, open_passive_thread, &args) != 0) {
        fprintf(stderr, "[失败] pthread_create\n");
        goto cleanup;
    }

    if (!ensure_ok("secs_hsms_session_open_active_connection",
                   secs_hsms_session_open_active_connection(
                       client_hsms, &client_conn))) {
        (void)pthread_join(th, NULL);
        goto cleanup;
    }
    (void)pthread_join(th, NULL);
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
                                                          kSessionId,
                                                          &proto_opt,
                                                          &client_proto))) {
        goto cleanup;
    }
    if (!ensure_ok("secs_protocol_session_create_from_hsms(server)",
                   secs_protocol_session_create_from_hsms(ctx,
                                                          server_hsms,
                                                          kSessionId,
                                                          &proto_opt,
                                                          &server_proto))) {
        goto cleanup;
    }

    /* server：创建并挂载 CEID dispatcher（CEID 在 list 的 index=1） */
    size_t ceid_path[] = {1};
    if (!ensure_ok("secs_ceid_dispatcher_create_list_path",
                   secs_ceid_dispatcher_create_list_path(
                       ceid_path, 1, NULL, 1, &disp))) {
        goto cleanup;
    }
    if (!ensure_ok("secs_ceid_dispatcher_set_handler(0x5000)",
                   secs_ceid_dispatcher_set_handler(
                       disp, kCeid, server_ceid_handler, NULL))) {
        goto cleanup;
    }
    if (!ensure_ok("secs_protocol_session_set_ceid_dispatcher(S6F11)",
                   secs_protocol_session_set_ceid_dispatcher(
                       server_proto, 6, 11, disp))) {
        goto cleanup;
    }

    /* client：构造 tvoc-like 的 S6F11 body，并按 list path 提取/校验 CEID */
    const uint16_t dataid = 1;
    if (!ensure_ok("encode_tvoc_like_s6f11_body",
                   encode_tvoc_like_s6f11_body(
                       dataid, kCeid, &req_body, &req_body_n))) {
        goto cleanup;
    }

    printf("[client] request S6F11(W=1), DATAID=%u CEID=0x%04X body=%zu\n",
           (unsigned)dataid,
           (unsigned)kCeid,
           req_body_n);

    secs_data_message_t reply;
    memset(&reply, 0, sizeof(reply));

    int has_req_ceid = 0;
    uint32_t out_req_ceid = 0;
    int has_rsp_ceid = 0;
    uint32_t out_rsp_ceid = 0;

    /* verify_equal=1：要求 reply 中也能提取出 CEID，且与 request 一致 */
    secs_error_t r = secs_protocol_session_request_with_ceid_list_path(
        client_proto,
        6,
        11,
        req_body,
        req_body_n,
        3000,
        ceid_path,
        1,
        NULL,
        1,
        &reply,
        &has_req_ceid,
        &out_req_ceid,
        &has_rsp_ceid,
        &out_rsp_ceid);

    if (!ensure_ok("secs_protocol_session_request_with_ceid_list_path", r)) {
        secs_data_message_free(&reply);
        goto cleanup;
    }

    printf("[client] got reply: S%uF%u W=%d SB=0x%08" PRIX32 " body=%zu\n",
           reply.stream,
           reply.function,
           reply.w_bit,
           reply.system_bytes,
           reply.body_n);

    printf("[client] request_ceid: %s 0x%04" PRIX32 "\n",
           (has_req_ceid ? "present" : "absent"),
           out_req_ceid);
    printf("[client] reply_ceid:   %s 0x%04" PRIX32 "\n",
           (has_rsp_ceid ? "present" : "absent"),
           out_rsp_ceid);

    secs_data_message_free(&reply);

    exit_code = 0;

cleanup:
    secs_free(req_body);
    if (disp) {
        secs_ceid_dispatcher_destroy(disp);
    }
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
    if (ctx) {
        secs_context_destroy(ctx);
    }
    return exit_code;
}

