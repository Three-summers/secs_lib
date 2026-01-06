/*
 * C API（C ABI）示例：HSMS（TCP）客户端
 *
 * 特点：
 * - 纯 C 代码：#include <secs/c_api.h>
 * - 通过 `secs_hsms_session_open_active_ip()` 连接并 SELECT（阻塞式）
 * - 发送 1 次 request（W=1），并等待服务器响应
 *
 * 用法：
 *   ./c_api_hsms_client [ip] [port]
 *
 * 示例：
 *   ./c_api_hsms_server 127.0.0.1 5000
 *   ./c_api_hsms_client 127.0.0.1 5000
 */

#include <secs/c_api.h>

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
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

int main(int argc, char **argv) {
    const char *ip = "127.0.0.1";
    uint16_t port = 5000;
    if (argc >= 2) {
        ip = argv[1];
    }
    if (argc >= 3) {
        port = (uint16_t)atoi(argv[2]);
    }

    printf("=== C API HSMS（TCP）客户端示例 ===\n\n");
    printf("secs version: %s\n", secs_version_string());
    printf("connect: %s:%u\n\n", ip, (unsigned)port);

    (void)secs_log_set_level(SECS_LOG_DEBUG);

    int exit_code = 1;
    secs_context_t *ctx = NULL;
    secs_hsms_session_t *sess = NULL;
    secs_hsms_data_message_t reply;
    memset(&reply, 0, sizeof(reply));

    uint8_t *req_body = NULL;
    size_t req_n = 0;

    if (!ensure_ok("secs_context_create", secs_context_create(&ctx))) {
        goto cleanup;
    }

    secs_hsms_session_options_t opt;
    memset(&opt, 0, sizeof(opt));
    opt.session_id = 0x0001;
    opt.t3_ms = 45000;
    opt.t5_ms = 1000;
    opt.t6_ms = 5000;
    opt.t7_ms = 10000;
    opt.t8_ms = 5000;
    opt.linktest_interval_ms = 0;
    opt.auto_reconnect = 0;
    opt.passive_accept_select = 1;

    if (!ensure_ok("secs_hsms_session_create",
                   secs_hsms_session_create(ctx, &opt, &sess))) {
        goto cleanup;
    }

    printf("[client] connecting...\n");
    if (!ensure_ok("secs_hsms_session_open_active_ip",
                   secs_hsms_session_open_active_ip(sess, ip, port))) {
        goto cleanup;
    }
    printf("[client] selected\n");

    {
        const char *text = "Hello(from c_api_hsms_client)";
        secs_ii_item_t *item = NULL;
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
    }

    printf("[client] request: S1F1 body=%zu\n", req_n);
    if (!ensure_ok("secs_hsms_session_request_data",
                   secs_hsms_session_request_data(
                       sess, 1, 1, req_body, req_n, 10000, &reply))) {
        goto cleanup;
    }

    printf("[client] reply: session_id=0x%04X S%uF%u W=%d SB=0x%08" PRIX32
           " body=%zu\n",
           reply.session_id,
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
    secs_hsms_data_message_free(&reply);
    secs_free(req_body);
    if (sess) {
        (void)secs_hsms_session_stop(sess);
        secs_hsms_session_destroy(sess);
    }
    if (ctx) {
        secs_context_destroy(ctx);
    }
    return exit_code;
}

