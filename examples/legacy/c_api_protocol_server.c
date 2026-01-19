/*
 * C API（C ABI）示例：协议层（Protocol Session）+ HSMS（TCP）服务器
 *
 * 目标：
 * - 演示“用户层只写 handler”的用法（避免大量 SxFy 写一堆 glue 代码）；
 * - 演示 default handler：未注册的 (S,F) 统一走一个回调；
 * - 演示“双方都可以主动发送 primary”：server 在收到 client 的 S1F1 后，
 *   反向向 client 发起一次 S2F1 request。
 *
 * 用法：
 *   ./c_api_protocol_server [ip] [port]
 *
 * 示例：
 *   ./c_api_protocol_server 127.0.0.1 5001
 *   ./c_api_protocol_client 127.0.0.1 5001
 */

#include <secs/c_api.h>

#include <inttypes.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <time.h>
#endif

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

static void sleep_ms(unsigned ms) {
#if defined(_WIN32)
    Sleep(ms);
#else
    struct timespec req;
    req.tv_sec = (time_t)(ms / 1000u);
    req.tv_nsec = (long)((ms % 1000u) * 1000000ul);
    (void)nanosleep(&req, NULL);
#endif
}

static int wait_until_atomic_eq(const atomic_int *v,
                                int expected,
                                int max_tries,
                                unsigned sleep_each_ms) {
    for (int i = 0; i < max_tries; ++i) {
        if (atomic_load(v) == expected) {
            return 1;
        }
        sleep_ms(sleep_each_ms);
    }
    return atomic_load(v) == expected;
}

static void dump_hex_prefix(const uint8_t *p, size_t n, size_t max) {
    if (!p || n == 0) {
        printf("(empty)\n");
        return;
    }

    const size_t k = (n < max ? n : max);
    for (size_t i = 0; i < k; ++i) {
        printf("%02X", (unsigned)p[i]);
        if (i + 1 != k) {
            putchar(' ');
        }
    }
    if (n > max) {
        printf(" ...");
    }
    putchar('\n');
}

static void try_dump_secs2_ascii(const uint8_t *body, size_t body_n) {
    if (!body || body_n == 0) {
        return;
    }

    size_t consumed = 0;
    secs_ii_item_t *item = NULL;
    if (!ensure_ok("secs_ii_decode_one",
                   secs_ii_decode_one(body, body_n, &consumed, &item))) {
        secs_ii_item_destroy(item);
        return;
    }

    secs_ii_item_type_t ty;
    if (!ensure_ok("secs_ii_item_get_type", secs_ii_item_get_type(item, &ty))) {
        secs_ii_item_destroy(item);
        return;
    }

    if (ty == SECS_II_ITEM_ASCII) {
        const char *p = NULL;
        size_t n = 0;
        if (ensure_ok("secs_ii_item_ascii_view",
                      secs_ii_item_ascii_view(item, &p, &n))) {
            printf("  [SECS-II] ASCII(%zu): ", n);
            if (p && n != 0) {
                printf("%.*s\n", (int)n, p);
            } else {
                printf("(empty)\n");
            }
        }
    } else {
        printf("  [SECS-II] item_type=%d (consumed=%zu/%zu)\n",
               (int)ty,
               consumed,
               body_n);
    }

    secs_ii_item_destroy(item);
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
    return err; /* OK */
}

struct server_state {
    atomic_int client_ready;
};

static secs_error_t server_default_handler(void *user_data,
                                           const secs_data_message_view_t *req,
                                           uint8_t **out_body,
                                           size_t *out_body_n) {
    struct server_state *st = (struct server_state *)user_data;

    printf("[server][handler] recv S%uF%u W=%d SB=0x%08" PRIX32 " body=%zu\n",
           req->stream,
           req->function,
           req->w_bit,
           req->system_bytes,
           req->body_n);
    if (req->body_n != 0) {
        printf("  [raw] bytes=%zu prefix=", req->body_n);
        dump_hex_prefix(req->body, req->body_n, 32);
        try_dump_secs2_ascii(req->body, req->body_n);
    }

    if (req->stream == 1u && req->function == 1u) {
        atomic_store(&st->client_ready, 1);
        return encode_ascii_body("S1F2 OK(from c_api_protocol_server)",
                                 out_body,
                                 out_body_n);
    }

    if (req->stream == 2u && req->function == 1u) {
        return encode_ascii_body("S2F2 OK(from c_api_protocol_server)",
                                 out_body,
                                 out_body_n);
    }

    return encode_ascii_body("UNHANDLED(from c_api_protocol_server)",
                             out_body,
                             out_body_n);
}

