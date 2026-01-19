/*
 * C API（C ABI）示例：协议层（Protocol Session）回环（无 socket 环境也可运行）
 *
 * 说明：
 * - 使用 `secs_hsms_connection_create_memory_duplex()` 创建一对“内存互联”的
 *   HSMS Connection；
 * - 通过 `open_active_connection/open_passive_connection` 完成 SELECT；
 * - 在同一进程中创建 client/server 两个 protocol session，并进行双向 request。
 *
 * 适用场景：
 * - 容器/沙箱禁止 socket（例如本仓库单测环境），但仍希望验证 C API 端到端链路；
 * - 快速验证“双方都可以主动发送 primary”。
 */

#include <secs/c_api.h>

#include <inttypes.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

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

static secs_error_t encode_ascii_body(const char *text,
                                      uint8_t **out_body,
                                      size_t *out_body_n) {
    *out_body = NULL;
    *out_body_n = 0;

    secs_ii_item_t *item = NULL;
    secs_error_t err =
        secs_ii_item_create_ascii(text, strlen(text), &item);
    if (!secs_error_is_ok(err)) {
        secs_ii_item_destroy(item);
        return err;
    }

    uint8_t *bytes = NULL;
    size_t n = 0;
    err = secs_ii_encode(item, &bytes, &n);
    secs_ii_item_destroy(item);
    if (!secs_error_is_ok(err)) {
        secs_free(bytes);
        return err;
    }

    *out_body = bytes;
    *out_body_n = n;
    return err;
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

struct loop_state {
    atomic_int client_ready;
    atomic_int server_requested;
};

static secs_error_t server_default_handler(void *user_data,
                                           const secs_data_message_view_t *req,
                                           uint8_t **out_body,
                                           size_t *out_body_n) {
    struct loop_state *st = (struct loop_state *)user_data;
    if (req->stream == 1u && req->function == 1u) {
        atomic_store(&st->client_ready, 1);
        return encode_ascii_body("S1F2 OK(loopback)", out_body, out_body_n);
    }
    return encode_ascii_body("UNHANDLED(loopback)", out_body, out_body_n);
}

static secs_error_t client_s2f1_handler(void *user_data,
                                        const secs_data_message_view_t *req,
                                        uint8_t **out_body,
                                        size_t *out_body_n) {
    struct loop_state *st = (struct loop_state *)user_data;
    (void)req;
    atomic_store(&st->server_requested, 1);
    return encode_ascii_body("S2F2 OK(loopback)", out_body, out_body_n);
}

static int wait_until_atomic_eq(const atomic_int *v,
                                int expected,
                                int max_tries) {
    for (int i = 0; i < max_tries; ++i) {
        if (atomic_load(v) == expected) {
            return 1;
        }
        /* 短暂让出 CPU：避免忙等占满（测试/示例足够） */
        struct timespec req;
        req.tv_sec = 0;
        req.tv_nsec = 1000000L; /* 1ms */
        (void)nanosleep(&req, NULL);
    }
    return atomic_load(v) == expected;
}

int main(void) {
    printf("=== C API 协议层回环示例（memory duplex）===\n\n");
    printf("secs version: %s\n\n", secs_version_string());
    (void)secs_log_set_level(SECS_LOG_DEBUG);

    int exit_code = 1;
    secs_context_t *ctx = NULL;
    secs_hsms_connection_t *client_conn = NULL;
    secs_hsms_connection_t *server_conn = NULL;
    secs_hsms_session_t *client_hsms = NULL;
    secs_hsms_session_t *server_hsms = NULL;
    secs_protocol_session_t *client_proto = NULL;
    secs_protocol_session_t *server_proto = NULL;

    struct loop_state st;
    atomic_init(&st.client_ready, 0);
    atomic_init(&st.server_requested, 0);

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

    if (!ensure_ok(
            "secs_protocol_session_create_from_hsms(client)",
            secs_protocol_session_create_from_hsms(
                ctx,
                client_hsms,
                hsms_opt.session_id,
                &proto_opt,
                &client_proto))) {
        goto cleanup;
    }
    if (!ensure_ok(
            "secs_protocol_session_create_from_hsms(server)",
            secs_protocol_session_create_from_hsms(
                ctx,
                server_hsms,
                hsms_opt.session_id,
                &proto_opt,
                &server_proto))) {
        goto cleanup;
    }

    if (!ensure_ok("secs_protocol_session_set_default_handler(server)",
                   secs_protocol_session_set_default_handler(
                       server_proto, server_default_handler, &st))) {
        goto cleanup;
    }
    if (!ensure_ok("secs_protocol_session_set_handler(client S2F1)",
                   secs_protocol_session_set_handler(
                       client_proto, 2, 1, client_s2f1_handler, &st))) {
        goto cleanup;
    }

    printf("[loopback] client -> server: request S1F1\n");
    secs_data_message_t reply;
    memset(&reply, 0, sizeof(reply));
    if (!ensure_ok("secs_protocol_session_request(client->server)",
                   secs_protocol_session_request(
                       client_proto, 1, 1, NULL, 0, 3000, &reply))) {
        goto cleanup;
    }
    printf("[loopback] got reply: S%uF%u W=%d SB=0x%08" PRIX32 " body=%zu\n",
           reply.stream,
           reply.function,
           reply.w_bit,
           reply.system_bytes,
           reply.body_n);
    secs_data_message_free(&reply);

    if (!wait_until_atomic_eq(&st.client_ready, 1, 1000)) {
        fprintf(stderr, "[失败] client_ready not set\n");
        goto cleanup;
    }

    printf("[loopback] server -> client: request S2F1\n");
    memset(&reply, 0, sizeof(reply));
    if (!ensure_ok("secs_protocol_session_request(server->client)",
                   secs_protocol_session_request(
                       server_proto, 2, 1, NULL, 0, 3000, &reply))) {
        goto cleanup;
    }
    printf("[loopback] got reply: S%uF%u W=%d SB=0x%08" PRIX32 " body=%zu\n",
           reply.stream,
           reply.function,
           reply.w_bit,
           reply.system_bytes,
           reply.body_n);
    secs_data_message_free(&reply);

    if (!wait_until_atomic_eq(&st.server_requested, 1, 1000)) {
        fprintf(stderr, "[失败] server_requested not set\n");
        goto cleanup;
    }

    exit_code = 0;

cleanup:
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
