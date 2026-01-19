/*
 * C API（C ABI）示例：协议层（Protocol Session）+ HSMS（TCP）客户端
 *
 * 配套程序：c_api_protocol_server
 *
 * 演示点：
 * - client 主动发起 S1F1 request，server 通过 default handler 回 S1F2；
 * - server 也会反向发起一次 S2F1 request，client 通过 handler 回 S2F2；
 * - 证明“Host/Equipment 都可以主动发送 primary”（与 SECS 角色无关）。
 *
 * 用法：
 *   ./c_api_protocol_client [ip] [port]
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

struct client_state {
    atomic_int server_requested;
};

static secs_error_t client_s2f1_handler(void *user_data,
                                        const secs_data_message_view_t *req,
                                        uint8_t **out_body,
                                        size_t *out_body_n) {
    struct client_state *st = (struct client_state *)user_data;
    printf("[client][handler] recv S%uF%u W=%d SB=0x%08" PRIX32 " body=%zu\n",
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

    atomic_store(&st->server_requested, 1);
    return encode_ascii_body("S2F2 OK(from c_api_protocol_client)",
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

    printf("=== C API 协议层（HSMS TCP）客户端示例 ===\n\n");
    printf("secs version: %s\n", secs_version_string());
    printf("connect: %s:%u\n\n", ip, (unsigned)port);

    (void)secs_log_set_level(SECS_LOG_DEBUG);

    int exit_code = 1;
    secs_context_t *ctx = NULL;
    secs_hsms_session_t *hsms = NULL;
    secs_protocol_session_t *proto = NULL;
    secs_data_message_t reply;
    memset(&reply, 0, sizeof(reply));

    struct client_state st;
    atomic_init(&st.server_requested, 0);

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

    printf("[client] connecting...\n");
    if (!ensure_ok("secs_hsms_session_open_active_ip",
                   secs_hsms_session_open_active_ip(hsms, ip, port))) {
        goto cleanup;
    }
    printf("[client] selected\n");

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

    if (!ensure_ok("secs_protocol_session_set_handler(S2F1)",
                   secs_protocol_session_set_handler(
                       proto, 2, 1, client_s2f1_handler, &st))) {
        goto cleanup;
    }

    printf("[client] request: S1F1 -> server\n");
    {
        const char *text = "HELLO(from c_api_protocol_client)";
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

        if (!ensure_ok("secs_protocol_session_request(client->server)",
                       secs_protocol_session_request(
                           proto, 1, 1, req_body, req_n, 3000, &reply))) {
            secs_free(req_body);
            goto cleanup;
        }
        secs_free(req_body);
    }

    printf("[client] got reply: S%uF%u W=%d SB=0x%08" PRIX32 " body=%zu\n",
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
    secs_data_message_free(&reply);
    memset(&reply, 0, sizeof(reply));

    printf("[client] waiting server->client S2F1...\n");
    if (!wait_until_atomic_eq(&st.server_requested, 1, 300, 10)) {
        fprintf(stderr, "[client] timeout: server->client request not received\n");
        goto cleanup;
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