int main(int argc, char **argv) {
    const char *ip = "127.0.0.1";
    uint16_t port = 5001;
    if (argc >= 2) {
        ip = argv[1];
    }
    if (argc >= 3) {
        port = (uint16_t)atoi(argv[2]);
    }

    printf("=== C API 协议层（HSMS TCP）服务器示例 ===\n\n");
    printf("secs version: %s\n", secs_version_string());
    printf("listen: %s:%u\n\n", ip, (unsigned)port);

    (void)secs_log_set_level(SECS_LOG_DEBUG);

    int exit_code = 1;
    secs_context_t *ctx = NULL;
    secs_hsms_session_t *hsms = NULL;
    secs_protocol_session_t *proto = NULL;
    secs_data_message_t reply;
    memset(&reply, 0, sizeof(reply));

    struct server_state st;
    atomic_init(&st.client_ready, 0);

    if (!ensure_ok("secs_context_create", secs_context_create(&ctx))) {
        goto cleanup;
    }

    secs_hsms_session_options_t hsms_opt;
    memset(&hsms_opt, 0, sizeof(hsms_opt));
    hsms_opt.session_id = 0x0001;
    hsms_opt.t3_ms = 45000;
    hsms_opt.t5_ms = 1000;
    hsms_opt.t6_ms = 5000;
    hsms_opt.t7_ms = 10000;
    hsms_opt.t8_ms = 5000;
    hsms_opt.linktest_interval_ms = 0;
    hsms_opt.auto_reconnect = 0;
    hsms_opt.passive_accept_select = 1;

    if (!ensure_ok("secs_hsms_session_create",
                   secs_hsms_session_create(ctx, &hsms_opt, &hsms))) {
        goto cleanup;
    }

    printf("[server] waiting for connection...\n");
    if (!ensure_ok("secs_hsms_session_open_passive_ip",
                   secs_hsms_session_open_passive_ip(hsms, ip, port))) {
        goto cleanup;
    }
    printf("[server] selected\n");

    secs_protocol_session_options_t proto_opt;
    memset(&proto_opt, 0, sizeof(proto_opt));
    proto_opt.t3_ms = 3000;
    proto_opt.poll_interval_ms = 5;

    if (!ensure_ok(
            "secs_protocol_session_create_from_hsms",
            secs_protocol_session_create_from_hsms(
                ctx, hsms, hsms_opt.session_id, &proto_opt, &proto))) {
        goto cleanup;
    }

    if (!ensure_ok("secs_protocol_session_set_default_handler",
                   secs_protocol_session_set_default_handler(
                       proto, server_default_handler, &st))) {
        goto cleanup;
    }

    printf("[server] waiting client S1F1...\n");
    if (!wait_until_atomic_eq(&st.client_ready, 1, 200, 10)) {
        fprintf(stderr, "[server] timeout: client S1F1 not received\n");
        goto cleanup;
    }

    printf("[server] client is ready; server -> client request S2F1...\n");
    {
        const char *text = "PING(from c_api_protocol_server)";
        secs_ii_item_t *item = NULL;
        uint8_t *req_body = NULL;
        size_t req_n = 0;

        if (!ensure_ok("secs_ii_item_create_ascii",
                       secs_ii_item_create_ascii(
                           text, strlen(text), &item))) {
            secs_ii_item_destroy(item);
            goto cleanup;
        }
        if (!ensure_ok("secs_ii_encode", secs_ii_encode(item, &req_body, &req_n))) {
            secs_ii_item_destroy(item);
            secs_free(req_body);
            goto cleanup;
        }
        secs_ii_item_destroy(item);

        if (!ensure_ok("secs_protocol_session_request(server->client)",
                       secs_protocol_session_request(
                           proto, 2, 1, req_body, req_n, 3000, &reply))) {
            secs_free(req_body);
            goto cleanup;
        }
        secs_free(req_body);
    }

    printf("[server] got reply: S%uF%u W=%d SB=0x%08" PRIX32 " body=%zu\n",
           reply.stream,
           reply.function,
           reply.w_bit,
           reply.system_bytes,
           reply.body_n);
    if (reply.body_n != 0) {
        printf("  [raw] bytes=%zu prefix=", reply.body_n);
        dump_hex_prefix(reply.body, reply.body_n, 32);
        try_dump_secs2_ascii(reply.body, reply.body_n);
    }

    exit_code = 0;

cleanup:
    secs_data_message_free(&reply);
    if (proto) {
        (void)secs_protocol_session_stop(proto);
        secs_protocol_session_destroy(proto);
    }
    if (hsms) {
        (void)secs_hsms_session_stop(hsms);
        secs_hsms_session_destroy(hsms);
    }
    if (ctx) {
        secs_context_destroy(ctx);
    }
    return exit_code;
}

