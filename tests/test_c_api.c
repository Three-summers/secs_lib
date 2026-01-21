/*
 * @file test_c_api.c
 * @brief C ABI（secs/c_api.h）最小可用性与健壮性测试（包含“恶意输入”用例）。
 *
 * 说明：
 * - 本文件用 C 编译器编译，确保头文件对 C 语言可用；
 * - 但链接阶段必须使用 C++ 链接器（底层实现为 C++20）。
 */

#if defined(__unix__) || defined(__APPLE__)
/* 需要暴露 posix_openpt/grantpt/unlockpt/ptsname 等声明。 */
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 600
#endif
#endif

#include "secs/c_api.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#if defined(__unix__) || defined(__APPLE__)
#include <errno.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

static int g_failures = 0;

static void failf(const char *what, secs_error_t err) {
    char *msg = secs_error_message(err);
    fprintf(stderr,
            "FAIL: %s -> category=%s value=%d msg=%s\n",
            what,
            (err.category ? err.category : "(null)"),
            err.value,
            (msg ? msg : "(null)"));
    if (msg) {
        secs_free(msg);
    }
    ++g_failures;
}

static void expect_ok(const char *what, secs_error_t err) {
    if (err.value == 0) {
        return;
    }
    failf(what, err);
}

static void expect_err(const char *what, secs_error_t err) {
    if (err.value != 0) {
        return;
    }
    fprintf(stderr, "FAIL: %s -> expected error but got OK\n", what);
    ++g_failures;
}

static int wait_until_atomic_eq(const atomic_int *v,
                                int expected,
                                int max_tries,
                                long sleep_ns) {
    /* 仅用于单测：用短暂 sleep 轮询等待异步事件完成，避免长时间阻塞/偶发挂死。 */
    struct timespec req;
    req.tv_sec = 0;
    req.tv_nsec = (sleep_ns > 0 ? sleep_ns : 1);

    for (int i = 0; i < max_tries; ++i) {
        if (atomic_load(v) == expected) {
            return 1;
        }
        (void)nanosleep(&req, NULL);
    }
    return atomic_load(v) == expected;
}

static int wait_until_atomic_gt(const atomic_int *v,
                                int threshold,
                                int max_tries,
                                long sleep_ns) {
    struct timespec req;
    req.tv_sec = 0;
    req.tv_nsec = (sleep_ns > 0 ? sleep_ns : 1);

    for (int i = 0; i < max_tries; ++i) {
        if (atomic_load(v) > threshold) {
            return 1;
        }
        (void)nanosleep(&req, NULL);
    }
    return atomic_load(v) > threshold;
}

#if defined(__unix__) || defined(__APPLE__)
static int pick_free_loopback_tcp_port(uint16_t *out_port) {
    if (!out_port) {
        return 0;
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return 0;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(0);

    if (bind(fd, (struct sockaddr *)&addr, (socklen_t)sizeof(addr)) != 0) {
        (void)close(fd);
        return 0;
    }

    socklen_t len = (socklen_t)sizeof(addr);
    if (getsockname(fd, (struct sockaddr *)&addr, &len) != 0) {
        (void)close(fd);
        return 0;
    }

    const uint16_t port = ntohs(addr.sin_port);
    (void)close(fd);
    if (port == 0) {
        return 0;
    }
    *out_port = port;
    return 1;
}

static void best_effort_tcp_connect(uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);

    (void)connect(fd, (struct sockaddr *)&addr, (socklen_t)sizeof(addr));
    (void)close(fd);
}

struct pty_pair {
    int master_fd;
    char slave_path[256];
};

static int create_pty_pair(struct pty_pair *out) {
    if (!out) {
        return 0;
    }
    out->master_fd = -1;
    out->slave_path[0] = '\0';

    const int fd = posix_openpt(O_RDWR | O_NOCTTY);
    if (fd < 0) {
        return 0;
    }
    if (grantpt(fd) != 0 || unlockpt(fd) != 0) {
        (void)close(fd);
        return 0;
    }

    char *name = ptsname(fd);
    if (!name || name[0] == '\0') {
        (void)close(fd);
        return 0;
    }

    strncpy(out->slave_path, name, sizeof(out->slave_path) - 1);
    out->slave_path[sizeof(out->slave_path) - 1] = '\0';
    out->master_fd = fd;
    return 1;
}

static void destroy_pty_pair(struct pty_pair *p) {
    if (!p) {
        return;
    }
    if (p->master_fd >= 0) {
        (void)close(p->master_fd);
    }
    p->master_fd = -1;
    p->slave_path[0] = '\0';
}
#endif

static void proto_dump_sink(void *user_data, const char *data, size_t size) {
    (void)data;
    (void)size;
    atomic_int *cnt = (atomic_int *)user_data;
    if (!cnt) {
        return;
    }
    (void)atomic_fetch_add(cnt, 1);
}

static void test_version_and_error_message(void) {
    const char *ver = secs_version_string();
    if (!ver || ver[0] == '\0') {
        fprintf(stderr, "FAIL: secs_version_string returned empty\n");
        ++g_failures;
    }

    /* 本地错误域：应能生成可读 message 且可释放 */
    {
        secs_error_t err;
        err.value = (int)SECS_C_API_INVALID_ARGUMENT;
        err.category = "secs.c_api";
        char *msg = secs_error_message(err);
        if (!msg) {
            fprintf(stderr, "FAIL: secs_error_message returned NULL\n");
            ++g_failures;
        } else {
            secs_free(msg);
        }
    }

    /* value==0：应返回 ok */
    {
        secs_error_t err;
        err.value = 0;
        err.category = "secs.c_api";
        char *msg = secs_error_message(err);
        if (!msg) {
            fprintf(stderr, "FAIL: secs_error_message(ok) returned NULL\n");
            ++g_failures;
        } else {
            secs_free(msg);
        }
    }

    /* 未知错误域：也不应崩溃 */
    {
        secs_error_t err;
        err.value = 123;
        err.category = "unknown.category";
        char *msg = secs_error_message(err);
        if (msg) {
            secs_free(msg);
        }
    }
}

static void test_error_message_category_mapping(void) {
    /* 覆盖 c_api.cpp 内部的 category_from_name
     * 分支：只验证“能生成字符串且可释放”。 */
    const char *cats[] = {
        "secs.c_api",
        "secs.core",
        "secs.secs1",
        "secs.ii",
        "sml.lexer",
        "sml.parser",
        "sml.render",
        "system",
        "generic",
        "unknown.category",
    };

    for (size_t i = 0; i < (sizeof(cats) / sizeof(cats[0])); ++i) {
        if (strcmp(cats[i], "secs.c_api") == 0) {
            /* 覆盖 c_api_message_for 的各个分支（value==0
             * 在外层已被提前返回，因此不在此覆盖） */
            const int vals[] = {
                (int)SECS_C_API_INVALID_ARGUMENT,
                (int)SECS_C_API_NOT_FOUND,
                (int)SECS_C_API_OUT_OF_MEMORY,
                (int)SECS_C_API_WRONG_THREAD,
                (int)SECS_C_API_EXCEPTION,
                999,
            };
            for (size_t j = 0; j < (sizeof(vals) / sizeof(vals[0])); ++j) {
                secs_error_t err;
                err.value = vals[j];
                err.category = cats[i];
                char *msg = secs_error_message(err);
                if (!msg) {
                    fprintf(stderr,
                            "FAIL: secs_error_message returned NULL for "
                            "category=%s value=%d\n",
                            cats[i],
                            vals[j]);
                    ++g_failures;
                    continue;
                }
                secs_free(msg);
            }
            continue;
        }

        secs_error_t err;
        err.value = 1;
        err.category = cats[i];
        char *msg = secs_error_message(err);
        if (!msg) {
            fprintf(stderr,
                    "FAIL: secs_error_message returned NULL for category=%s\n",
                    cats[i]);
            ++g_failures;
            continue;
        }
        secs_free(msg);
    }

    /* category==NULL：也不应崩溃 */
    {
        secs_error_t err;
        err.value = 1;
        err.category = NULL;
        char *msg = secs_error_message(err);
        if (msg) {
            secs_free(msg);
        }
    }
}

static void test_log_set_level_smoke(void) {
    expect_ok("secs_log_set_level(trace)", secs_log_set_level(SECS_LOG_TRACE));
    expect_ok("secs_log_set_level(debug)", secs_log_set_level(SECS_LOG_DEBUG));
    expect_ok("secs_log_set_level(info)", secs_log_set_level(SECS_LOG_INFO));
    expect_ok("secs_log_set_level(warn)", secs_log_set_level(SECS_LOG_WARN));
    expect_ok("secs_log_set_level(error)", secs_log_set_level(SECS_LOG_ERROR));
    expect_ok("secs_log_set_level(critical)",
              secs_log_set_level(SECS_LOG_CRITICAL));
    expect_ok("secs_log_set_level(off)", secs_log_set_level(SECS_LOG_OFF));

    secs_error_t err = secs_log_set_level((secs_log_level_t)999);
    expect_err("secs_log_set_level(invalid)", err);
    if (err.value != (int)SECS_C_API_INVALID_ARGUMENT) {
        failf("secs_log_set_level(invalid)", err);
    }
}

static void test_context_create_with_options_smoke(void) {
    /* options init helper */
    secs_context_options_t def;
    memset(&def, 0, sizeof(def));
    secs_context_options_init_default(&def);
    /* NULL 入参应安全 */
    secs_context_options_init_default(NULL);
    if (def.io_threads != 1) {
        fprintf(stderr, "FAIL: secs_context_options_init_default io_threads=%zu\n",
                def.io_threads);
        ++g_failures;
    }

    /* create with explicit thread count */
    secs_context_t *ctx = NULL;
    secs_context_options_t opt;
    memset(&opt, 0, sizeof(opt));
    opt.io_threads = 2;
    expect_ok("secs_context_create_with_options(io_threads=2)",
              secs_context_create_with_options(&ctx, &opt));
    secs_context_destroy(ctx);

    /* io_threads=0：按约定表示“使用默认值” */
    ctx = NULL;
    memset(&opt, 0, sizeof(opt));
    opt.io_threads = 0;
    expect_ok("secs_context_create_with_options(io_threads=0)",
              secs_context_create_with_options(&ctx, &opt));
    secs_context_destroy(ctx);

    /* opt=NULL：使用默认参数（与 secs_context_create 行为一致） */
    ctx = NULL;
    expect_ok("secs_context_create_with_options(NULL opt)",
              secs_context_create_with_options(&ctx, NULL));
    secs_context_destroy(ctx);
}

static void test_hsms_open_passive_ip_invalid_cases(void) {
    secs_context_t *ctx = NULL;
    expect_ok("secs_context_create(ctx)", secs_context_create(&ctx));

    secs_hsms_session_options_t opt;
    memset(&opt, 0, sizeof(opt));
    opt.session_id = 0x2468;
    opt.t3_ms = 2000;
    opt.t5_ms = 200;
    opt.t6_ms = 2000;
    opt.t7_ms = 2000;
    opt.t8_ms = 0;
    opt.linktest_interval_ms = 0;
    opt.auto_reconnect = 0;
    opt.passive_accept_select = 1;

    secs_hsms_session_t *server = NULL;
    secs_hsms_session_t *client = NULL;
    expect_ok("secs_hsms_session_create(server)", secs_hsms_session_create(ctx, &opt, &server));
    expect_ok("secs_hsms_session_create(client)", secs_hsms_session_create(ctx, &opt, &client));

    /* open_passive_ip：invalid argument / parse fast-fail 分支（不触发实际 socket）。 */
    expect_err("secs_hsms_session_open_passive_ip(NULL)",
               secs_hsms_session_open_passive_ip(NULL, "127.0.0.1", 1));
    expect_err("secs_hsms_session_open_passive_ip(NULL ip)",
               secs_hsms_session_open_passive_ip(server, NULL, 1));
    expect_err("secs_hsms_session_open_passive_ip(bad ip)",
               secs_hsms_session_open_passive_ip(server, "not_an_ip", 1));

    (void)secs_hsms_session_stop(client);
    (void)secs_hsms_session_stop(server);
    secs_hsms_session_destroy(client);
    secs_hsms_session_destroy(server);
    secs_context_destroy(ctx);
}

#if defined(__unix__) || defined(__APPLE__)
struct open_ip_args {
    secs_hsms_session_t *sess;
    const char *ip;
    uint16_t port;
    secs_error_t out_err;
};

static void *open_passive_ip_thread(void *p) {
    struct open_ip_args *args = (struct open_ip_args *)p;
    args->out_err = secs_hsms_session_open_passive_ip(args->sess, args->ip, args->port);
    return NULL;
}
#endif

static void test_hsms_open_ip_smoke(void) {
#if !defined(__unix__) && !defined(__APPLE__)
    /* 非 UNIX 环境：跳过该用例（本仓库主要面向 UNIX）。 */
    return;
#else
    uint16_t port = 0;
    if (!pick_free_loopback_tcp_port(&port)) {
        /* 某些沙箱环境会禁止创建 socket（EPERM）。此时跳过该用例，不影响其它覆盖率/功能。 */
        return;
    }

    secs_context_t *ctx = NULL;
    expect_ok("secs_context_create(open_ip)", secs_context_create(&ctx));

    secs_hsms_session_options_t opt;
    memset(&opt, 0, sizeof(opt));
    opt.session_id = 0x3333;
    opt.t3_ms = 2000;
    opt.t5_ms = 200;
    opt.t6_ms = 2000;
    opt.t7_ms = 2000;
    opt.t8_ms = 2000;
    opt.linktest_interval_ms = 0;
    opt.auto_reconnect = 0;
    opt.passive_accept_select = 1;

    secs_hsms_session_t *server = NULL;
    secs_hsms_session_t *client = NULL;
    expect_ok("secs_hsms_session_create(open_ip server)",
              secs_hsms_session_create(ctx, &opt, &server));
    expect_ok("secs_hsms_session_create(open_ip client)",
              secs_hsms_session_create(ctx, &opt, &client));

    struct open_ip_args args;
    memset(&args, 0, sizeof(args));
    args.sess = server;
    args.ip = "127.0.0.1";
    args.port = port;

    pthread_t th;
    int started = pthread_create(&th, NULL, open_passive_ip_thread, &args);
    if (started != 0) {
        fprintf(stderr, "FAIL: pthread_create(open_passive_ip)\n");
        ++g_failures;
        goto cleanup;
    }

    /* 等待 server bind/listen */
    {
        struct timespec req;
        req.tv_sec = 0;
        req.tv_nsec = 20 * 1000 * 1000;
        (void)nanosleep(&req, NULL);
    }

    secs_error_t active_err;
    memset(&active_err, 0, sizeof(active_err));
    int active_ok = 0;
    for (int i = 0; i < 20; ++i) {
        active_err = secs_hsms_session_open_active_ip(client, "127.0.0.1", port);
        if (active_err.value == 0) {
            active_ok = 1;
            break;
        }
        struct timespec req;
        req.tv_sec = 0;
        req.tv_nsec = 10 * 1000 * 1000;
        (void)nanosleep(&req, NULL);
    }
    if (!active_ok) {
        failf("secs_hsms_session_open_active_ip(loopback)", active_err);
        /* 避免 server 线程永久卡在 accept：尽力触发一次连接以解锁 accept。 */
        best_effort_tcp_connect(port);
    }

    if (started == 0) {
        (void)pthread_join(th, NULL);
        if (active_ok) {
            expect_ok("secs_hsms_session_open_passive_ip(loopback)", args.out_err);
        }
    }

    if (active_ok && args.out_err.value == 0) {
        int selected = 0;
        expect_ok("secs_hsms_session_is_selected(open_ip client)",
                  secs_hsms_session_is_selected(client, &selected));
        if (!selected) {
            fprintf(stderr, "FAIL: open_ip client not selected\n");
            ++g_failures;
        }
        selected = 0;
        expect_ok("secs_hsms_session_is_selected(open_ip server)",
                  secs_hsms_session_is_selected(server, &selected));
        if (!selected) {
            fprintf(stderr, "FAIL: open_ip server not selected\n");
            ++g_failures;
        }
    }

cleanup:
    (void)secs_hsms_session_stop(client);
    (void)secs_hsms_session_stop(server);
    secs_hsms_session_destroy(client);
    secs_hsms_session_destroy(server);
    secs_context_destroy(ctx);
#endif
}

static void test_invalid_argument_fast_fail(void) {
    /* 这些用例不追求业务意义，主要用于覆盖“参数校验/快速失败”分支，且必须不阻塞/不崩溃。
     */
    {
        secs_error_t err = secs_context_create(NULL);
        expect_err("secs_context_create(NULL)", err);
        if (err.value != (int)SECS_C_API_INVALID_ARGUMENT) {
            failf("secs_context_create(NULL)", err);
        }
    }

    /* destroy/free 对 NULL 应安全 */
    secs_context_destroy(NULL);
    secs_free(NULL);
    secs_hsms_data_message_free(NULL);
    secs_data_message_free(NULL);
    secs_hsms_connection_destroy(NULL);
    secs_sml_runtime_destroy(NULL);
    secs_sml_render_context_destroy(NULL);
    secs_sml_render_context_clear(NULL);
    secs_sml_match_traces_free(NULL, 123);

    secs_context_t *ctx = NULL;
    expect_ok("secs_context_create(valid)", secs_context_create(&ctx));

    /* SECS-II 参数校验 */
    {
        secs_ii_item_t *item = NULL;
        expect_err("secs_ii_item_create_list(NULL out)",
                   secs_ii_item_create_list(NULL));
        expect_err("secs_ii_item_create_ascii(NULL out)",
                   secs_ii_item_create_ascii("x", 1, NULL));
        expect_err("secs_ii_item_create_ascii(NULL,n>0)",
                   secs_ii_item_create_ascii(NULL, 1, &item));
        expect_err("secs_ii_item_create_ascii_cstr(NULL value)",
                   secs_ii_item_create_ascii_cstr(NULL, &item));
        expect_err("secs_ii_item_create_ascii_cstr(NULL out)",
                   secs_ii_item_create_ascii_cstr("x", NULL));
        expect_err("secs_ii_item_create_binary(NULL,n>0)",
                   secs_ii_item_create_binary(NULL, 1, &item));
        expect_err("secs_ii_item_create_boolean(NULL,n>0)",
                   secs_ii_item_create_boolean(NULL, 1, &item));
        expect_err("secs_ii_item_create_i1(NULL,n>0)",
                   secs_ii_item_create_i1(NULL, 1, &item));
        expect_err("secs_ii_item_create_i2(NULL,n>0)",
                   secs_ii_item_create_i2(NULL, 1, &item));
        expect_err("secs_ii_item_create_i4(NULL,n>0)",
                   secs_ii_item_create_i4(NULL, 1, &item));
        expect_err("secs_ii_item_create_i8(NULL,n>0)",
                   secs_ii_item_create_i8(NULL, 1, &item));
        expect_err("secs_ii_item_create_u1(NULL,n>0)",
                   secs_ii_item_create_u1(NULL, 1, &item));
        expect_err("secs_ii_item_create_u2(NULL,n>0)",
                   secs_ii_item_create_u2(NULL, 1, &item));
        expect_err("secs_ii_item_create_u4(NULL,n>0)",
                   secs_ii_item_create_u4(NULL, 1, &item));
        expect_err("secs_ii_item_create_u8(NULL,n>0)",
                   secs_ii_item_create_u8(NULL, 1, &item));
        expect_err("secs_ii_item_create_f4(NULL,n>0)",
                   secs_ii_item_create_f4(NULL, 1, &item));
        expect_err("secs_ii_item_create_f8(NULL,n>0)",
                   secs_ii_item_create_f8(NULL, 1, &item));

        expect_err("secs_ii_item_clone(NULL src)", secs_ii_item_clone(NULL, &item));
        {
            secs_ii_item_t *tmp = NULL;
            expect_ok("secs_ii_item_create_list(clone tmp)",
                      secs_ii_item_create_list(&tmp));
            expect_err("secs_ii_item_clone(NULL out)",
                       secs_ii_item_clone(tmp, NULL));
            secs_ii_item_destroy(tmp);
        }

        secs_ii_item_type_t ty;
        expect_err("secs_ii_item_get_type(NULL)",
                   secs_ii_item_get_type(NULL, &ty));

        uint8_t *out_bytes = NULL;
        size_t out_n = 0;
        expect_err("secs_ii_encode(NULL)",
                   secs_ii_encode(NULL, &out_bytes, &out_n));

        size_t consumed = 0;
        expect_err("secs_ii_decode_one(NULL, n>0)",
                   secs_ii_decode_one(NULL, 1, &consumed, &item));

        /* at_list_path 参数校验 */
        {
            const uint8_t *bp = NULL;
            size_t bn = 0;
            expect_err("secs_ii_item_binary_view_at_list_path(NULL root)",
                       secs_ii_item_binary_view_at_list_path(NULL, &bp, &bn, NULL, 0));

            int32_t i4 = 0;
            expect_err("secs_ii_item_get_i4_at_list_path(NULL root)",
                       secs_ii_item_get_i4_at_list_path(NULL, &i4, NULL, 0));

            uint8_t b01 = 0;
            expect_err("secs_ii_item_get_boolean_at_list_path(NULL root)",
                       secs_ii_item_get_boolean_at_list_path(NULL, &b01, NULL, 0));
        }
    }

    /* SML 参数校验 */
    {
        secs_sml_runtime_t *rt = NULL;
        expect_ok("secs_sml_runtime_create(valid)",
                  secs_sml_runtime_create(&rt));
        expect_err("secs_sml_runtime_load(NULL)",
                   secs_sml_runtime_load(NULL, "x", 1));
        expect_err("secs_sml_runtime_load_cstr(NULL)",
                   secs_sml_runtime_load_cstr(NULL, "x"));
        expect_err("secs_sml_runtime_load_cstr(NULL source)",
                   secs_sml_runtime_load_cstr(rt, NULL));

        expect_err("secs_sml_render_context_create(NULL)",
                   secs_sml_render_context_create(NULL));
        secs_sml_render_context_t *rctx = NULL;
        expect_ok("secs_sml_render_context_create(valid)",
                  secs_sml_render_context_create(&rctx));

        secs_ii_item_t *tmp = NULL;
        expect_ok("secs_ii_item_create_ascii(tmp)",
                  secs_ii_item_create_ascii("x", 1, &tmp));
        expect_err("secs_sml_render_context_set(NULL ctx)",
                   secs_sml_render_context_set(NULL, "X", tmp));
        expect_err("secs_sml_render_context_set(NULL name)",
                   secs_sml_render_context_set(rctx, NULL, tmp));
        expect_err("secs_sml_render_context_set(NULL value)",
                   secs_sml_render_context_set(rctx, "X", NULL));

        /* RenderContext 便捷 setter：减少样板代码 & 覆盖 C API 新增符号 */
        expect_err("secs_sml_render_context_set_ascii(NULL ctx)",
                   secs_sml_render_context_set_ascii(NULL, "X", "x"));
        expect_err("secs_sml_render_context_set_ascii(NULL name)",
                   secs_sml_render_context_set_ascii(rctx, NULL, "x"));
        expect_err("secs_sml_render_context_set_ascii(NULL value)",
                   secs_sml_render_context_set_ascii(rctx, "X", NULL));

        expect_err("secs_sml_render_context_set_binary(NULL bytes,n>0)",
                   secs_sml_render_context_set_binary(rctx, "B", NULL, 1));
        expect_err("secs_sml_render_context_set_boolean(bad value01)",
                   secs_sml_render_context_set_boolean(rctx, "BOOL", 2));

        /* ASCII */
        expect_ok("secs_sml_render_context_set_ascii(ASCI)",
                  secs_sml_render_context_set_ascii(rctx, "ASCI", "hi"));
        {
            secs_ii_item_t *out = NULL;
            expect_ok("secs_sml_render_context_get(ASCI)",
                      secs_sml_render_context_get(rctx, "ASCI", &out));
            const char *p = NULL;
            size_t n = 0;
            expect_ok("secs_ii_item_ascii_view(ASCI)",
                      secs_ii_item_ascii_view(out, &p, &n));
            if (!p || n != 2 || memcmp(p, "hi", 2) != 0) {
                fprintf(stderr, "FAIL: RenderContext ASCI mismatch\n");
                ++g_failures;
            }
            secs_ii_item_destroy(out);
        }

        /* Binary */
        expect_ok("secs_sml_render_context_set_binary(BIN)",
                  secs_sml_render_context_set_binary(
                      rctx, "BIN", (const uint8_t *)"\x01\x02", 2));
        {
            secs_ii_item_t *out = NULL;
            expect_ok("secs_sml_render_context_get(BIN)",
                      secs_sml_render_context_get(rctx, "BIN", &out));
            const uint8_t *p = NULL;
            size_t n = 0;
            expect_ok("secs_ii_item_binary_view(BIN)",
                      secs_ii_item_binary_view(out, &p, &n));
            if (!p || n != 2 || p[0] != 1 || p[1] != 2) {
                fprintf(stderr, "FAIL: RenderContext BIN mismatch\n");
                ++g_failures;
            }
            secs_ii_item_destroy(out);
        }

        /* Boolean */
        expect_ok("secs_sml_render_context_set_boolean(BOOL)",
                  secs_sml_render_context_set_boolean(rctx, "BOOL", 1));
        {
            secs_ii_item_t *out = NULL;
            expect_ok("secs_sml_render_context_get(BOOL)",
                      secs_sml_render_context_get(rctx, "BOOL", &out));
            uint8_t *p = NULL;
            size_t n = 0;
            expect_ok("secs_ii_item_boolean_copy(BOOL)",
                      secs_ii_item_boolean_copy(out, &p, &n));
            if (!p || n != 1 || p[0] != 1) {
                fprintf(stderr, "FAIL: RenderContext BOOL mismatch\n");
                ++g_failures;
            }
            secs_free(p);
            secs_ii_item_destroy(out);
        }

        /* I1/I2/I4/I8 */
        expect_ok("secs_sml_render_context_set_i1(I1)",
                  secs_sml_render_context_set_i1(rctx, "I1", (int8_t)-3));
        expect_ok("secs_sml_render_context_set_i2(I2)",
                  secs_sml_render_context_set_i2(rctx, "I2", (int16_t)-300));
        expect_ok("secs_sml_render_context_set_i4(I4)",
                  secs_sml_render_context_set_i4(rctx, "I4", (int32_t)-123456));
        expect_ok("secs_sml_render_context_set_i8(I8)",
                  secs_sml_render_context_set_i8(rctx, "I8", (int64_t)-1234567890));
        {
            secs_ii_item_t *out = NULL;
            const int8_t *p1 = NULL;
            const int16_t *p2 = NULL;
            const int32_t *p4 = NULL;
            const int64_t *p8 = NULL;
            size_t n = 0;

            expect_ok("secs_sml_render_context_get(I1)",
                      secs_sml_render_context_get(rctx, "I1", &out));
            expect_ok("secs_ii_item_i1_view(I1)", secs_ii_item_i1_view(out, &p1, &n));
            if (!p1 || n != 1 || p1[0] != (int8_t)-3) {
                fprintf(stderr, "FAIL: RenderContext I1 mismatch\n");
                ++g_failures;
            }
            secs_ii_item_destroy(out);

            out = NULL;
            expect_ok("secs_sml_render_context_get(I2)",
                      secs_sml_render_context_get(rctx, "I2", &out));
            expect_ok("secs_ii_item_i2_view(I2)", secs_ii_item_i2_view(out, &p2, &n));
            if (!p2 || n != 1 || p2[0] != (int16_t)-300) {
                fprintf(stderr, "FAIL: RenderContext I2 mismatch\n");
                ++g_failures;
            }
            secs_ii_item_destroy(out);

            out = NULL;
            expect_ok("secs_sml_render_context_get(I4)",
                      secs_sml_render_context_get(rctx, "I4", &out));
            expect_ok("secs_ii_item_i4_view(I4)", secs_ii_item_i4_view(out, &p4, &n));
            if (!p4 || n != 1 || p4[0] != (int32_t)-123456) {
                fprintf(stderr, "FAIL: RenderContext I4 mismatch\n");
                ++g_failures;
            }
            secs_ii_item_destroy(out);

            out = NULL;
            expect_ok("secs_sml_render_context_get(I8)",
                      secs_sml_render_context_get(rctx, "I8", &out));
            expect_ok("secs_ii_item_i8_view(I8)", secs_ii_item_i8_view(out, &p8, &n));
            if (!p8 || n != 1 || p8[0] != (int64_t)-1234567890) {
                fprintf(stderr, "FAIL: RenderContext I8 mismatch\n");
                ++g_failures;
            }
            secs_ii_item_destroy(out);
        }

        /* U1/U2/U4/U8 */
        expect_ok("secs_sml_render_context_set_u1(U1)",
                  secs_sml_render_context_set_u1(rctx, "U1", (uint8_t)3));
        expect_ok("secs_sml_render_context_set_u2(U2)",
                  secs_sml_render_context_set_u2(rctx, "U2", (uint16_t)300));
        expect_ok("secs_sml_render_context_set_u4(U4)",
                  secs_sml_render_context_set_u4(rctx, "U4", (uint32_t)123456));
        expect_ok("secs_sml_render_context_set_u8(U8)",
                  secs_sml_render_context_set_u8(rctx, "U8", (uint64_t)1234567890));
        {
            secs_ii_item_t *out = NULL;
            const uint8_t *p1 = NULL;
            const uint16_t *p2 = NULL;
            const uint32_t *p4 = NULL;
            const uint64_t *p8 = NULL;
            size_t n = 0;

            expect_ok("secs_sml_render_context_get(U1)",
                      secs_sml_render_context_get(rctx, "U1", &out));
            expect_ok("secs_ii_item_u1_view(U1)", secs_ii_item_u1_view(out, &p1, &n));
            if (!p1 || n != 1 || p1[0] != (uint8_t)3) {
                fprintf(stderr, "FAIL: RenderContext U1 mismatch\n");
                ++g_failures;
            }
            secs_ii_item_destroy(out);

            out = NULL;
            expect_ok("secs_sml_render_context_get(U2)",
                      secs_sml_render_context_get(rctx, "U2", &out));
            expect_ok("secs_ii_item_u2_view(U2)", secs_ii_item_u2_view(out, &p2, &n));
            if (!p2 || n != 1 || p2[0] != (uint16_t)300) {
                fprintf(stderr, "FAIL: RenderContext U2 mismatch\n");
                ++g_failures;
            }
            secs_ii_item_destroy(out);

            out = NULL;
            expect_ok("secs_sml_render_context_get(U4)",
                      secs_sml_render_context_get(rctx, "U4", &out));
            expect_ok("secs_ii_item_u4_view(U4)", secs_ii_item_u4_view(out, &p4, &n));
            if (!p4 || n != 1 || p4[0] != (uint32_t)123456) {
                fprintf(stderr, "FAIL: RenderContext U4 mismatch\n");
                ++g_failures;
            }
            secs_ii_item_destroy(out);

            out = NULL;
            expect_ok("secs_sml_render_context_get(U8)",
                      secs_sml_render_context_get(rctx, "U8", &out));
            expect_ok("secs_ii_item_u8_view(U8)", secs_ii_item_u8_view(out, &p8, &n));
            if (!p8 || n != 1 || p8[0] != (uint64_t)1234567890) {
                fprintf(stderr, "FAIL: RenderContext U8 mismatch\n");
                ++g_failures;
            }
            secs_ii_item_destroy(out);
        }

        /* F4/F8 */
        expect_ok("secs_sml_render_context_set_f4(F4)",
                  secs_sml_render_context_set_f4(rctx, "F4", 1.25f));
        expect_ok("secs_sml_render_context_set_f8(F8)",
                  secs_sml_render_context_set_f8(rctx, "F8", 1.25));
        {
            secs_ii_item_t *out = NULL;
            const float *pf4 = NULL;
            const double *pf8 = NULL;
            size_t n = 0;

            expect_ok("secs_sml_render_context_get(F4)",
                      secs_sml_render_context_get(rctx, "F4", &out));
            expect_ok("secs_ii_item_f4_view(F4)", secs_ii_item_f4_view(out, &pf4, &n));
            if (!pf4 || n != 1 || pf4[0] != 1.25f) {
                fprintf(stderr, "FAIL: RenderContext F4 mismatch\n");
                ++g_failures;
            }
            secs_ii_item_destroy(out);

            out = NULL;
            expect_ok("secs_sml_render_context_get(F8)",
                      secs_sml_render_context_get(rctx, "F8", &out));
            expect_ok("secs_ii_item_f8_view(F8)", secs_ii_item_f8_view(out, &pf8, &n));
            if (!pf8 || n != 1 || pf8[0] != 1.25) {
                fprintf(stderr, "FAIL: RenderContext F8 mismatch\n");
                ++g_failures;
            }
            secs_ii_item_destroy(out);
        }
        secs_ii_item_destroy(tmp);
        secs_sml_render_context_destroy(rctx);

        {
            uint8_t *body = NULL;
            size_t body_n = 0;
            char *out_name = NULL;
            secs_sml_match_trace_t *traces = NULL;
            size_t trace_n = 0;

            expect_err("secs_sml_runtime_encode_message_body(NULL rt)",
                       secs_sml_runtime_encode_message_body(
                           NULL, "x", NULL, &body, &body_n, NULL, NULL, NULL));
            expect_err("secs_sml_runtime_encode_message_body(NULL name)",
                       secs_sml_runtime_encode_message_body(
                           rt, NULL, NULL, &body, &body_n, NULL, NULL, NULL));
            expect_err("secs_sml_runtime_encode_message_body(NULL out_body)",
                       secs_sml_runtime_encode_message_body(
                           rt, "x", NULL, NULL, &body_n, NULL, NULL, NULL));

            expect_err("secs_sml_runtime_match_response_with_context(NULL rt)",
                       secs_sml_runtime_match_response_with_context(
                           NULL, 1, 1, NULL, 0, NULL, &out_name));
            expect_err("secs_sml_runtime_match_response_with_context(NULL out)",
                       secs_sml_runtime_match_response_with_context(
                           rt, 1, 1, NULL, 0, NULL, NULL));

            expect_err("secs_sml_runtime_match_response_with_trace(NULL rt)",
                       secs_sml_runtime_match_response_with_trace(
                           NULL,
                           1,
                           1,
                           NULL,
                           0,
                           NULL,
                           &out_name,
                           &traces,
                           &trace_n));
            expect_err("secs_sml_runtime_match_response_with_trace(NULL out_name)",
                       secs_sml_runtime_match_response_with_trace(
                           rt, 1, 1, NULL, 0, NULL, NULL, &traces, &trace_n));
        }
        secs_sml_runtime_destroy(rt);
    }

    /* HSMS 参数校验 */
    {
        secs_hsms_connection_t *c = NULL;
        secs_hsms_connection_t *s = NULL;
        expect_err("secs_hsms_connection_create_memory_duplex(NULL)",
                   secs_hsms_connection_create_memory_duplex(NULL, &c, &s));
        expect_err("secs_hsms_connection_create_memory_duplex(ctx,NULL)",
                   secs_hsms_connection_create_memory_duplex(ctx, NULL, &s));
    }
    {
        secs_hsms_session_t *sess = NULL;
        secs_hsms_session_options_t opt;
        memset(&opt, 0, sizeof(opt));
        opt.session_id = 1;
        expect_err("secs_hsms_session_create(ctx,NULL)",
                   secs_hsms_session_create(ctx, NULL, &sess));
        expect_err("secs_hsms_session_create(NULL,opt)",
                   secs_hsms_session_create(NULL, &opt, &sess));

        /* 合法 IP + 大概率关闭端口：覆盖 open_active_ip
         * 的后续路径（应快速失败） */
        expect_ok("secs_hsms_session_create(tmp)",
                  secs_hsms_session_create(ctx, &opt, &sess));
        expect_err("secs_hsms_session_open_active_ip(127.0.0.1:65535)",
                   secs_hsms_session_open_active_ip(sess, "127.0.0.1", 65535));

        /* 非法 IP：应在解析阶段直接失败，不做网络尝试 */
        expect_err("secs_hsms_session_open_active_ip(bad ip)",
                   secs_hsms_session_open_active_ip(sess, "not_an_ip", 1));

        /* is_selected 参数校验：out_selected 为空 / sess 为空 */
        {
            int selected = 0;
            expect_err("secs_hsms_session_is_selected(NULL out)",
                       secs_hsms_session_is_selected(sess, NULL));
            expect_err("secs_hsms_session_is_selected(NULL sess)",
                       secs_hsms_session_is_selected(NULL, &selected));
        }

        /* HSMS：其它 API 的快速失败分支（不要求业务意义，只要求不阻塞/不崩溃） */
        {
            uint32_t sb = 0;
            secs_error_t err = secs_hsms_session_send_data_auto_system_bytes(
                NULL, 1, 1, 0, NULL, 0, &sb);
            expect_err("secs_hsms_session_send_data_auto_system_bytes(NULL)",
                       err);
            if (err.value != (int)SECS_C_API_INVALID_ARGUMENT) {
                failf("secs_hsms_session_send_data_auto_system_bytes(NULL)", err);
            }

            err = secs_hsms_session_send_data_with_system_bytes(
                NULL, 1, 1, 0, 1, NULL, 0);
            expect_err("secs_hsms_session_send_data_with_system_bytes(NULL)",
                       err);
            if (err.value != (int)SECS_C_API_INVALID_ARGUMENT) {
                failf("secs_hsms_session_send_data_with_system_bytes(NULL)", err);
            }

            secs_hsms_data_message_t rx;
            memset(&rx, 0, sizeof(rx));
            err = secs_hsms_session_receive_data(NULL, 1, &rx);
            expect_err("secs_hsms_session_receive_data(NULL)", err);
            if (err.value != (int)SECS_C_API_INVALID_ARGUMENT) {
                failf("secs_hsms_session_receive_data(NULL)", err);
            }
            secs_hsms_data_message_free(&rx);

            err = secs_hsms_session_receive_data(sess, 1, NULL);
            expect_err("secs_hsms_session_receive_data(NULL out)", err);
            if (err.value != (int)SECS_C_API_INVALID_ARGUMENT) {
                failf("secs_hsms_session_receive_data(NULL out)", err);
            }

            secs_hsms_data_message_t reply;
            memset(&reply, 0, sizeof(reply));
            err = secs_hsms_session_request_data(NULL, 1, 1, NULL, 0, 1, &reply);
            expect_err("secs_hsms_session_request_data(NULL)", err);
            if (err.value != (int)SECS_C_API_INVALID_ARGUMENT) {
                failf("secs_hsms_session_request_data(NULL)", err);
            }
            secs_hsms_data_message_free(&reply);

            err = secs_hsms_session_request_data(sess, 1, 1, NULL, 0, 1, NULL);
            expect_err("secs_hsms_session_request_data(NULL out)", err);
            if (err.value != (int)SECS_C_API_INVALID_ARGUMENT) {
                failf("secs_hsms_session_request_data(NULL out)", err);
            }

            err = secs_hsms_session_linktest(NULL);
            expect_err("secs_hsms_session_linktest(NULL)", err);
            if (err.value != (int)SECS_C_API_INVALID_ARGUMENT) {
                failf("secs_hsms_session_linktest(NULL)", err);
            }

            /* open_active/passive_connection：io_conn 为空/空指针 */
            err = secs_hsms_session_open_active_connection(NULL, NULL);
            expect_err("secs_hsms_session_open_active_connection(NULL sess)",
                       err);
            if (err.value != (int)SECS_C_API_INVALID_ARGUMENT) {
                failf("secs_hsms_session_open_active_connection(NULL sess)", err);
            }

            err = secs_hsms_session_open_active_connection(sess, NULL);
            expect_err("secs_hsms_session_open_active_connection(NULL io_conn)",
                       err);
            if (err.value != (int)SECS_C_API_INVALID_ARGUMENT) {
                failf("secs_hsms_session_open_active_connection(NULL io_conn)",
                      err);
            }
            {
                secs_hsms_connection_t *tmp = NULL;
                err = secs_hsms_session_open_active_connection(sess, &tmp);
                expect_err("secs_hsms_session_open_active_connection(*NULL)",
                           err);
                if (err.value != (int)SECS_C_API_INVALID_ARGUMENT) {
                    failf("secs_hsms_session_open_active_connection(*NULL)", err);
                }
            }

            err = secs_hsms_session_open_passive_connection(NULL, NULL);
            expect_err("secs_hsms_session_open_passive_connection(NULL sess)",
                       err);
            if (err.value != (int)SECS_C_API_INVALID_ARGUMENT) {
                failf("secs_hsms_session_open_passive_connection(NULL sess)", err);
            }

            err = secs_hsms_session_open_passive_connection(sess, NULL);
            expect_err("secs_hsms_session_open_passive_connection(NULL io_conn)",
                       err);
            if (err.value != (int)SECS_C_API_INVALID_ARGUMENT) {
                failf("secs_hsms_session_open_passive_connection(NULL io_conn)",
                      err);
            }
            {
                secs_hsms_connection_t *tmp = NULL;
                err = secs_hsms_session_open_passive_connection(sess, &tmp);
                expect_err("secs_hsms_session_open_passive_connection(*NULL)",
                           err);
                if (err.value != (int)SECS_C_API_INVALID_ARGUMENT) {
                    failf("secs_hsms_session_open_passive_connection(*NULL)", err);
                }
            }
        }
        secs_hsms_session_destroy(sess);
        sess = NULL;

        /* create_from_hsms：options==NULL（覆盖 make_proto_options 的默认分支） */
        {
            secs_hsms_session_t *hsms_for_proto = NULL;
            expect_ok("secs_hsms_session_create(for proto opt null)",
                      secs_hsms_session_create(ctx, &opt, &hsms_for_proto));

            secs_protocol_session_t *ps = NULL;
            expect_ok("secs_protocol_session_create_from_hsms(options NULL)",
                      secs_protocol_session_create_from_hsms(
                          ctx, hsms_for_proto, opt.session_id, NULL, &ps));
            {
                int handled = 0;
                expect_err("secs_protocol_session_poll_once(NULL out_handled)",
                           secs_protocol_session_poll_once(ps, 1, NULL));
                expect_err("secs_protocol_session_poll_once(NULL sess)",
                           secs_protocol_session_poll_once(NULL, 1, &handled));
            }
            secs_protocol_session_destroy(ps);
            secs_hsms_session_destroy(hsms_for_proto);
        }

        expect_err("secs_hsms_session_open_active_ip(NULL)",
                   secs_hsms_session_open_active_ip(NULL, "127.0.0.1", 1));
        expect_err("secs_hsms_session_open_active_ip(NULL ip)",
                   secs_hsms_session_open_active_ip(sess, NULL, 1));

        expect_err("secs_hsms_session_stop(NULL)",
                   secs_hsms_session_stop(NULL));
        secs_hsms_session_destroy(NULL);
    }

    /* Protocol 参数校验 */
    {
        secs_protocol_session_t *ps = NULL;
        expect_err(
            "secs_protocol_session_create_from_hsms(NULL)",
            secs_protocol_session_create_from_hsms(NULL, NULL, 0, NULL, &ps));
        expect_err("secs_protocol_session_set_handler(NULL)",
                   secs_protocol_session_set_handler(NULL, 1, 1, NULL, NULL));
        expect_err("secs_protocol_session_set_stream_default_handler(NULL)",
                   secs_protocol_session_set_stream_default_handler(
                       NULL, 1, NULL, NULL));
        expect_err("secs_protocol_session_clear_stream_default_handler(NULL)",
                   secs_protocol_session_clear_stream_default_handler(NULL, 1));
        expect_err(
            "secs_protocol_session_set_sml_default_handler(NULL)",
            secs_protocol_session_set_sml_default_handler(NULL, NULL));
        expect_err("secs_protocol_session_set_sml_stream_default_handler(NULL)",
                   secs_protocol_session_set_sml_stream_default_handler(NULL, 1, NULL));
        expect_err("secs_protocol_session_set_decoded_handler(NULL)",
                   secs_protocol_session_set_decoded_handler(
                       NULL, 1, 1, NULL, 1, NULL, NULL));
        expect_err("secs_protocol_session_set_decoded_stream_default_handler(NULL)",
                   secs_protocol_session_set_decoded_stream_default_handler(
                       NULL, 1, NULL, 1, NULL, NULL));
        expect_err("secs_protocol_session_set_decoded_default_handler(NULL)",
                   secs_protocol_session_set_decoded_default_handler(
                       NULL, NULL, 1, NULL, NULL));
        expect_err("secs_protocol_session_send(NULL)",
                   secs_protocol_session_send(NULL, 1, 1, NULL, 0));
        expect_err("secs_protocol_session_request(NULL)",
                   secs_protocol_session_request(NULL, 1, 1, NULL, 0, 1, NULL));
        secs_protocol_session_destroy(NULL);
    }

    /* CEID dispatcher 参数校验 */
    {
        expect_err("secs_ceid_dispatcher_clear_default_handler(NULL)",
                   secs_ceid_dispatcher_clear_default_handler(NULL));
        expect_err("secs_ceid_dispatcher_erase_handler(NULL)",
                   secs_ceid_dispatcher_erase_handler(NULL, 1));

        /* create_list_path：out_disp/indices 参数校验 */
        {
            const size_t path[1] = {0};
            expect_err("secs_ceid_dispatcher_create_list_path(NULL out_disp)",
                       secs_ceid_dispatcher_create_list_path(
                           path, 1, NULL, 0, NULL));

            secs_ceid_dispatcher_t *tmp = NULL;
            expect_err("secs_ceid_dispatcher_create_list_path(NULL indices,n>0)",
                       secs_ceid_dispatcher_create_list_path(
                           NULL, 1, NULL, 0, &tmp));
            secs_ceid_dispatcher_destroy(tmp);
        }

        /* handler 注册：cb 为 NULL 必须拒绝 */
        expect_err("secs_ceid_dispatcher_set_handler(NULL cb)",
                   secs_ceid_dispatcher_set_handler(NULL, 1, NULL, NULL));
        expect_err("secs_ceid_dispatcher_set_default_handler(NULL cb)",
                   secs_ceid_dispatcher_set_default_handler(NULL, NULL, NULL));
    }

    /* request_with_ceid_list_path：需要有效 session 才能覆盖更深的参数校验 */
    {
        secs_protocol_session_options_t opt;
        memset(&opt, 0, sizeof(opt));
        opt.t3_ms = 200;
        opt.poll_interval_ms = 1;

        secs_protocol_session_t *host = NULL;
        secs_protocol_session_t *eq = NULL;
        expect_ok("secs_protocol_session_create_from_secs1_memory_duplex(for invalid args)",
                  secs_protocol_session_create_from_secs1_memory_duplex(
                      ctx, 0x0101, &opt, &host, &eq));

        /* cb==NULL：覆盖 router 注册的校验分支 */
        expect_err("secs_protocol_session_set_stream_default_handler(NULL cb)",
                   secs_protocol_session_set_stream_default_handler(
                       host, 1, NULL, NULL));
        expect_err("secs_protocol_session_set_default_handler(NULL cb)",
                   secs_protocol_session_set_default_handler(host, NULL, NULL));
        expect_err("secs_protocol_session_set_decoded_default_handler(NULL cb)",
                   secs_protocol_session_set_decoded_default_handler(
                       host, NULL, 0, NULL, NULL));

        /* request_with_ceid_list_path：body/ceid_indices 的指针/长度一致性检查 */
        {
            secs_data_message_t reply;
            memset(&reply, 0, sizeof(reply));
            const size_t ceid_path[1] = {1};

            expect_err(
                "secs_protocol_session_request_with_ceid_list_path(NULL body,n>0)",
                secs_protocol_session_request_with_ceid_list_path(
                    host,
                    1,
                    1,
                    NULL,
                    1,
                    1,
                    ceid_path,
                    1,
                    NULL,
                    0,
                    &reply,
                    NULL,
                    NULL,
                    NULL,
                    NULL));

            expect_err(
                "secs_protocol_session_request_with_ceid_list_path(NULL ceid_indices,n>0)",
                secs_protocol_session_request_with_ceid_list_path(
                    host,
                    1,
                    1,
                    NULL,
                    0,
                    1,
                    NULL,
                    1,
                    NULL,
                    0,
                    &reply,
                    NULL,
                    NULL,
                    NULL,
                    NULL));

            secs_data_message_free(&reply);
        }

        secs_protocol_session_destroy(host);
        secs_protocol_session_destroy(eq);
    }

    /* Protocol：ctx 与 hsms_session 所属 ctx 不一致必须拒绝（避免跨 io_context
     * 误用）。 */
    {
        secs_context_t *ctx2 = NULL;
        expect_ok("secs_context_create(ctx2)", secs_context_create(&ctx2));

        secs_hsms_session_options_t opt;
        memset(&opt, 0, sizeof(opt));
        opt.session_id = 1;

        secs_hsms_session_t *hsms = NULL;
        expect_ok("secs_hsms_session_create(ctx mismatch)",
                  secs_hsms_session_create(ctx, &opt, &hsms));

        secs_protocol_session_t *ps = NULL;
        expect_err(
            "secs_protocol_session_create_from_hsms(ctx mismatch)",
            secs_protocol_session_create_from_hsms(ctx2, hsms, 1, NULL, &ps));
        secs_protocol_session_destroy(ps);

        secs_hsms_session_destroy(hsms);
        secs_context_destroy(ctx2);
    }

    secs_context_destroy(ctx);
}

static secs_ii_item_t *make_nested_list_item(size_t depth) {
    secs_ii_item_t *cur = NULL;
    if (secs_ii_item_create_list(&cur).value != 0) {
        return NULL;
    }

    for (size_t i = 0; i < depth; ++i) {
        secs_ii_item_t *parent = NULL;
        if (secs_ii_item_create_list(&parent).value != 0) {
            secs_ii_item_destroy(cur);
            return NULL;
        }
        (void)secs_ii_item_list_append(parent, cur);
        secs_ii_item_destroy(cur);
        cur = parent;
    }
    return cur;
}

static void test_ii_encode_decode_and_malicious(void) {
    secs_ii_item_t *list = NULL;
    expect_ok("secs_ii_item_create_list", secs_ii_item_create_list(&list));

    secs_ii_item_t *ascii = NULL;
    expect_ok("secs_ii_item_create_ascii",
              secs_ii_item_create_ascii("ABC", 3, &ascii));
    expect_ok("secs_ii_item_list_append",
              secs_ii_item_list_append(list, ascii));
    secs_ii_item_destroy(ascii);

    uint8_t *bytes = NULL;
    size_t n = 0;
    expect_ok("secs_ii_encode", secs_ii_encode(list, &bytes, &n));
    if (!bytes || n == 0) {
        fprintf(stderr, "FAIL: secs_ii_encode returned empty\n");
        ++g_failures;
    }

    /* 正常解码 */
    {
        size_t consumed = 0;
        secs_ii_item_t *decoded = NULL;
        expect_ok("secs_ii_decode_one",
                  secs_ii_decode_one(bytes, n, &consumed, &decoded));
        if (consumed != n) {
            fprintf(stderr,
                    "FAIL: secs_ii_decode_one consumed mismatch: %zu != %zu\n",
                    consumed,
                    n);
            ++g_failures;
        }

        secs_ii_item_type_t ty;
        expect_ok("secs_ii_item_get_type", secs_ii_item_get_type(decoded, &ty));
        if (ty != SECS_II_ITEM_LIST) {
            fprintf(stderr, "FAIL: decoded type mismatch\n");
            ++g_failures;
        }

        size_t child_n = 0;
        expect_ok("secs_ii_item_list_size",
                  secs_ii_item_list_size(decoded, &child_n));
        if (child_n != 1u) {
            fprintf(stderr, "FAIL: decoded list size mismatch\n");
            ++g_failures;
        }

        secs_ii_item_t *child = NULL;
        expect_ok("secs_ii_item_list_get",
                  secs_ii_item_list_get(decoded, 0, &child));
        expect_ok("secs_ii_item_get_type(child)",
                  secs_ii_item_get_type(child, &ty));
        if (ty != SECS_II_ITEM_ASCII) {
            fprintf(stderr, "FAIL: decoded child type mismatch\n");
            ++g_failures;
        }

        const char *p = NULL;
        size_t pn = 0;
        expect_ok("secs_ii_item_ascii_view",
                  secs_ii_item_ascii_view(child, &p, &pn));
        if (pn != 3u || memcmp(p, "ABC", 3) != 0) {
            fprintf(stderr, "FAIL: decoded ASCII payload mismatch\n");
            ++g_failures;
        }

        secs_ii_item_destroy(child);
        secs_ii_item_destroy(decoded);
    }

    /* 恶意输入：截断数据（不应崩溃，应返回 secs.ii::truncated） */
    {
        size_t consumed = 0;
        secs_ii_item_t *decoded = NULL;
        secs_error_t err =
            secs_ii_decode_one(bytes, (n > 0 ? n - 1 : 0), &consumed, &decoded);
        expect_err("secs_ii_decode_one(truncated)", err);
        if (err.category && strcmp(err.category, "secs.ii") != 0) {
            fprintf(stderr,
                    "FAIL: truncated category mismatch: %s\n",
                    err.category);
            ++g_failures;
        }
        if (decoded) {
            secs_ii_item_destroy(decoded);
        }
    }

    /* 恶意输入：非法头（FormatByte 低 2 位=3 -> length_bytes==4，应判
     * invalid_header） */
    {
        const uint8_t bad[1] = {0xFFu};
        size_t consumed = 0;
        secs_ii_item_t *decoded = NULL;
        secs_error_t err = secs_ii_decode_one(bad, 1, &consumed, &decoded);
        expect_err("secs_ii_decode_one(invalid header)", err);
        if (err.category && strcmp(err.category, "secs.ii") != 0) {
            fprintf(stderr,
                    "FAIL: invalid header category mismatch: %s\n",
                    err.category);
            ++g_failures;
        }
        if (decoded) {
            secs_ii_item_destroy(decoded);
        }
    }

    /* 恶意输入：深度爆破（>64 层嵌套）不应导致栈溢出 */
    {
        secs_ii_item_t *deep = make_nested_list_item(80u);
        if (!deep) {
            fprintf(stderr, "FAIL: make_nested_list_item failed\n");
            ++g_failures;
        } else {
            uint8_t *deep_bytes = NULL;
            size_t deep_n = 0;
            expect_ok("secs_ii_encode(deep)",
                      secs_ii_encode(deep, &deep_bytes, &deep_n));

            size_t consumed = 0;
            secs_ii_item_t *decoded = NULL;
            secs_error_t err =
                secs_ii_decode_one(deep_bytes, deep_n, &consumed, &decoded);
            expect_err("secs_ii_decode_one(deep nesting)", err);
            if (decoded) {
                secs_ii_item_destroy(decoded);
            }

            /* 用自定义 DecodeLimits 放宽 max_depth，应允许成功解码 */
            {
                secs_ii_decode_limits_t limits;
                memset(&limits, 0, sizeof(limits));
                secs_ii_decode_limits_init_default(&limits);
                limits.max_depth = 128u;

                size_t consumed2 = 0;
                secs_ii_item_t *decoded2 = NULL;
                secs_error_t err2 = secs_ii_decode_one_with_limits(
                    deep_bytes, deep_n, &limits, &consumed2, &decoded2);
                expect_ok("secs_ii_decode_one_with_limits(deep ok)", err2);
                if (secs_error_is_ok(err2) && consumed2 != deep_n) {
                    fprintf(stderr,
                            "FAIL: decode_one_with_limits consumed mismatch: %zu != %zu\n",
                            consumed2,
                            deep_n);
                    ++g_failures;
                }
                if (decoded2) {
                    secs_ii_item_destroy(decoded2);
                }
            }
            if (deep_bytes) {
                secs_free(deep_bytes);
            }
            secs_ii_item_destroy(deep);
        }
    }

    /* 参数校验：空指针 + 非零长度应报 INVALID_ARGUMENT */
    {
        size_t consumed = 0;
        secs_ii_item_t *decoded = NULL;
        secs_error_t err = secs_ii_decode_one(NULL, 1, &consumed, &decoded);
        expect_err("secs_ii_decode_one(NULL, n>0)", err);
        if (err.value != (int)SECS_C_API_INVALID_ARGUMENT) {
            fprintf(
                stderr, "FAIL: invalid arg value mismatch: %d\n", err.value);
            ++g_failures;
        }
    }

    if (bytes) {
        secs_free(bytes);
    }
    secs_ii_item_destroy(list);
}

static void test_hsms_session_create_v2_smoke(void) {
    secs_context_t *ctx = NULL;
    expect_ok("secs_context_create(v2)", secs_context_create(&ctx));

    secs_hsms_session_options_v2_t opt;
    memset(&opt, 0, sizeof(opt));
    opt.session_id = 0x1010;
    opt.t3_ms = 2000;
    opt.t5_ms = 200;
    opt.t6_ms = 2000;
    opt.t7_ms = 2000;
    opt.t8_ms = 2000;
    opt.linktest_interval_ms = 0;
    opt.linktest_max_consecutive_failures = 3;
    opt.auto_reconnect = 0;
    opt.passive_accept_select = 1;

    secs_hsms_session_t *sess = NULL;
    expect_ok("secs_hsms_session_create_v2",
              secs_hsms_session_create_v2(ctx, &opt, &sess));
    secs_hsms_session_destroy(sess);

    secs_context_destroy(ctx);
}

static void test_ii_all_types_and_views(void) {
    /* Binary */
    {
        const uint8_t in[2] = {0x00u, 0xFFu};
        secs_ii_item_t *item = NULL;
        expect_ok("secs_ii_item_create_binary",
                  secs_ii_item_create_binary(in, sizeof(in), &item));

        secs_ii_item_type_t ty;
        expect_ok("secs_ii_item_get_type(binary)",
                  secs_ii_item_get_type(item, &ty));
        if (ty != SECS_II_ITEM_BINARY) {
            fprintf(stderr, "FAIL: binary type mismatch\n");
            ++g_failures;
        }

        const uint8_t *p = NULL;
        size_t n = 0;
        expect_ok("secs_ii_item_binary_view",
                  secs_ii_item_binary_view(item, &p, &n));
        if (n != sizeof(in) || memcmp(p, in, sizeof(in)) != 0) {
            fprintf(stderr, "FAIL: binary view mismatch\n");
            ++g_failures;
        }

        /* 类型不匹配：应报错 */
        {
            const char *ap = NULL;
            size_t an = 0;
            expect_err("secs_ii_item_ascii_view(binary)",
                       secs_ii_item_ascii_view(item, &ap, &an));
        }

        secs_ii_item_destroy(item);
    }

    /* Binary：n==0 也是合法输入（覆盖 bytes_to_vec 的空分支） */
    {
        const uint8_t dummy = 0;
        secs_ii_item_t *item = NULL;
        expect_ok("secs_ii_item_create_binary(n==0)",
                  secs_ii_item_create_binary(&dummy, 0, &item));
        const uint8_t *p = NULL;
        size_t n = 0;
        expect_ok("secs_ii_item_binary_view(n==0)",
                  secs_ii_item_binary_view(item, &p, &n));
        if (n != 0u) {
            fprintf(stderr, "FAIL: binary(n==0) view size mismatch\n");
            ++g_failures;
        }
        secs_ii_item_destroy(item);
    }

    /* Boolean */
    {
        const uint8_t in01[3] = {0u, 1u, 2u};
        secs_ii_item_t *item = NULL;
        expect_ok("secs_ii_item_create_boolean",
                  secs_ii_item_create_boolean(in01, 3, &item));

        uint8_t *out01 = NULL;
        size_t out_n = 0;
        expect_ok("secs_ii_item_boolean_copy",
                  secs_ii_item_boolean_copy(item, &out01, &out_n));
        if (out_n != 3u) {
            fprintf(stderr, "FAIL: boolean_copy size mismatch\n");
            ++g_failures;
        } else {
            if (out01[0] != 0u || out01[1] != 1u || out01[2] != 1u) {
                fprintf(stderr, "FAIL: boolean_copy payload mismatch\n");
                ++g_failures;
            }
        }
        if (out01) {
            secs_free(out01);
        }
        secs_ii_item_destroy(item);
    }

    /* Boolean：空数组（覆盖 boolean_copy 的 empty 分支） */
    {
        secs_ii_item_t *item = NULL;
        expect_ok("secs_ii_item_create_boolean(empty)",
                  secs_ii_item_create_boolean(NULL, 0, &item));

        uint8_t *out01 = NULL;
        size_t out_n = 0;
        expect_ok("secs_ii_item_boolean_copy(empty)",
                  secs_ii_item_boolean_copy(item, &out01, &out_n));
        if (out_n != 0u || out01 != NULL) {
            fprintf(stderr, "FAIL: boolean_copy(empty) expected (NULL,0)\n");
            ++g_failures;
        }
        secs_ii_item_destroy(item);
    }

#define TEST_NUMERIC_VIEW(tag, c_type, create_fn, view_fn, type_tag)           \
    do {                                                                       \
        const c_type in[2] = {(c_type)1, (c_type)2};                           \
        secs_ii_item_t *item = NULL;                                           \
        expect_ok(#create_fn, create_fn(in, 2, &item));                        \
        secs_ii_item_type_t ty;                                                \
        expect_ok("secs_ii_item_get_type(" #tag ")",                           \
                  secs_ii_item_get_type(item, &ty));                           \
        if (ty != type_tag) {                                                  \
            fprintf(stderr, "FAIL: " #tag " type mismatch\n");                 \
            ++g_failures;                                                      \
        }                                                                      \
        const c_type *p = NULL;                                                \
        size_t n = 0;                                                          \
        expect_ok(#view_fn, view_fn(item, &p, &n));                            \
        if (n != 2u || p[0] != in[0] || p[1] != in[1]) {                       \
            fprintf(stderr, "FAIL: " #tag " view mismatch\n");                 \
            ++g_failures;                                                      \
        }                                                                      \
        secs_ii_item_destroy(item);                                            \
    } while (0)

    TEST_NUMERIC_VIEW(i1,
                      int8_t,
                      secs_ii_item_create_i1,
                      secs_ii_item_i1_view,
                      SECS_II_ITEM_I1);
    TEST_NUMERIC_VIEW(i2,
                      int16_t,
                      secs_ii_item_create_i2,
                      secs_ii_item_i2_view,
                      SECS_II_ITEM_I2);
    TEST_NUMERIC_VIEW(i4,
                      int32_t,
                      secs_ii_item_create_i4,
                      secs_ii_item_i4_view,
                      SECS_II_ITEM_I4);
    TEST_NUMERIC_VIEW(i8,
                      int64_t,
                      secs_ii_item_create_i8,
                      secs_ii_item_i8_view,
                      SECS_II_ITEM_I8);
    TEST_NUMERIC_VIEW(u1,
                      uint8_t,
                      secs_ii_item_create_u1,
                      secs_ii_item_u1_view,
                      SECS_II_ITEM_U1);
    TEST_NUMERIC_VIEW(u2,
                      uint16_t,
                      secs_ii_item_create_u2,
                      secs_ii_item_u2_view,
                      SECS_II_ITEM_U2);
    TEST_NUMERIC_VIEW(u4,
                      uint32_t,
                      secs_ii_item_create_u4,
                      secs_ii_item_u4_view,
                      SECS_II_ITEM_U4);
    TEST_NUMERIC_VIEW(u8,
                      uint64_t,
                      secs_ii_item_create_u8,
                      secs_ii_item_u8_view,
                      SECS_II_ITEM_U8);

    /* 浮点：避免精度问题，仅验证数量与大致值 */
    {
        const float in[2] = {0.5f, -1.25f};
        secs_ii_item_t *item = NULL;
        expect_ok("secs_ii_item_create_f4",
                  secs_ii_item_create_f4(in, 2, &item));
        secs_ii_item_type_t ty;
        expect_ok("secs_ii_item_get_type(f4)",
                  secs_ii_item_get_type(item, &ty));
        if (ty != SECS_II_ITEM_F4) {
            fprintf(stderr, "FAIL: f4 type mismatch\n");
            ++g_failures;
        }
        const float *p = NULL;
        size_t n = 0;
        expect_ok("secs_ii_item_f4_view", secs_ii_item_f4_view(item, &p, &n));
        if (n != 2u) {
            fprintf(stderr, "FAIL: f4 view size mismatch\n");
            ++g_failures;
        } else {
            if (p[0] != in[0] || p[1] != in[1]) {
                fprintf(stderr, "FAIL: f4 view payload mismatch\n");
                ++g_failures;
            }
        }
        secs_ii_item_destroy(item);
    }
    {
        const double in[2] = {0.25, -2.5};
        secs_ii_item_t *item = NULL;
        expect_ok("secs_ii_item_create_f8",
                  secs_ii_item_create_f8(in, 2, &item));
        secs_ii_item_type_t ty;
        expect_ok("secs_ii_item_get_type(f8)",
                  secs_ii_item_get_type(item, &ty));
        if (ty != SECS_II_ITEM_F8) {
            fprintf(stderr, "FAIL: f8 type mismatch\n");
            ++g_failures;
        }
        const double *p = NULL;
        size_t n = 0;
        expect_ok("secs_ii_item_f8_view", secs_ii_item_f8_view(item, &p, &n));
        if (n != 2u) {
            fprintf(stderr, "FAIL: f8 view size mismatch\n");
            ++g_failures;
        } else {
            if (p[0] != in[0] || p[1] != in[1]) {
                fprintf(stderr, "FAIL: f8 view payload mismatch\n");
                ++g_failures;
            }
        }
        secs_ii_item_destroy(item);
    }

#define TEST_NUMERIC_EMPTY(tag, c_type, create_fn, view_fn, type_tag)          \
    do {                                                                       \
        secs_ii_item_t *item = NULL;                                           \
        expect_ok(#create_fn "(NULL,0)",                                       \
                  create_fn((const c_type *)NULL, 0, &item));                  \
        secs_ii_item_type_t ty;                                                \
        expect_ok("secs_ii_item_get_type(" #tag ")",                           \
                  secs_ii_item_get_type(item, &ty));                           \
        if (ty != type_tag) {                                                  \
            fprintf(stderr, "FAIL: " #tag " type mismatch (empty)\n");         \
            ++g_failures;                                                      \
        }                                                                      \
        const c_type *p = (const c_type *)0x1;                                 \
        size_t n = 123;                                                        \
        expect_ok(#view_fn "(empty)", view_fn(item, &p, &n));                  \
        if (n != 0u) {                                                         \
            fprintf(stderr, "FAIL: " #tag " empty view size mismatch\n");      \
            ++g_failures;                                                      \
        }                                                                      \
        secs_ii_item_destroy(item);                                            \
    } while (0)

    /* 空数组：允许 NULL + 0（避免 C 侧常见写法触发 UB） */
    TEST_NUMERIC_EMPTY(i1,
                       int8_t,
                       secs_ii_item_create_i1,
                       secs_ii_item_i1_view,
                       SECS_II_ITEM_I1);
    TEST_NUMERIC_EMPTY(i2,
                       int16_t,
                       secs_ii_item_create_i2,
                       secs_ii_item_i2_view,
                       SECS_II_ITEM_I2);
    TEST_NUMERIC_EMPTY(i4,
                       int32_t,
                       secs_ii_item_create_i4,
                       secs_ii_item_i4_view,
                       SECS_II_ITEM_I4);
    TEST_NUMERIC_EMPTY(i8,
                       int64_t,
                       secs_ii_item_create_i8,
                       secs_ii_item_i8_view,
                       SECS_II_ITEM_I8);
    TEST_NUMERIC_EMPTY(u1,
                       uint8_t,
                       secs_ii_item_create_u1,
                       secs_ii_item_u1_view,
                       SECS_II_ITEM_U1);
    TEST_NUMERIC_EMPTY(u2,
                       uint16_t,
                       secs_ii_item_create_u2,
                       secs_ii_item_u2_view,
                       SECS_II_ITEM_U2);
    TEST_NUMERIC_EMPTY(u4,
                       uint32_t,
                       secs_ii_item_create_u4,
                       secs_ii_item_u4_view,
                       SECS_II_ITEM_U4);
    TEST_NUMERIC_EMPTY(u8,
                       uint64_t,
                       secs_ii_item_create_u8,
                       secs_ii_item_u8_view,
                       SECS_II_ITEM_U8);
    TEST_NUMERIC_EMPTY(f4,
                       float,
                       secs_ii_item_create_f4,
                       secs_ii_item_f4_view,
                       SECS_II_ITEM_F4);
    TEST_NUMERIC_EMPTY(f8,
                       double,
                       secs_ii_item_create_f8,
                       secs_ii_item_f8_view,
                       SECS_II_ITEM_F8);

#undef TEST_NUMERIC_EMPTY

#undef TEST_NUMERIC_VIEW

    /* List 的边界：对非 List 调用 list_size/get 应报错 */
    {
        secs_ii_item_t *ascii = NULL;
        expect_ok("secs_ii_item_create_ascii(edge)",
                  secs_ii_item_create_ascii("X", 1, &ascii));
        size_t n = 0;
        expect_err("secs_ii_item_list_size(non-list)",
                   secs_ii_item_list_size(ascii, &n));
        secs_ii_item_t *child = NULL;
        expect_err("secs_ii_item_list_get(non-list)",
                   secs_ii_item_list_get(ascii, 0, &child));
        secs_ii_item_destroy(ascii);
    }
}

static void test_ii_list_builder_helpers(void) {
    /* Phase2：List Builder helpers（append_* / append_take） */
    secs_ii_item_t *list = NULL;
    expect_ok("secs_ii_item_create_list(builder)",
              secs_ii_item_create_list(&list));

    /* 参数校验：list==NULL / 非 List */
    {
        secs_ii_item_t *ascii = NULL;
        expect_ok("secs_ii_item_create_ascii(non-list for append)",
                  secs_ii_item_create_ascii("X", 1, &ascii));
        expect_err("secs_ii_item_list_append_u2(NULL list)",
                   secs_ii_item_list_append_u2(NULL, 1));
        expect_err("secs_ii_item_list_append_u2(non-list)",
                   secs_ii_item_list_append_u2(ascii, 1));
        secs_ii_item_destroy(ascii);
    }

    /* append_take：应 destroy 并把指针置空 */
    {
        expect_err("secs_ii_item_list_append_take(NULL io_elem)",
                   secs_ii_item_list_append_take(list, NULL));
        secs_ii_item_t *null_elem = NULL;
        expect_err("secs_ii_item_list_append_take(*io_elem==NULL)",
                   secs_ii_item_list_append_take(list, &null_elem));

        secs_ii_item_t *tmp = NULL;
        expect_ok("secs_ii_item_create_ascii(tmp)",
                  secs_ii_item_create_ascii("hi", 2, &tmp));
        expect_ok("secs_ii_item_list_append_take(ascii)",
                  secs_ii_item_list_append_take(list, &tmp));
        if (tmp != NULL) {
            fprintf(stderr, "FAIL: append_take should NULL-out tmp\n");
            ++g_failures;
            secs_ii_item_destroy(tmp);
        }

        size_t n = 0;
        expect_ok("secs_ii_item_list_size(builder after take)",
                  secs_ii_item_list_size(list, &n));
        if (n != 1u) {
            fprintf(stderr, "FAIL: builder list size after take\n");
            ++g_failures;
        }

        secs_ii_item_t *child = NULL;
        expect_ok("secs_ii_item_list_get(builder[0])",
                  secs_ii_item_list_get(list, 0, &child));
        const char *p = NULL;
        size_t pn = 0;
        expect_ok("secs_ii_item_ascii_view(builder[0])",
                  secs_ii_item_ascii_view(child, &p, &pn));
        if (!p || pn != 2u || memcmp(p, "hi", 2) != 0) {
            fprintf(stderr, "FAIL: builder[0] ascii mismatch\n");
            ++g_failures;
        }
        secs_ii_item_destroy(child);
    }

    /* 其它 append_*：覆盖每个新符号至少一次 */
    expect_err("secs_ii_item_list_append_ascii(NULL value)",
               secs_ii_item_list_append_ascii(list, NULL));
    expect_ok("secs_ii_item_list_append_ascii(\"A\")",
              secs_ii_item_list_append_ascii(list, "A"));

    {
        const char bytes3[3] = {'B', '\0', 'C'};
        expect_ok("secs_ii_item_list_append_ascii_n(B\\0C)",
                  secs_ii_item_list_append_ascii_n(list, bytes3, 3));
    }
    expect_ok("secs_ii_item_list_append_ascii_n(empty)",
              secs_ii_item_list_append_ascii_n(list, NULL, 0));

    {
        const uint8_t bin[2] = {1u, 2u};
        expect_ok("secs_ii_item_list_append_binary",
                  secs_ii_item_list_append_binary(list, bin, 2));
    }
    expect_ok("secs_ii_item_list_append_binary(empty)",
              secs_ii_item_list_append_binary(list, NULL, 0));

    expect_err("secs_ii_item_list_append_boolean(bad value01)",
               secs_ii_item_list_append_boolean(list, 2));
    expect_ok("secs_ii_item_list_append_boolean(1)",
              secs_ii_item_list_append_boolean(list, 1));

    {
        const uint8_t bad01[2] = {0u, 2u};
        expect_err("secs_ii_item_list_append_boolean_values(bad)",
                   secs_ii_item_list_append_boolean_values(list, bad01, 2));
    }
    expect_ok("secs_ii_item_list_append_boolean_values(empty)",
              secs_ii_item_list_append_boolean_values(list, NULL, 0));

    expect_ok("secs_ii_item_list_append_i1",
              secs_ii_item_list_append_i1(list, (int8_t)-1));
    expect_ok("secs_ii_item_list_append_i2",
              secs_ii_item_list_append_i2(list, (int16_t)-2));
    expect_ok("secs_ii_item_list_append_i4",
              secs_ii_item_list_append_i4(list, (int32_t)-3));
    expect_ok("secs_ii_item_list_append_i8",
              secs_ii_item_list_append_i8(list, (int64_t)-4));
    expect_ok("secs_ii_item_list_append_u1",
              secs_ii_item_list_append_u1(list, (uint8_t)5));
    expect_ok("secs_ii_item_list_append_u2",
              secs_ii_item_list_append_u2(list, (uint16_t)6));
    expect_ok("secs_ii_item_list_append_u4",
              secs_ii_item_list_append_u4(list, (uint32_t)7));
    expect_ok("secs_ii_item_list_append_u8",
              secs_ii_item_list_append_u8(list, (uint64_t)8));
    expect_ok("secs_ii_item_list_append_f4",
              secs_ii_item_list_append_f4(list, 1.25f));
    expect_ok("secs_ii_item_list_append_f8",
              secs_ii_item_list_append_f8(list, -2.5));

    {
        const int8_t v[1] = {-9};
        expect_ok("secs_ii_item_list_append_i1_values",
                  secs_ii_item_list_append_i1_values(list, v, 1));
    }
    {
        const int16_t v[1] = {-10};
        expect_ok("secs_ii_item_list_append_i2_values",
                  secs_ii_item_list_append_i2_values(list, v, 1));
    }
    {
        const int32_t v[1] = {-11};
        expect_ok("secs_ii_item_list_append_i4_values",
                  secs_ii_item_list_append_i4_values(list, v, 1));
    }
    {
        const int64_t v[1] = {-12};
        expect_ok("secs_ii_item_list_append_i8_values",
                  secs_ii_item_list_append_i8_values(list, v, 1));
    }
    {
        const uint8_t v[1] = {13u};
        expect_ok("secs_ii_item_list_append_u1_values",
                  secs_ii_item_list_append_u1_values(list, v, 1));
    }
    {
        const uint16_t v[1] = {14u};
        expect_ok("secs_ii_item_list_append_u2_values",
                  secs_ii_item_list_append_u2_values(list, v, 1));
    }
    {
        const uint32_t v[1] = {15u};
        expect_ok("secs_ii_item_list_append_u4_values",
                  secs_ii_item_list_append_u4_values(list, v, 1));
    }
    {
        const uint64_t v[1] = {16u};
        expect_ok("secs_ii_item_list_append_u8_values",
                  secs_ii_item_list_append_u8_values(list, v, 1));
    }
    {
        const float v[1] = {2.0f};
        expect_ok("secs_ii_item_list_append_f4_values",
                  secs_ii_item_list_append_f4_values(list, v, 1));
    }
    {
        const double v[1] = {3.0};
        expect_ok("secs_ii_item_list_append_f8_values",
                  secs_ii_item_list_append_f8_values(list, v, 1));
    }

    secs_ii_item_destroy(list);
}

static void test_ii_builder(void) {
    /* Phase2.5：Item Builder（栈式 begin/end） */

    /* 参数校验：out_builder==NULL */
    expect_err("secs_ii_builder_create(NULL)",
               secs_ii_builder_create(NULL));

    /* 正常构建：<L <U2 101> <A \"A\"> <L <U4 1>>> */
    {
        secs_ii_builder_t *b = NULL;
        expect_ok("secs_ii_builder_create",
                  secs_ii_builder_create(&b));

        expect_ok("secs_ii_builder_list_begin(root)",
                  secs_ii_builder_list_begin(b));
        expect_ok("secs_ii_builder_add_u2(101)",
                  secs_ii_builder_add_u2(b, (uint16_t)101));
        expect_ok("secs_ii_builder_add_ascii(A)",
                  secs_ii_builder_add_ascii(b, "A"));
        expect_ok("secs_ii_builder_list_begin(nested)",
                  secs_ii_builder_list_begin(b));
        expect_ok("secs_ii_builder_add_u4(1)",
                  secs_ii_builder_add_u4(b, (uint32_t)1));
        expect_ok("secs_ii_builder_list_end(nested)",
                  secs_ii_builder_list_end(b));
        expect_ok("secs_ii_builder_list_end(root)",
                  secs_ii_builder_list_end(b));

        secs_ii_item_t *out = NULL;
        expect_ok("secs_ii_builder_finalize",
                  secs_ii_builder_finalize(b, &out));

        /* finalize 后不可复用 */
        expect_err("secs_ii_builder_add_u2(after finalize)",
                   secs_ii_builder_add_u2(b, (uint16_t)1));

        secs_ii_builder_destroy(b);

        /* 验证结构 */
        secs_ii_item_type_t type = (secs_ii_item_type_t)99;
        expect_ok("secs_ii_item_get_type(builder root)",
                  secs_ii_item_get_type(out, &type));
        if (type != SECS_II_ITEM_LIST) {
            fprintf(stderr, "FAIL: builder root should be LIST\n");
            ++g_failures;
        }

        size_t n = 0;
        expect_ok("secs_ii_item_list_size(builder root)",
                  secs_ii_item_list_size(out, &n));
        if (n != 3u) {
            fprintf(stderr, "FAIL: builder root size mismatch\n");
            ++g_failures;
        }

        /* child[0] = U2 101 */
        secs_ii_item_t *c0 = NULL;
        expect_ok("secs_ii_item_list_get(builder[0])",
                  secs_ii_item_list_get(out, 0, &c0));
        const uint16_t *p_u2 = NULL;
        size_t p_u2_n = 0;
        expect_ok("secs_ii_item_u2_view(builder[0])",
                  secs_ii_item_u2_view(c0, &p_u2, &p_u2_n));
        if (!p_u2 || p_u2_n != 1u || p_u2[0] != 101u) {
            fprintf(stderr, "FAIL: builder[0] u2 mismatch\n");
            ++g_failures;
        }
        secs_ii_item_destroy(c0);

        /* child[1] = ASCII \"A\" */
        secs_ii_item_t *c1 = NULL;
        expect_ok("secs_ii_item_list_get(builder[1])",
                  secs_ii_item_list_get(out, 1, &c1));
        const char *p_a = NULL;
        size_t p_a_n = 0;
        expect_ok("secs_ii_item_ascii_view(builder[1])",
                  secs_ii_item_ascii_view(c1, &p_a, &p_a_n));
        if (!p_a || p_a_n != 1u || memcmp(p_a, "A", 1) != 0) {
            fprintf(stderr, "FAIL: builder[1] ascii mismatch\n");
            ++g_failures;
        }
        secs_ii_item_destroy(c1);

        /* child[2] = <L <U4 1>> */
        secs_ii_item_t *c2 = NULL;
        expect_ok("secs_ii_item_list_get(builder[2])",
                  secs_ii_item_list_get(out, 2, &c2));
        expect_ok("secs_ii_item_list_size(builder[2])",
                  secs_ii_item_list_size(c2, &n));
        if (n != 1u) {
            fprintf(stderr, "FAIL: builder[2] nested size mismatch\n");
            ++g_failures;
        }
        secs_ii_item_t *c20 = NULL;
        expect_ok("secs_ii_item_list_get(builder[2][0])",
                  secs_ii_item_list_get(c2, 0, &c20));
        const uint32_t *p_u4 = NULL;
        size_t p_u4_n = 0;
        expect_ok("secs_ii_item_u4_view(builder[2][0])",
                  secs_ii_item_u4_view(c20, &p_u4, &p_u4_n));
        if (!p_u4 || p_u4_n != 1u || p_u4[0] != 1u) {
            fprintf(stderr, "FAIL: builder[2][0] u4 mismatch\n");
            ++g_failures;
        }
        secs_ii_item_destroy(c20);
        secs_ii_item_destroy(c2);

        secs_ii_item_destroy(out);
    }

    /* 首错记忆：list_end() 先错，后续调用应继续返回同一错误 */
    {
        secs_ii_builder_t *b = NULL;
        expect_ok("secs_ii_builder_create(first_err)",
                  secs_ii_builder_create(&b));

        secs_error_t err0 = secs_ii_builder_list_end(b);
        expect_err("secs_ii_builder_list_end(without begin)", err0);

        secs_error_t err1 = secs_ii_builder_list_begin(b);
        expect_err("secs_ii_builder_list_begin(after error)", err1);
        if (err0.value != err1.value || strcmp(err0.category, err1.category) != 0) {
            fprintf(stderr, "FAIL: builder should remember first error\n");
            ++g_failures;
        }

        /* take 语义：即使 builder 已出错，也应 destroy 并置空 */
        secs_ii_item_t *tmp = NULL;
        expect_ok("secs_ii_item_create_ascii(tmp for take)",
                  secs_ii_item_create_ascii("hi", 2, &tmp));
        secs_error_t err2 = secs_ii_builder_add_item_take(b, &tmp);
        expect_err("secs_ii_builder_add_item_take(after error)", err2);
        if (tmp != NULL) {
            fprintf(stderr, "FAIL: builder_add_item_take should NULL-out tmp\n");
            ++g_failures;
            secs_ii_item_destroy(tmp);
        }

        secs_ii_item_t *out = NULL;
        expect_err("secs_ii_builder_finalize(after error)",
                   secs_ii_builder_finalize(b, &out));

        secs_ii_builder_destroy(b);
    }

    /* finalize 未闭合 list：应报错并记忆 */
    {
        secs_ii_builder_t *b = NULL;
        expect_ok("secs_ii_builder_create(unclosed)",
                  secs_ii_builder_create(&b));
        expect_ok("secs_ii_builder_list_begin(unclosed)",
                  secs_ii_builder_list_begin(b));

        secs_ii_item_t *out = NULL;
        expect_err("secs_ii_builder_finalize(unclosed)",
                   secs_ii_builder_finalize(b, &out));
        expect_err("secs_ii_builder_list_end(after finalize error)",
                   secs_ii_builder_list_end(b));

        secs_ii_builder_destroy(b);
    }
}

static void test_ii_extraction_helpers(void) {
    /* Phase3：at_path / get_ascii_at helpers */

    /* get_ascii_at：成功/越界/类型不匹配 */
    {
        secs_ii_item_t *list = NULL;
        expect_ok("secs_ii_item_create_list(get_ascii_at)",
                  secs_ii_item_create_list(&list));
        expect_ok("secs_ii_item_list_append_ascii(get_ascii_at)",
                  secs_ii_item_list_append_ascii(list, "X"));

        const char *p = NULL;
        size_t n = 0;
        expect_ok("secs_ii_item_get_ascii_at(ok)",
                  secs_ii_item_get_ascii_at(list, 0, &p, &n));
        if (!p || n != 1u || memcmp(p, "X", 1) != 0) {
            fprintf(stderr, "FAIL: get_ascii_at payload mismatch\n");
            ++g_failures;
        }

        expect_err("secs_ii_item_get_ascii_at(oob)",
                   secs_ii_item_get_ascii_at(list, 1, &p, &n));

        expect_ok("secs_ii_item_list_append_u2(type mismatch)",
                  secs_ii_item_list_append_u2(list, 1));
        expect_err("secs_ii_item_get_ascii_at(type mismatch)",
                   secs_ii_item_get_ascii_at(list, 1, &p, &n));

        secs_ii_item_destroy(list);
    }

    /* depth==0：各类型 view/get */
#define TEST_NUMERIC_AT_PATH(tag, c_type, create_fn, view_at_fn, get_at_fn, v0) \
    do {                                                                       \
        secs_ii_item_t *item = NULL;                                           \
        const c_type in = (c_type)(v0);                                        \
        expect_ok(#create_fn "(scalar)", create_fn(&in, 1, &item));            \
        const c_type *p = NULL;                                                \
        size_t n = 0;                                                          \
        expect_ok(#view_at_fn "(depth0)", view_at_fn(item, &p, &n, 0));        \
        if (!p || n != 1u || p[0] != in) {                                     \
            fprintf(stderr, "FAIL: " #tag " view_at_path mismatch\n");         \
            ++g_failures;                                                      \
        }                                                                      \
        c_type out = 0;                                                        \
        expect_ok(#get_at_fn "(depth0)", get_at_fn(item, &out, 0));            \
        if (out != in) {                                                       \
            fprintf(stderr, "FAIL: " #tag " get_at_path mismatch\n");          \
            ++g_failures;                                                      \
        }                                                                      \
        secs_ii_item_destroy(item);                                            \
    } while (0)

    TEST_NUMERIC_AT_PATH(i1,
                         int8_t,
                         secs_ii_item_create_i1,
                         secs_ii_item_i1_view_at_path,
                         secs_ii_item_get_i1_at_path,
                         -3);
    TEST_NUMERIC_AT_PATH(i2,
                         int16_t,
                         secs_ii_item_create_i2,
                         secs_ii_item_i2_view_at_path,
                         secs_ii_item_get_i2_at_path,
                         -300);
    TEST_NUMERIC_AT_PATH(i4,
                         int32_t,
                         secs_ii_item_create_i4,
                         secs_ii_item_i4_view_at_path,
                         secs_ii_item_get_i4_at_path,
                         -123456);
    TEST_NUMERIC_AT_PATH(i8,
                         int64_t,
                         secs_ii_item_create_i8,
                         secs_ii_item_i8_view_at_path,
                         secs_ii_item_get_i8_at_path,
                         -1234567890);
    TEST_NUMERIC_AT_PATH(u1,
                         uint8_t,
                         secs_ii_item_create_u1,
                         secs_ii_item_u1_view_at_path,
                         secs_ii_item_get_u1_at_path,
                         7);
    /* U2：get 函数名不同 */
    {
        secs_ii_item_t *item = NULL;
        const uint16_t in = 9u;
        expect_ok("secs_ii_item_create_u2(depth0)",
                  secs_ii_item_create_u2(&in, 1, &item));
        const uint16_t *p = NULL;
        size_t n = 0;
        expect_ok("secs_ii_item_u2_view_at_path(depth0)",
                  secs_ii_item_u2_view_at_path(item, &p, &n, 0));
        if (!p || n != 1u || p[0] != in) {
            fprintf(stderr, "FAIL: u2 view_at_path mismatch\n");
            ++g_failures;
        }
        uint16_t out = 0;
        expect_ok("secs_ii_item_get_u2_at_path(depth0)",
                  secs_ii_item_get_u2_at_path(item, &out, 0));
        if (out != in) {
            fprintf(stderr, "FAIL: u2 get_at_path mismatch\n");
            ++g_failures;
        }
        secs_ii_item_destroy(item);
    }
    TEST_NUMERIC_AT_PATH(u4,
                         uint32_t,
                         secs_ii_item_create_u4,
                         secs_ii_item_u4_view_at_path,
                         secs_ii_item_get_u4_at_path,
                         12345);
    TEST_NUMERIC_AT_PATH(u8,
                         uint64_t,
                         secs_ii_item_create_u8,
                         secs_ii_item_u8_view_at_path,
                         secs_ii_item_get_u8_at_path,
                         123456);
    TEST_NUMERIC_AT_PATH(f4,
                         float,
                         secs_ii_item_create_f4,
                         secs_ii_item_f4_view_at_path,
                         secs_ii_item_get_f4_at_path,
                         0.5f);
    TEST_NUMERIC_AT_PATH(f8,
                         double,
                         secs_ii_item_create_f8,
                         secs_ii_item_f8_view_at_path,
                         secs_ii_item_get_f8_at_path,
                         -2.5);

#undef TEST_NUMERIC_AT_PATH

    /* ASCII/Binary view_at_path(depth0) */
    {
        secs_ii_item_t *a = NULL;
        expect_ok("secs_ii_item_create_ascii(depth0)",
                  secs_ii_item_create_ascii("hi", 2, &a));
        const char *p = NULL;
        size_t n = 0;
        expect_ok("secs_ii_item_ascii_view_at_path(depth0)",
                  secs_ii_item_ascii_view_at_path(a, &p, &n, 0));
        if (!p || n != 2u || memcmp(p, "hi", 2) != 0) {
            fprintf(stderr, "FAIL: ascii_view_at_path mismatch\n");
            ++g_failures;
        }
        /* type mismatch */
        {
            const uint8_t *bp = NULL;
            size_t bn = 0;
            expect_err("secs_ii_item_binary_view_at_path(type mismatch)",
                       secs_ii_item_binary_view_at_path(a, &bp, &bn, 0));
        }
        secs_ii_item_destroy(a);
    }
    {
        const uint8_t in[2] = {1u, 2u};
        secs_ii_item_t *b = NULL;
        expect_ok("secs_ii_item_create_binary(depth0)",
                  secs_ii_item_create_binary(in, 2, &b));
        const uint8_t *p = NULL;
        size_t n = 0;
        expect_ok("secs_ii_item_binary_view_at_path(depth0)",
                  secs_ii_item_binary_view_at_path(b, &p, &n, 0));
        if (!p || n != 2u || p[0] != 1u || p[1] != 2u) {
            fprintf(stderr, "FAIL: binary_view_at_path mismatch\n");
            ++g_failures;
        }
        secs_ii_item_destroy(b);
    }

    /* Boolean：copy_at_path + get_boolean_at_path */
    {
        const uint8_t in01[2] = {0u, 1u};
        secs_ii_item_t *b = NULL;
        expect_ok("secs_ii_item_create_boolean(depth0)",
                  secs_ii_item_create_boolean(in01, 2, &b));
        uint8_t *out01 = NULL;
        size_t out_n = 0;
        expect_ok("secs_ii_item_boolean_copy_at_path(depth0)",
                  secs_ii_item_boolean_copy_at_path(b, &out01, &out_n, 0));
        if (!out01 || out_n != 2u || out01[0] != 0u || out01[1] != 1u) {
            fprintf(stderr, "FAIL: boolean_copy_at_path mismatch\n");
            ++g_failures;
        }
        secs_free(out01);
        secs_ii_item_destroy(b);
    }
    {
        const uint8_t in01 = 1u;
        secs_ii_item_t *b = NULL;
        expect_ok("secs_ii_item_create_boolean(scalar)",
                  secs_ii_item_create_boolean(&in01, 1, &b));
        uint8_t out = 0;
        expect_ok("secs_ii_item_get_boolean_at_path(depth0)",
                  secs_ii_item_get_boolean_at_path(b, &out, 0));
        if (out != 1u) {
            fprintf(stderr, "FAIL: get_boolean_at_path mismatch\n");
            ++g_failures;
        }
        secs_ii_item_destroy(b);
    }

    /* depth>0：路径提取成功/越界/中间非 List */
    {
        secs_ii_item_t *root = NULL;
        secs_ii_item_t *inner = NULL;
        expect_ok("secs_ii_item_create_list(path root)",
                  secs_ii_item_create_list(&root));
        expect_ok("secs_ii_item_create_list(path inner)",
                  secs_ii_item_create_list(&inner));
        expect_ok("secs_ii_item_list_append_u4(inner)",
                  secs_ii_item_list_append_u4(inner, 42u));
        expect_ok("secs_ii_item_list_append_take(root, inner)",
                  secs_ii_item_list_append_take(root, &inner));

        uint32_t out = 0;
        expect_ok("secs_ii_item_get_u4_at_path(depth2)",
                  secs_ii_item_get_u4_at_path(root,
                                              &out,
                                              2,
                                              (size_t)0,
                                              (size_t)0));
        if (out != 42u) {
            fprintf(stderr, "FAIL: get_u4_at_path(depth2) mismatch\n");
            ++g_failures;
        }

        expect_err("secs_ii_item_get_u4_at_path(oob)",
                   secs_ii_item_get_u4_at_path(root,
                                               &out,
                                               2,
                                               (size_t)0,
                                               (size_t)99));

        /* 中间非 List：root 为 U4，但 depth>0 */
        {
            secs_ii_item_t *u4 = NULL;
            const uint32_t v = 1u;
            expect_ok("secs_ii_item_create_u4(non-list root)",
                      secs_ii_item_create_u4(&v, 1, &u4));
            expect_err("secs_ii_item_get_u4_at_path(non-list root)",
                       secs_ii_item_get_u4_at_path(u4, &out, 1, (size_t)0));
            secs_ii_item_destroy(u4);
        }

        secs_ii_item_destroy(root);
    }

    /* 标量长度!=1：get_* 应报错 */
    {
        const uint32_t v[2] = {1u, 2u};
        secs_ii_item_t *u4 = NULL;
        expect_ok("secs_ii_item_create_u4(n==2)",
                  secs_ii_item_create_u4(v, 2, &u4));
        uint32_t out = 0;
        expect_err("secs_ii_item_get_u4_at_path(len!=1)",
                   secs_ii_item_get_u4_at_path(u4, &out, 0));
        secs_ii_item_destroy(u4);
    }

    /* 参数校验：out 指针为空 */
    {
        const uint16_t v = 1u;
        secs_ii_item_t *u2 = NULL;
        expect_ok("secs_ii_item_create_u2(for null-out)",
                  secs_ii_item_create_u2(&v, 1, &u2));
        size_t n = 0;
        expect_err("secs_ii_item_u2_view_at_path(NULL out_ptr)",
                   secs_ii_item_u2_view_at_path(u2, NULL, &n, 0));
        secs_ii_item_destroy(u2);
    }
}

static void test_ii_builder_add_helpers(void) {
    /* 覆盖：此前未触达的 secs_ii_builder_add_* 便捷 API */
    secs_ii_builder_t *b = NULL;
    expect_ok("secs_ii_builder_create(add helpers)", secs_ii_builder_create(&b));

    expect_ok("secs_ii_builder_list_begin(add helpers)",
              secs_ii_builder_list_begin(b));

    {
        const char bytes3[3] = {'A', '\0', 'B'};
        expect_ok("secs_ii_builder_add_ascii_n",
                  secs_ii_builder_add_ascii_n(b, bytes3, 3));
    }
    {
        const uint8_t bin[2] = {1u, 2u};
        expect_ok("secs_ii_builder_add_binary",
                  secs_ii_builder_add_binary(b, bin, 2));
    }
    expect_ok("secs_ii_builder_add_boolean(1)",
              secs_ii_builder_add_boolean(b, 1));
    {
        const uint8_t b01[2] = {0u, 1u};
        expect_ok("secs_ii_builder_add_boolean_values",
                  secs_ii_builder_add_boolean_values(b, b01, 2));
    }

    expect_ok("secs_ii_builder_add_i1", secs_ii_builder_add_i1(b, (int8_t)-1));
    expect_ok("secs_ii_builder_add_i2", secs_ii_builder_add_i2(b, (int16_t)-2));
    expect_ok("secs_ii_builder_add_i4", secs_ii_builder_add_i4(b, (int32_t)-3));
    expect_ok("secs_ii_builder_add_i8", secs_ii_builder_add_i8(b, (int64_t)-4));
    expect_ok("secs_ii_builder_add_u1", secs_ii_builder_add_u1(b, (uint8_t)5));
    expect_ok("secs_ii_builder_add_u8", secs_ii_builder_add_u8(b, (uint64_t)8));
    expect_ok("secs_ii_builder_add_f4", secs_ii_builder_add_f4(b, 1.25f));
    expect_ok("secs_ii_builder_add_f8", secs_ii_builder_add_f8(b, -2.5));

    /* add_item：append 会拷贝 */
    {
        secs_ii_item_t *u2 = NULL;
        const uint16_t v = 123;
        expect_ok("secs_ii_item_create_u2(add_item)",
                  secs_ii_item_create_u2(&v, 1, &u2));
        expect_ok("secs_ii_builder_add_item", secs_ii_builder_add_item(b, u2));
        secs_ii_item_destroy(u2);
    }

    expect_ok("secs_ii_builder_list_end(add helpers)",
              secs_ii_builder_list_end(b));

    secs_ii_item_t *out = NULL;
    expect_ok("secs_ii_builder_finalize(add helpers)",
              secs_ii_builder_finalize(b, &out));
    secs_ii_builder_destroy(b);

    secs_ii_item_type_t ty;
    expect_ok("secs_ii_item_get_type(add helpers)",
              secs_ii_item_get_type(out, &ty));
    if (ty != SECS_II_ITEM_LIST) {
        fprintf(stderr, "FAIL: builder(add helpers) output should be LIST\n");
        ++g_failures;
    }
    secs_ii_item_destroy(out);
}

static void test_ii_more_view_get_at_path_and_list_path(void) {
    /* 覆盖：此前未触达的 *_view_at_list_path / *_get_*_at_list_path / 部分 *_at_path */
    secs_ii_item_t *root = NULL;
    expect_ok("secs_ii_item_create_list(more views)",
              secs_ii_item_create_list(&root));

    /* root = [ I1(-1), I2(-2), I4(-3), I8(-4), U1(5), U4(7), U8(8), F4(1.25), F8(-2.5) ] */
    {
        secs_ii_item_t *v = NULL;
        const int8_t x = -1;
        expect_ok("secs_ii_item_create_i1",
                  secs_ii_item_create_i1(&x, 1, &v));
        expect_ok("secs_ii_item_list_append_take(i1)",
                  secs_ii_item_list_append_take(root, &v));
    }
    {
        secs_ii_item_t *v = NULL;
        const int16_t x = -2;
        expect_ok("secs_ii_item_create_i2",
                  secs_ii_item_create_i2(&x, 1, &v));
        expect_ok("secs_ii_item_list_append_take(i2)",
                  secs_ii_item_list_append_take(root, &v));
    }
    {
        secs_ii_item_t *v = NULL;
        const int32_t x = -3;
        expect_ok("secs_ii_item_create_i4",
                  secs_ii_item_create_i4(&x, 1, &v));
        expect_ok("secs_ii_item_list_append_take(i4)",
                  secs_ii_item_list_append_take(root, &v));
    }
    {
        secs_ii_item_t *v = NULL;
        const int64_t x = -4;
        expect_ok("secs_ii_item_create_i8",
                  secs_ii_item_create_i8(&x, 1, &v));
        expect_ok("secs_ii_item_list_append_take(i8)",
                  secs_ii_item_list_append_take(root, &v));
    }
    {
        secs_ii_item_t *v = NULL;
        const uint8_t x = 5u;
        expect_ok("secs_ii_item_create_u1",
                  secs_ii_item_create_u1(&x, 1, &v));
        expect_ok("secs_ii_item_list_append_take(u1)",
                  secs_ii_item_list_append_take(root, &v));
    }
    {
        secs_ii_item_t *v = NULL;
        const uint32_t x = 7u;
        expect_ok("secs_ii_item_create_u4",
                  secs_ii_item_create_u4(&x, 1, &v));
        expect_ok("secs_ii_item_list_append_take(u4)",
                  secs_ii_item_list_append_take(root, &v));
    }
    {
        secs_ii_item_t *v = NULL;
        const uint64_t x = 8u;
        expect_ok("secs_ii_item_create_u8",
                  secs_ii_item_create_u8(&x, 1, &v));
        expect_ok("secs_ii_item_list_append_take(u8)",
                  secs_ii_item_list_append_take(root, &v));
    }
    {
        secs_ii_item_t *v = NULL;
        const float x = 1.25f;
        expect_ok("secs_ii_item_create_f4",
                  secs_ii_item_create_f4(&x, 1, &v));
        expect_ok("secs_ii_item_list_append_take(f4)",
                  secs_ii_item_list_append_take(root, &v));
    }
    {
        secs_ii_item_t *v = NULL;
        const double x = -2.5;
        expect_ok("secs_ii_item_create_f8",
                  secs_ii_item_create_f8(&x, 1, &v));
        expect_ok("secs_ii_item_list_append_take(f8)",
                  secs_ii_item_list_append_take(root, &v));
    }

    {
        size_t idx[1];
        size_t n = 0;

        idx[0] = 0;
        const int8_t *p_i1 = NULL;
        expect_ok("secs_ii_item_i1_view_at_list_path",
                  secs_ii_item_i1_view_at_list_path(root, &p_i1, &n, idx, 1));
        int8_t v_i1 = 0;
        expect_ok("secs_ii_item_get_i1_at_list_path",
                  secs_ii_item_get_i1_at_list_path(root, &v_i1, idx, 1));
        {
            const int8_t *p2 = NULL;
            size_t n2 = 0;
            expect_ok("secs_ii_item_i1_view_at_path",
                      secs_ii_item_i1_view_at_path(root, &p2, &n2, 1, (size_t)0));
            int8_t v2 = 0;
            expect_ok("secs_ii_item_get_i1_at_path",
                      secs_ii_item_get_i1_at_path(root, &v2, 1, (size_t)0));
        }

        idx[0] = 1;
        const int16_t *p_i2 = NULL;
        expect_ok("secs_ii_item_i2_view_at_list_path",
                  secs_ii_item_i2_view_at_list_path(root, &p_i2, &n, idx, 1));
        int16_t v_i2 = 0;
        expect_ok("secs_ii_item_get_i2_at_list_path",
                  secs_ii_item_get_i2_at_list_path(root, &v_i2, idx, 1));
        {
            const int16_t *p2 = NULL;
            size_t n2 = 0;
            expect_ok("secs_ii_item_i2_view_at_path",
                      secs_ii_item_i2_view_at_path(root, &p2, &n2, 1, (size_t)1));
            int16_t v2 = 0;
            expect_ok("secs_ii_item_get_i2_at_path",
                      secs_ii_item_get_i2_at_path(root, &v2, 1, (size_t)1));
        }

        idx[0] = 2;
        const int32_t *p_i4 = NULL;
        expect_ok("secs_ii_item_i4_view_at_list_path",
                  secs_ii_item_i4_view_at_list_path(root, &p_i4, &n, idx, 1));
        int32_t v_i4 = 0;
        expect_ok("secs_ii_item_get_i4_at_list_path",
                  secs_ii_item_get_i4_at_list_path(root, &v_i4, idx, 1));
        {
            const int32_t *p2 = NULL;
            size_t n2 = 0;
            expect_ok("secs_ii_item_i4_view_at_path",
                      secs_ii_item_i4_view_at_path(root, &p2, &n2, 1, (size_t)2));
            int32_t v2 = 0;
            expect_ok("secs_ii_item_get_i4_at_path",
                      secs_ii_item_get_i4_at_path(root, &v2, 1, (size_t)2));
        }

        idx[0] = 3;
        const int64_t *p_i8 = NULL;
        expect_ok("secs_ii_item_i8_view_at_list_path",
                  secs_ii_item_i8_view_at_list_path(root, &p_i8, &n, idx, 1));
        int64_t v_i8 = 0;
        expect_ok("secs_ii_item_get_i8_at_list_path",
                  secs_ii_item_get_i8_at_list_path(root, &v_i8, idx, 1));
        {
            const int64_t *p2 = NULL;
            size_t n2 = 0;
            expect_ok("secs_ii_item_i8_view_at_path",
                      secs_ii_item_i8_view_at_path(root, &p2, &n2, 1, (size_t)3));
            int64_t v2 = 0;
            expect_ok("secs_ii_item_get_i8_at_path",
                      secs_ii_item_get_i8_at_path(root, &v2, 1, (size_t)3));
        }

        idx[0] = 4;
        const uint8_t *p_u1 = NULL;
        expect_ok("secs_ii_item_u1_view_at_list_path",
                  secs_ii_item_u1_view_at_list_path(root, &p_u1, &n, idx, 1));
        uint8_t v_u1 = 0;
        expect_ok("secs_ii_item_get_u1_at_list_path",
                  secs_ii_item_get_u1_at_list_path(root, &v_u1, idx, 1));
        {
            const uint8_t *p2 = NULL;
            size_t n2 = 0;
            expect_ok("secs_ii_item_u1_view_at_path",
                      secs_ii_item_u1_view_at_path(root, &p2, &n2, 1, (size_t)4));
            uint8_t v2 = 0;
            expect_ok("secs_ii_item_get_u1_at_path",
                      secs_ii_item_get_u1_at_path(root, &v2, 1, (size_t)4));
        }

        idx[0] = 5;
        const uint32_t *p_u4 = NULL;
        expect_ok("secs_ii_item_u4_view_at_list_path",
                  secs_ii_item_u4_view_at_list_path(root, &p_u4, &n, idx, 1));
        uint32_t v_u4 = 0;
        expect_ok("secs_ii_item_get_u4_at_list_path",
                  secs_ii_item_get_u4_at_list_path(root, &v_u4, idx, 1));
        {
            const uint32_t *p2 = NULL;
            size_t n2 = 0;
            expect_ok("secs_ii_item_u4_view_at_path",
                      secs_ii_item_u4_view_at_path(root, &p2, &n2, 1, (size_t)5));
            uint32_t v2 = 0;
            expect_ok("secs_ii_item_get_u4_at_path",
                      secs_ii_item_get_u4_at_path(root, &v2, 1, (size_t)5));
        }

        idx[0] = 6;
        const uint64_t *p_u8 = NULL;
        expect_ok("secs_ii_item_u8_view_at_list_path",
                  secs_ii_item_u8_view_at_list_path(root, &p_u8, &n, idx, 1));
        uint64_t v_u8 = 0;
        expect_ok("secs_ii_item_get_u8_at_list_path",
                  secs_ii_item_get_u8_at_list_path(root, &v_u8, idx, 1));
        {
            const uint64_t *p2 = NULL;
            size_t n2 = 0;
            expect_ok("secs_ii_item_u8_view_at_path",
                      secs_ii_item_u8_view_at_path(root, &p2, &n2, 1, (size_t)6));
            uint64_t v2 = 0;
            expect_ok("secs_ii_item_get_u8_at_path",
                      secs_ii_item_get_u8_at_path(root, &v2, 1, (size_t)6));
        }

        idx[0] = 7;
        const float *p_f4 = NULL;
        expect_ok("secs_ii_item_f4_view_at_list_path",
                  secs_ii_item_f4_view_at_list_path(root, &p_f4, &n, idx, 1));
        float v_f4 = 0.0f;
        expect_ok("secs_ii_item_get_f4_at_list_path",
                  secs_ii_item_get_f4_at_list_path(root, &v_f4, idx, 1));
        {
            const float *p2 = NULL;
            size_t n2 = 0;
            expect_ok("secs_ii_item_f4_view_at_path",
                      secs_ii_item_f4_view_at_path(root, &p2, &n2, 1, (size_t)7));
            float v2 = 0.0f;
            expect_ok("secs_ii_item_get_f4_at_path",
                      secs_ii_item_get_f4_at_path(root, &v2, 1, (size_t)7));
        }

        idx[0] = 8;
        const double *p_f8 = NULL;
        expect_ok("secs_ii_item_f8_view_at_list_path",
                  secs_ii_item_f8_view_at_list_path(root, &p_f8, &n, idx, 1));
        double v_f8 = 0.0;
        expect_ok("secs_ii_item_get_f8_at_list_path",
                  secs_ii_item_get_f8_at_list_path(root, &v_f8, idx, 1));
        {
            const double *p2 = NULL;
            size_t n2 = 0;
            expect_ok("secs_ii_item_f8_view_at_path",
                      secs_ii_item_f8_view_at_path(root, &p2, &n2, 1, (size_t)8));
            double v2 = 0.0;
            expect_ok("secs_ii_item_get_f8_at_path",
                      secs_ii_item_get_f8_at_path(root, &v2, 1, (size_t)8));
        }
    }

    secs_ii_item_destroy(root);
}

static void test_ii_clone_and_list_path_array_helpers(void) {
    /* create_ascii_cstr + ascii_view */
    {
        secs_ii_item_t *a = NULL;
        expect_ok("secs_ii_item_create_ascii_cstr",
                  secs_ii_item_create_ascii_cstr("abc", &a));
        const char *p = NULL;
        size_t n = 0;
        expect_ok("secs_ii_item_ascii_view(cstr)",
                  secs_ii_item_ascii_view(a, &p, &n));
        if (!p || n != 3u || memcmp(p, "abc", 3) != 0) {
            fprintf(stderr, "FAIL: create_ascii_cstr/ascii_view mismatch\n");
            ++g_failures;
        }
        secs_ii_item_destroy(a);
    }

    /* clone：深拷贝后销毁原对象不影响 clone */
    {
        secs_ii_item_t *src = NULL;
        expect_ok("secs_ii_item_create_ascii(src)",
                  secs_ii_item_create_ascii("hello", 5, &src));

        secs_ii_item_t *cl = NULL;
        expect_ok("secs_ii_item_clone", secs_ii_item_clone(src, &cl));
        secs_ii_item_destroy(src);

        const char *p = NULL;
        size_t n = 0;
        expect_ok("secs_ii_item_ascii_view(clone)",
                  secs_ii_item_ascii_view(cl, &p, &n));
        if (!p || n != 5u || memcmp(p, "hello", 5) != 0) {
            fprintf(stderr, "FAIL: clone/ascii_view mismatch\n");
            ++g_failures;
        }
        secs_ii_item_destroy(cl);
    }

    /* at_list_path：array 版本（避免 C varargs UB） */
    {
        secs_ii_item_t *root = NULL;
        expect_ok("secs_ii_item_create_list(list_path)",
                  secs_ii_item_create_list(&root));
        expect_ok("secs_ii_item_list_append_ascii(list_path)",
                  secs_ii_item_list_append_ascii(root, "hello"));

        secs_ii_item_t *inner = NULL;
        expect_ok("secs_ii_item_create_list(inner)", secs_ii_item_create_list(&inner));
        expect_ok("secs_ii_item_list_append_u2(inner)",
                  secs_ii_item_list_append_u2(inner, 9u));
        expect_ok("secs_ii_item_list_append_take(inner->root)",
                  secs_ii_item_list_append_take(root, &inner));

        /* root[0] == "hello" */
        {
            const size_t path0[] = {0u};
            const char *p = NULL;
            size_t n = 0;
            expect_ok("secs_ii_item_ascii_view_at_list_path(root[0])",
                      secs_ii_item_ascii_view_at_list_path(
                          root, &p, &n, path0, sizeof(path0) / sizeof(path0[0])));
            if (!p || n != 5u || memcmp(p, "hello", 5) != 0) {
                fprintf(stderr, "FAIL: ascii_view_at_list_path mismatch\n");
                ++g_failures;
            }
        }

        /* root[1][0] == U2(9) */
        {
            const size_t path[] = {1u, 0u};
            uint16_t out = 0;
            expect_ok("secs_ii_item_get_u2_at_list_path(root[1][0])",
                      secs_ii_item_get_u2_at_list_path(
                          root, &out, path, sizeof(path) / sizeof(path[0])));
            if (out != 9u) {
                fprintf(stderr, "FAIL: get_u2_at_list_path mismatch\n");
                ++g_failures;
            }

            const uint16_t *p = NULL;
            size_t n = 0;
            expect_ok("secs_ii_item_u2_view_at_list_path(root[1][0])",
                      secs_ii_item_u2_view_at_list_path(
                          root, &p, &n, path, sizeof(path) / sizeof(path[0])));
            if (!p || n != 1u || p[0] != 9u) {
                fprintf(stderr, "FAIL: u2_view_at_list_path mismatch\n");
                ++g_failures;
            }
        }

        /* indices==NULL 且 indices_n!=0：必须报错 */
        {
            const char *p = NULL;
            size_t n = 0;
            expect_err("secs_ii_item_ascii_view_at_list_path(NULL indices)",
                       secs_ii_item_ascii_view_at_list_path(root, &p, &n, NULL, 1));
        }

        secs_ii_item_destroy(root);
    }

    /* indices_n==0：选择 root 本身 */
    {
        secs_ii_item_t *u2 = NULL;
        const uint16_t v = 7u;
        expect_ok("secs_ii_item_create_u2(root)", secs_ii_item_create_u2(&v, 1, &u2));

        uint16_t out = 0;
        expect_ok("secs_ii_item_get_u2_at_list_path(depth0)",
                  secs_ii_item_get_u2_at_list_path(u2, &out, NULL, 0));
        if (out != v) {
            fprintf(stderr, "FAIL: get_u2_at_list_path(depth0) mismatch\n");
            ++g_failures;
        }
        secs_ii_item_destroy(u2);
    }

    /* 覆盖其它 *_at_list_path：各数值类型 + binary/boolean */
    {
#define TEST_NUMERIC_AT_LIST_PATH(tag, c_type, create_fn, view_at_fn, get_at_fn, v0) \
    do {                                                                            \
        secs_ii_item_t *item = NULL;                                                \
        const c_type in = (c_type)(v0);                                             \
        expect_ok(#create_fn "(list_path)", create_fn(&in, 1, &item));              \
        const c_type *p = NULL;                                                     \
        size_t n = 0;                                                               \
        expect_ok(#view_at_fn "(depth0)", view_at_fn(item, &p, &n, NULL, 0));       \
        if (!p || n != 1u || p[0] != in) {                                          \
            fprintf(stderr, "FAIL: " #tag " view_at_list_path mismatch\n");         \
            ++g_failures;                                                           \
        }                                                                           \
        c_type out = (c_type)0;                                                     \
        expect_ok(#get_at_fn "(depth0)", get_at_fn(item, &out, NULL, 0));           \
        if (out != in) {                                                            \
            fprintf(stderr, "FAIL: " #tag " get_at_list_path mismatch\n");          \
            ++g_failures;                                                           \
        }                                                                           \
        secs_ii_item_destroy(item);                                                 \
    } while (0)

        TEST_NUMERIC_AT_LIST_PATH(i1,
                                  int8_t,
                                  secs_ii_item_create_i1,
                                  secs_ii_item_i1_view_at_list_path,
                                  secs_ii_item_get_i1_at_list_path,
                                  -3);
        TEST_NUMERIC_AT_LIST_PATH(i2,
                                  int16_t,
                                  secs_ii_item_create_i2,
                                  secs_ii_item_i2_view_at_list_path,
                                  secs_ii_item_get_i2_at_list_path,
                                  -300);
        TEST_NUMERIC_AT_LIST_PATH(i4,
                                  int32_t,
                                  secs_ii_item_create_i4,
                                  secs_ii_item_i4_view_at_list_path,
                                  secs_ii_item_get_i4_at_list_path,
                                  -123456);
        TEST_NUMERIC_AT_LIST_PATH(i8,
                                  int64_t,
                                  secs_ii_item_create_i8,
                                  secs_ii_item_i8_view_at_list_path,
                                  secs_ii_item_get_i8_at_list_path,
                                  -1234567890);
        TEST_NUMERIC_AT_LIST_PATH(u1,
                                  uint8_t,
                                  secs_ii_item_create_u1,
                                  secs_ii_item_u1_view_at_list_path,
                                  secs_ii_item_get_u1_at_list_path,
                                  7);
        TEST_NUMERIC_AT_LIST_PATH(u2,
                                  uint16_t,
                                  secs_ii_item_create_u2,
                                  secs_ii_item_u2_view_at_list_path,
                                  secs_ii_item_get_u2_at_list_path,
                                  9);
        TEST_NUMERIC_AT_LIST_PATH(u4,
                                  uint32_t,
                                  secs_ii_item_create_u4,
                                  secs_ii_item_u4_view_at_list_path,
                                  secs_ii_item_get_u4_at_list_path,
                                  12345);
        TEST_NUMERIC_AT_LIST_PATH(u8,
                                  uint64_t,
                                  secs_ii_item_create_u8,
                                  secs_ii_item_u8_view_at_list_path,
                                  secs_ii_item_get_u8_at_list_path,
                                  123456);
        TEST_NUMERIC_AT_LIST_PATH(f4,
                                  float,
                                  secs_ii_item_create_f4,
                                  secs_ii_item_f4_view_at_list_path,
                                  secs_ii_item_get_f4_at_list_path,
                                  0.5f);
        TEST_NUMERIC_AT_LIST_PATH(f8,
                                  double,
                                  secs_ii_item_create_f8,
                                  secs_ii_item_f8_view_at_list_path,
                                  secs_ii_item_get_f8_at_list_path,
                                  -2.5);

#undef TEST_NUMERIC_AT_LIST_PATH

        /* binary_view_at_list_path(depth0) */
        {
            const uint8_t in[3] = {1u, 2u, 3u};
            secs_ii_item_t *b = NULL;
            expect_ok("secs_ii_item_create_binary(list_path)",
                      secs_ii_item_create_binary(in, sizeof(in), &b));
            const uint8_t *p = NULL;
            size_t n = 0;
            expect_ok("secs_ii_item_binary_view_at_list_path(depth0)",
                      secs_ii_item_binary_view_at_list_path(b, &p, &n, NULL, 0));
            if (!p || n != sizeof(in) || memcmp(p, in, sizeof(in)) != 0) {
                fprintf(stderr, "FAIL: binary_view_at_list_path mismatch\n");
                ++g_failures;
            }
            secs_ii_item_destroy(b);
        }

        /* boolean_copy_at_list_path(depth0) */
        {
            const uint8_t in01[3] = {1u, 0u, 1u};
            secs_ii_item_t *b = NULL;
            expect_ok("secs_ii_item_create_boolean(list_path)",
                      secs_ii_item_create_boolean(in01, sizeof(in01), &b));

            uint8_t *out = NULL;
            size_t n = 0;
            expect_ok("secs_ii_item_boolean_copy_at_list_path(depth0)",
                      secs_ii_item_boolean_copy_at_list_path(b, &out, &n, NULL, 0));
            if (!out || n != sizeof(in01) || memcmp(out, in01, sizeof(in01)) != 0) {
                fprintf(stderr, "FAIL: boolean_copy_at_list_path mismatch\n");
                ++g_failures;
            }
            if (out) {
                secs_free(out);
            }
            secs_ii_item_destroy(b);
        }

        /* get_boolean_at_list_path(depth0) */
        {
            const uint8_t v01 = 1u;
            secs_ii_item_t *b = NULL;
            expect_ok("secs_ii_item_create_boolean(scalar)",
                      secs_ii_item_create_boolean(&v01, 1, &b));
            uint8_t out = 0;
            expect_ok("secs_ii_item_get_boolean_at_list_path(depth0)",
                      secs_ii_item_get_boolean_at_list_path(b, &out, NULL, 0));
            if (out != 1u) {
                fprintf(stderr, "FAIL: get_boolean_at_list_path mismatch\n");
                ++g_failures;
            }
            secs_ii_item_destroy(b);
        }
    }
}

static void test_sml_runtime_basic(void) {
    secs_sml_runtime_t *rt = NULL;
    expect_ok("secs_sml_runtime_create", secs_sml_runtime_create(&rt));

    const char *sml = "s1f1: S1F1 W <L>.\n"
                      "s1f2: S1F2 <L <A \"Hello\">>.\n"
                      "if (s1f1) s1f2.\n";
    expect_ok("secs_sml_runtime_load_cstr",
              secs_sml_runtime_load_cstr(rt, sml));

    /* 查模板：应返回 SECS-II body bytes（不贴源码，只验证结构） */
    {
        uint8_t *body = NULL;
        size_t body_n = 0;
        uint8_t stream = 0;
        uint8_t function = 0;
        int w_bit = 0;
        expect_ok("secs_sml_runtime_get_message_body_by_name",
                  secs_sml_runtime_get_message_body_by_name(
                      rt, "s1f2", &body, &body_n, &stream, &function, &w_bit));
        if (stream != 1u || function != 2u || w_bit != 0) {
            fprintf(stderr, "FAIL: s1f2 meta mismatch\n");
            ++g_failures;
        }

        size_t consumed = 0;
        secs_ii_item_t *decoded = NULL;
        expect_ok("secs_ii_decode_one(s1f2 body)",
                  secs_ii_decode_one(body, body_n, &consumed, &decoded));
        secs_ii_item_destroy(decoded);
        secs_free(body);
    }

    /* 兼容：允许直接用 "SxFy" 字符串查模板（覆盖 runtime.get_message 的 parse_sf 分支） */
    {
        uint8_t *body = NULL;
        size_t body_n = 0;
        uint8_t stream = 0;
        uint8_t function = 0;
        int w_bit = 0;
        expect_ok("secs_sml_runtime_get_message_body_by_name(S1F2)",
                  secs_sml_runtime_get_message_body_by_name(
                      rt, "S1F2", &body, &body_n, &stream, &function, &w_bit));
        if (stream != 1u || function != 2u || w_bit != 0) {
            fprintf(stderr, "FAIL: S1F2 meta mismatch\n");
            ++g_failures;
        }
        secs_free(body);
    }

    /* 条件匹配：s1f1 -> s1f2 */
    {
        secs_ii_item_t *req = NULL;
        expect_ok("secs_ii_item_create_list(req)",
                  secs_ii_item_create_list(&req));

        uint8_t *req_body = NULL;
        size_t req_body_n = 0;
        expect_ok("secs_ii_encode(req)",
                  secs_ii_encode(req, &req_body, &req_body_n));
        secs_ii_item_destroy(req);

        char *out_name = NULL;
        expect_ok("secs_sml_runtime_match_response",
                  secs_sml_runtime_match_response(
                      rt, 1, 1, req_body, req_body_n, &out_name));
        if (!out_name || strcmp(out_name, "s1f2") != 0) {
            fprintf(stderr, "FAIL: match_response expected s1f2\n");
            ++g_failures;
        }
        if (out_name) {
            secs_free(out_name);
        }

        /* 不匹配：应返回 OK 且 out_name==NULL */
        {
            char *no_match = NULL;
            expect_ok("secs_sml_runtime_match_response(no match)",
                      secs_sml_runtime_match_response(
                          rt, 9, 9, req_body, req_body_n, &no_match));
            if (no_match) {
                fprintf(stderr,
                        "FAIL: match_response(no match) expected NULL\n");
                ++g_failures;
                secs_free(no_match);
            }
        }

        /* 恶意输入：传入非法 SECS-II body，不应崩溃，应返回错误 */
        {
            const uint8_t bad[1] = {0xFFu};
            char *bad_out = NULL;
            expect_err("secs_sml_runtime_match_response(bad body)",
                       secs_sml_runtime_match_response(
                           rt, 1, 1, bad, sizeof(bad), &bad_out));
            if (bad_out) {
                secs_free(bad_out);
            }
        }

        secs_free(req_body);
    }

    /* 不存在的 name：应返回 NOT_FOUND */
    {
        uint8_t *body = NULL;
        size_t body_n = 0;
        secs_error_t err = secs_sml_runtime_get_message_body_by_name(
            rt, "no_such_message", &body, &body_n, NULL, NULL, NULL);
        if (err.value != (int)SECS_C_API_NOT_FOUND) {
            failf("secs_sml_runtime_get_message_body_by_name(no_such_message)",
                  err);
        }
    }

    /* 恶意输入：语法错误不应崩溃 */
    {
        const char *bad = "S1F1 W <L\n";
        secs_sml_runtime_t *bad_rt = NULL;
        expect_ok("secs_sml_runtime_create(bad)",
                  secs_sml_runtime_create(&bad_rt));
        secs_error_t err = secs_sml_runtime_load(bad_rt, bad, strlen(bad));
        expect_err("secs_sml_runtime_load(bad)", err);
        secs_sml_runtime_destroy(bad_rt);
    }

    secs_sml_runtime_destroy(rt);
}

static void test_sml_runtime_placeholders(void) {
    secs_sml_runtime_t *rt = NULL;
    expect_ok("secs_sml_runtime_create(placeholder)",
              secs_sml_runtime_create(&rt));

    /*
     * 占位符消息：
     * - req 的 body 依赖变量 MDLN（当前 C API 不提供变量注入接口，因此“取模板”
     *   应报错）；
     * - 但 if(req)->rsp 的条件匹配只依赖 S/F，不应受影响。
     */
    const char *sml = "req: S1F1 W <A MDLN>.\n"
                      "rsp: S1F2 <L <A \"OK\">>.\n"
                      "if (req) rsp.\n";
    expect_ok("secs_sml_runtime_load(placeholder)",
              secs_sml_runtime_load(rt, sml, strlen(sml)));

    /* rsp：不含占位符，应能正常取到 SECS-II body bytes */
    {
        uint8_t *body = NULL;
        size_t body_n = 0;
        expect_ok("secs_sml_runtime_get_message_body_by_name(rsp)",
                  secs_sml_runtime_get_message_body_by_name(
                      rt, "rsp", &body, &body_n, NULL, NULL, NULL));
        if (body == NULL || body_n == 0) {
            fprintf(stderr, "FAIL: rsp body should not be empty\n");
            ++g_failures;
        }
        secs_free(body);
    }

    /* req：含占位符，当前应返回 sml.render/missing_variable（value=1） */
    {
        uint8_t *body = NULL;
        size_t body_n = 0;
        secs_error_t err = secs_sml_runtime_get_message_body_by_name(
            rt, "req", &body, &body_n, NULL, NULL, NULL);
        expect_err("secs_sml_runtime_get_message_body_by_name(req placeholder)",
                   err);
        if (err.category == NULL || strcmp(err.category, "sml.render") != 0 ||
            err.value != 1) {
            failf("req placeholder error should be sml.render/missing_variable",
                  err);
        }
        if (body != NULL || body_n != 0) {
            fprintf(stderr,
                    "FAIL: req placeholder expected (body=NULL, body_n=0)\n");
            ++g_failures;
            if (body) {
                secs_free(body);
            }
        }
    }

    /* 条件匹配不应受占位符影响 */
    {
        secs_ii_item_t *req_item = NULL;
        expect_ok("secs_ii_item_create_list(req_item)",
                  secs_ii_item_create_list(&req_item));

        uint8_t *req_body = NULL;
        size_t req_body_n = 0;
        expect_ok("secs_ii_encode(req_item)",
                  secs_ii_encode(req_item, &req_body, &req_body_n));
        secs_ii_item_destroy(req_item);

        char *out_name = NULL;
        expect_ok("secs_sml_runtime_match_response(placeholder req)",
                  secs_sml_runtime_match_response(
                      rt, 1, 1, req_body, req_body_n, &out_name));
        if (!out_name || strcmp(out_name, "rsp") != 0) {
            fprintf(stderr, "FAIL: match_response expected rsp\n");
            ++g_failures;
        }
        if (out_name) {
            secs_free(out_name);
        }
        secs_free(req_body);
    }

    secs_sml_runtime_destroy(rt);

    /*
     * 负例：索引语义约束：(n) 与 [i] 互斥，组合写法应被解析器拒绝
     * （sml.parser/invalid_condition=7）。
     */
    {
        secs_sml_runtime_t *bad_rt = NULL;
        expect_ok("secs_sml_runtime_create(bad index combination)",
                  secs_sml_runtime_create(&bad_rt));
        const char *bad = "a: S1F1 <L>.\n"
                          "rsp: S1F2 <L>.\n"
                          "if (a(1)[1]==<A \"x\">) rsp.\n";
        secs_error_t err = secs_sml_runtime_load(bad_rt, bad, strlen(bad));
        expect_err("secs_sml_runtime_load(bad index combination)", err);
        if (err.category == NULL || strcmp(err.category, "sml.parser") != 0 ||
            err.value != 7) {
            failf("bad index combination should be sml.parser/invalid_condition",
                  err);
        }
        secs_sml_runtime_destroy(bad_rt);
    }
}

static void test_sml_runtime_match_response_with_capture(void) {
    secs_sml_runtime_t *rt = NULL;
    expect_ok("secs_sml_runtime_create(capture)", secs_sml_runtime_create(&rt));

    const char *sml = "r0: S2F22 <L <U2 CAP_A>>.\n"
                      "if (S2F21 <L [2] <U2 $CAP_A> <L $CAP_B>>) r0.\n";
    expect_ok("secs_sml_runtime_load(capture)",
              secs_sml_runtime_load(rt, sml, strlen(sml)));

    /* incoming body：<L <U2 0x1001> <L <A \"x\">>> */
    secs_ii_item_t *body_item = NULL;
    expect_ok("secs_ii_item_create_list(capture body)",
              secs_ii_item_create_list(&body_item));
    {
        uint16_t v = 0x1001;
        secs_ii_item_t *u2 = NULL;
        expect_ok("secs_ii_item_create_u2(capture A)",
                  secs_ii_item_create_u2(&v, 1, &u2));
        expect_ok("secs_ii_item_list_append(capture A)",
                  secs_ii_item_list_append(body_item, u2));
        secs_ii_item_destroy(u2);
    }
    {
        secs_ii_item_t *inner = NULL;
        expect_ok("secs_ii_item_create_list(capture B)",
                  secs_ii_item_create_list(&inner));
        secs_ii_item_t *ascii = NULL;
        expect_ok("secs_ii_item_create_ascii(capture x)",
                  secs_ii_item_create_ascii("x", 1, &ascii));
        expect_ok("secs_ii_item_list_append(capture x)",
                  secs_ii_item_list_append(inner, ascii));
        secs_ii_item_destroy(ascii);

        expect_ok("secs_ii_item_list_append(capture B)",
                  secs_ii_item_list_append(body_item, inner));
        secs_ii_item_destroy(inner);
    }

    uint8_t *body = NULL;
    size_t body_n = 0;
    expect_ok("secs_ii_encode(capture body)",
              secs_ii_encode(body_item, &body, &body_n));
    secs_ii_item_destroy(body_item);

    char *out_name = NULL;
    secs_sml_render_context_t *captures = NULL;
    expect_ok("secs_sml_runtime_match_response_with_capture",
              secs_sml_runtime_match_response_with_capture(
                  rt, 2, 21, body, body_n, NULL, &out_name, &captures));
    if (!out_name || strcmp(out_name, "r0") != 0) {
        fprintf(stderr, "FAIL: match_response_with_capture expected r0\n");
        ++g_failures;
    }
    if (!captures) {
        fprintf(stderr, "FAIL: match_response_with_capture expected captures ctx\n");
        ++g_failures;
    }

    if (captures) {
        /* CAP_A: <U2 0x1001> */
        secs_ii_item_t *a_item = NULL;
        expect_ok("secs_sml_render_context_get(CAP_A)",
                  secs_sml_render_context_get(captures, "CAP_A", &a_item));
        {
            const uint16_t *p = NULL;
            size_t n = 0;
            expect_ok("secs_ii_item_u2_view(CAP_A)",
                      secs_ii_item_u2_view(a_item, &p, &n));
            if (!p || n != 1 || p[0] != 0x1001) {
                fprintf(stderr, "FAIL: capture A mismatch\n");
                ++g_failures;
            }
        }
        secs_ii_item_destroy(a_item);

        /* CAP_B: <L <A \"x\">> */
        secs_ii_item_t *b_item = NULL;
        expect_ok("secs_sml_render_context_get(CAP_B)",
                  secs_sml_render_context_get(captures, "CAP_B", &b_item));
        {
            size_t n = 0;
            expect_ok("secs_ii_item_list_size(CAP_B)",
                      secs_ii_item_list_size(b_item, &n));
            if (n != 1) {
                fprintf(stderr, "FAIL: capture B list size mismatch\n");
                ++g_failures;
            }
            secs_ii_item_t *child = NULL;
            expect_ok("secs_ii_item_list_get(CAP_B[0])",
                      secs_ii_item_list_get(b_item, 0, &child));
            const char *s = NULL;
            size_t s_n = 0;
            expect_ok("secs_ii_item_ascii_view(CAP_B[0])",
                      secs_ii_item_ascii_view(child, &s, &s_n));
            if (!s || s_n != 1 || s[0] != 'x') {
                fprintf(stderr, "FAIL: capture B[0] ascii mismatch\n");
                ++g_failures;
            }
            secs_ii_item_destroy(child);
        }
        secs_ii_item_destroy(b_item);

        /* 不存在：应返回 NOT_FOUND */
        {
            secs_ii_item_t *missing = NULL;
            secs_error_t err =
                secs_sml_render_context_get(captures, "NO_SUCH", &missing);
            expect_err("secs_sml_render_context_get(NO_SUCH)", err);
            if (missing) {
                fprintf(stderr,
                        "FAIL: render_context_get(NO_SUCH) expected NULL\n");
                ++g_failures;
                secs_ii_item_destroy(missing);
            }
            if (err.value != (int)SECS_C_API_NOT_FOUND ||
                err.category == NULL || strcmp(err.category, "secs.c_api") != 0) {
                failf("render_context_get(NO_SUCH) should be secs.c_api/NOT_FOUND",
                      err);
            }
        }

        secs_sml_render_context_destroy(captures);
    }

    if (out_name) {
        secs_free(out_name);
    }
    secs_free(body);
    secs_sml_runtime_destroy(rt);
}

static void test_sml_render_context_lifecycle(void) {
    secs_sml_render_context_t *ctx = NULL;
    expect_ok("secs_sml_render_context_create", secs_sml_render_context_create(&ctx));

    secs_ii_item_t *mdln = NULL;
    expect_ok("secs_ii_item_create_ascii(MDLN)",
              secs_ii_item_create_ascii("MODEL-X", 7, &mdln));
    expect_ok("secs_sml_render_context_set(MDLN)",
              secs_sml_render_context_set(ctx, "MDLN", mdln));
    secs_ii_item_destroy(mdln);

    /* clear 后可复用 */
    secs_sml_render_context_clear(ctx);

    uint16_t ceid = 0x1234;
    secs_ii_item_t *ceid_item = NULL;
    expect_ok("secs_ii_item_create_u2(CEID)",
              secs_ii_item_create_u2(&ceid, 1, &ceid_item));
    expect_ok("secs_sml_render_context_set(CEID)",
              secs_sml_render_context_set(ctx, "CEID", ceid_item));
    secs_ii_item_destroy(ceid_item);

    secs_sml_render_context_destroy(ctx);
}

static void test_sml_runtime_encode_message_body_with_context(void) {
    secs_sml_runtime_t *rt = NULL;
    expect_ok("secs_sml_runtime_create(encode ctx)", secs_sml_runtime_create(&rt));

    const char *sml = "req: S1F1 W <A MDLN>.\n"
                      "rsp: S1F2 <L <A \"OK\">>.\n"
                      "if (req) rsp.\n";
    expect_ok("secs_sml_runtime_load(encode ctx)",
              secs_sml_runtime_load(rt, sml, strlen(sml)));

    secs_sml_render_context_t *ctx = NULL;
    expect_ok("secs_sml_render_context_create(encode ctx)",
              secs_sml_render_context_create(&ctx));

    /* 负例：缺失变量，应该返回 sml.render/missing_variable（value=1） */
    {
        uint8_t *body = NULL;
        size_t body_n = 0;
        secs_error_t err = secs_sml_runtime_encode_message_body(
            rt, "req", ctx, &body, &body_n, NULL, NULL, NULL);
        expect_err("secs_sml_runtime_encode_message_body(missing var)", err);
        if (err.category == NULL || strcmp(err.category, "sml.render") != 0 ||
            err.value != 1) {
            failf("encode(missing var) should be sml.render/missing_variable", err);
        }
        if (body != NULL || body_n != 0) {
            fprintf(stderr,
                    "FAIL: encode(missing var) expected (body=NULL, body_n=0)\n");
            ++g_failures;
            if (body) {
                secs_free(body);
            }
        }
    }

    /* 正例：注入 MDLN 后可正常渲染编码 */
    {
        secs_ii_item_t *mdln = NULL;
        expect_ok("secs_ii_item_create_ascii(MDLN)",
                  secs_ii_item_create_ascii("HELLO", 5, &mdln));
        expect_ok("secs_sml_render_context_set(MDLN)",
                  secs_sml_render_context_set(ctx, "MDLN", mdln));
        secs_ii_item_destroy(mdln);

        uint8_t *body = NULL;
        size_t body_n = 0;
        uint8_t stream = 0;
        uint8_t function = 0;
        int w_bit = 0;
        expect_ok("secs_sml_runtime_encode_message_body(req)",
                  secs_sml_runtime_encode_message_body(
                      rt, "req", ctx, &body, &body_n, &stream, &function, &w_bit));
        if (stream != 1u || function != 1u || w_bit != 1) {
            fprintf(stderr, "FAIL: encode(req) meta mismatch\n");
            ++g_failures;
        }

        size_t consumed = 0;
        secs_ii_item_t *decoded = NULL;
        expect_ok("secs_ii_decode_one(req body)",
                  secs_ii_decode_one(body, body_n, &consumed, &decoded));

        secs_ii_item_type_t ty;
        expect_ok("secs_ii_item_get_type(req body)",
                  secs_ii_item_get_type(decoded, &ty));
        if (ty != SECS_II_ITEM_ASCII) {
            fprintf(stderr, "FAIL: encode(req) expected ASCII item\n");
            ++g_failures;
        } else {
            const char *p = NULL;
            size_t n = 0;
            expect_ok("secs_ii_item_ascii_view(req body)",
                      secs_ii_item_ascii_view(decoded, &p, &n));
            if (!p || n != 5 || memcmp(p, "HELLO", 5) != 0) {
                fprintf(stderr, "FAIL: encode(req) ASCII mismatch\n");
                ++g_failures;
            }
        }

        secs_ii_item_destroy(decoded);
        secs_free(body);
    }

    secs_sml_render_context_destroy(ctx);
    secs_sml_runtime_destroy(rt);
}

static void test_sml_runtime_match_response_with_context(void) {
    secs_sml_runtime_t *rt = NULL;
    expect_ok("secs_sml_runtime_create(match ctx)", secs_sml_runtime_create(&rt));

    const char *sml = "req: S1F1 <L <U2 1> <U2 2>>.\n"
                      "rsp: S1F2 <L>.\n"
                      "if (req[1]==<U2 EXPECTED>) rsp.\n";
    expect_ok("secs_sml_runtime_load(match ctx)",
              secs_sml_runtime_load(rt, sml, strlen(sml)));

    /* 构造 incoming body：<L <U2 1> <U2 2>> */
    secs_ii_item_t *body_item = NULL;
    expect_ok("secs_ii_item_create_list(match ctx)",
              secs_ii_item_create_list(&body_item));
    {
        uint16_t v0 = 1;
        uint16_t v1 = 2;
        secs_ii_item_t *u2_0 = NULL;
        secs_ii_item_t *u2_1 = NULL;
        expect_ok("secs_ii_item_create_u2(v0)", secs_ii_item_create_u2(&v0, 1, &u2_0));
        expect_ok("secs_ii_item_create_u2(v1)", secs_ii_item_create_u2(&v1, 1, &u2_1));
        expect_ok("secs_ii_item_list_append(v0)",
                  secs_ii_item_list_append(body_item, u2_0));
        expect_ok("secs_ii_item_list_append(v1)",
                  secs_ii_item_list_append(body_item, u2_1));
        secs_ii_item_destroy(u2_0);
        secs_ii_item_destroy(u2_1);
    }

    uint8_t *body = NULL;
    size_t body_n = 0;
    expect_ok("secs_ii_encode(match ctx)", secs_ii_encode(body_item, &body, &body_n));
    secs_ii_item_destroy(body_item);

    /* ctx 为空：应不命中 */
    {
        char *out_name = NULL;
        expect_ok("secs_sml_runtime_match_response_with_context(no ctx)",
                  secs_sml_runtime_match_response_with_context(
                      rt, 1, 1, body, body_n, NULL, &out_name));
        if (out_name) {
            fprintf(stderr,
                    "FAIL: match_response_with_context(no ctx) expected NULL\n");
            ++g_failures;
            secs_free(out_name);
        }
    }

    /* 注入 EXPECTED=2：应命中 rsp */
    {
        secs_sml_render_context_t *ctx = NULL;
        expect_ok("secs_sml_render_context_create(match ctx)",
                  secs_sml_render_context_create(&ctx));
        uint16_t expected = 2;
        secs_ii_item_t *expected_item = NULL;
        expect_ok("secs_ii_item_create_u2(EXPECTED)",
                  secs_ii_item_create_u2(&expected, 1, &expected_item));
        expect_ok("secs_sml_render_context_set(EXPECTED)",
                  secs_sml_render_context_set(ctx, "EXPECTED", expected_item));
        secs_ii_item_destroy(expected_item);

        char *out_name = NULL;
        expect_ok("secs_sml_runtime_match_response_with_context(match)",
                  secs_sml_runtime_match_response_with_context(
                      rt, 1, 1, body, body_n, ctx, &out_name));
        if (!out_name || strcmp(out_name, "rsp") != 0) {
            fprintf(stderr, "FAIL: match_response_with_context expected rsp\n");
            ++g_failures;
        }
        if (out_name) {
            secs_free(out_name);
        }
        secs_sml_render_context_destroy(ctx);
    }

    secs_free(body);
    secs_sml_runtime_destroy(rt);
}

static void test_sml_runtime_match_response_empty_body(void) {
    secs_sml_runtime_t *rt = NULL;
    expect_ok("secs_sml_runtime_create(match empty body)",
              secs_sml_runtime_create(&rt));

    const char *sml = "rsp: S1F2 <L>.\n"
                      "if (S1F1) rsp.\n";
    expect_ok("secs_sml_runtime_load(match empty body)",
              secs_sml_runtime_load(rt, sml, strlen(sml)));

    /* body_bytes=NULL 且 body_n=0：兼容部分设备/模拟器发送“空 bytes” */
    {
        char *out_name = NULL;
        expect_ok("secs_sml_runtime_match_response_with_context(empty body)",
                  secs_sml_runtime_match_response_with_context(
                      rt, 1, 1, NULL, 0, NULL, &out_name));
        if (!out_name || strcmp(out_name, "rsp") != 0) {
            fprintf(stderr,
                    "FAIL: match_response_with_context(empty body) expected rsp\n");
            ++g_failures;
        }
        secs_free(out_name);
    }

    {
        char *out_name = NULL;
        secs_sml_render_context_t *captures = NULL;
        expect_ok("secs_sml_runtime_match_response_with_capture(empty body)",
                  secs_sml_runtime_match_response_with_capture(
                      rt, 1, 1, NULL, 0, NULL, &out_name, &captures));
        if (!out_name || strcmp(out_name, "rsp") != 0) {
            fprintf(stderr,
                    "FAIL: match_response_with_capture(empty body) expected rsp\n");
            ++g_failures;
        }
        if (!captures) {
            fprintf(stderr,
                    "FAIL: match_response_with_capture(empty body) expected captures ctx\n");
            ++g_failures;
        }
        secs_sml_render_context_destroy(captures);
        secs_free(out_name);
    }

    {
        char *out_name = NULL;
        secs_sml_match_trace_t *traces = NULL;
        size_t trace_n = 0;
        expect_ok("secs_sml_runtime_match_response_with_trace(empty body)",
                  secs_sml_runtime_match_response_with_trace(
                      rt, 1, 1, NULL, 0, NULL, &out_name, &traces, &trace_n));
        if (!out_name || strcmp(out_name, "rsp") != 0) {
            fprintf(stderr,
                    "FAIL: match_response_with_trace(empty body) expected rsp\n");
            ++g_failures;
        }
        if (traces || trace_n != 0) {
            fprintf(stderr,
                    "FAIL: match_response_with_trace(empty body) expected empty traces\n");
            ++g_failures;
            secs_sml_match_traces_free(traces, trace_n);
        }
        secs_free(out_name);
    }

    secs_sml_runtime_destroy(rt);
}

static void test_sml_runtime_match_response_with_trace(void) {
    enum {
        REASON_STREAM_FUNCTION_MISMATCH = 0,
        REASON_INDEX_OUT_OF_BOUNDS = 1,
        REASON_LIST_INDEX_OUT_OF_BOUNDS = 2,
        REASON_RENDER_MISSING_VARIABLE = 3,
        REASON_RENDER_TYPE_MISMATCH = 4,
        REASON_EXPECTED_VALUE_MISMATCH = 5,
        REASON_NOT_A_LIST = 6,
    };

    secs_sml_runtime_t *rt = NULL;
    expect_ok("secs_sml_runtime_create(trace)", secs_sml_runtime_create(&rt));

    const char *sml = "req: S1F1 <L <U2 1> <U2 2>>.\n"
                      "rsp1: S1F2 <L>.\n"
                      "rsp2: S1F3 <L>.\n"
                      "if (req[1]==<U2 999>) rsp1.\n"
                      "if (req[1]==<U2 EXPECTED>) rsp2.\n";
    expect_ok("secs_sml_runtime_load(trace)", secs_sml_runtime_load(rt, sml, strlen(sml)));

    /* incoming body：<L <U2 1> <U2 2>> */
    secs_ii_item_t *body_item = NULL;
    expect_ok("secs_ii_item_create_list(trace)", secs_ii_item_create_list(&body_item));
    {
        uint16_t v0 = 1;
        uint16_t v1 = 2;
        secs_ii_item_t *u2_0 = NULL;
        secs_ii_item_t *u2_1 = NULL;
        expect_ok("secs_ii_item_create_u2(trace v0)", secs_ii_item_create_u2(&v0, 1, &u2_0));
        expect_ok("secs_ii_item_create_u2(trace v1)", secs_ii_item_create_u2(&v1, 1, &u2_1));
        expect_ok("secs_ii_item_list_append(trace v0)",
                  secs_ii_item_list_append(body_item, u2_0));
        expect_ok("secs_ii_item_list_append(trace v1)",
                  secs_ii_item_list_append(body_item, u2_1));
        secs_ii_item_destroy(u2_0);
        secs_ii_item_destroy(u2_1);
    }

    uint8_t *body = NULL;
    size_t body_n = 0;
    expect_ok("secs_ii_encode(trace)", secs_ii_encode(body_item, &body, &body_n));
    secs_ii_item_destroy(body_item);

    /* 未命中：应返回 traces（规则 0: mismatch；规则 1: missing variable） */
    {
        char *out_name = NULL;
        secs_sml_match_trace_t *traces = NULL;
        size_t trace_n = 0;
        expect_ok("secs_sml_runtime_match_response_with_trace(no match)",
                  secs_sml_runtime_match_response_with_trace(
                      rt, 1, 1, body, body_n, NULL, &out_name, &traces, &trace_n));
        if (out_name) {
            fprintf(stderr, "FAIL: match_response_with_trace expected no match\n");
            ++g_failures;
            secs_free(out_name);
        }
        if (!traces || trace_n != 2) {
            fprintf(stderr,
                    "FAIL: match_response_with_trace expected 2 traces\n");
            ++g_failures;
        } else {
            if (traces[0].rule_index != 0 ||
                traces[0].condition_message_name == NULL ||
                strcmp(traces[0].condition_message_name, "req") != 0 ||
                traces[0].has_list_index != 1 || traces[0].list_index != 1 ||
                traces[0].reason != REASON_EXPECTED_VALUE_MISMATCH ||
                traces[0].detail == NULL ||
                strstr(traces[0].detail, "mismatch") == NULL) {
                fprintf(stderr, "FAIL: trace[0] unexpected content\n");
                ++g_failures;
            }
            if (traces[1].rule_index != 1 ||
                traces[1].reason != REASON_RENDER_MISSING_VARIABLE ||
                traces[1].detail == NULL ||
                strstr(traces[1].detail, "missing") == NULL) {
                fprintf(stderr, "FAIL: trace[1] unexpected content\n");
                ++g_failures;
            }
        }
        secs_sml_match_traces_free(traces, trace_n);
    }

    /* 命中：应返回 name 且 traces 为空 */
    {
        secs_sml_render_context_t *ctx = NULL;
        expect_ok("secs_sml_render_context_create(trace match)",
                  secs_sml_render_context_create(&ctx));
        uint16_t expected = 2;
        secs_ii_item_t *expected_item = NULL;
        expect_ok("secs_ii_item_create_u2(EXPECTED trace)",
                  secs_ii_item_create_u2(&expected, 1, &expected_item));
        expect_ok("secs_sml_render_context_set(EXPECTED trace)",
                  secs_sml_render_context_set(ctx, "EXPECTED", expected_item));
        secs_ii_item_destroy(expected_item);

        char *out_name = NULL;
        secs_sml_match_trace_t *traces = NULL;
        size_t trace_n = 0;
        expect_ok("secs_sml_runtime_match_response_with_trace(match)",
                  secs_sml_runtime_match_response_with_trace(
                      rt, 1, 1, body, body_n, ctx, &out_name, &traces, &trace_n));
        if (!out_name || strcmp(out_name, "rsp2") != 0) {
            fprintf(stderr, "FAIL: match_response_with_trace expected rsp2\n");
            ++g_failures;
        }
        if (traces || trace_n != 0) {
            fprintf(stderr, "FAIL: match_response_with_trace(match) expected empty traces\n");
            ++g_failures;
            secs_sml_match_traces_free(traces, trace_n);
        }
        if (out_name) {
            secs_free(out_name);
        }
        secs_sml_render_context_destroy(ctx);
    }

    /* 轻量循环：反复分配/释放 traces，辅助检查泄漏/悬空 */
    for (int i = 0; i < 50; ++i) {
        char *out_name = NULL;
        secs_sml_match_trace_t *traces = NULL;
        size_t trace_n = 0;
        expect_ok("secs_sml_runtime_match_response_with_trace(loop)",
                  secs_sml_runtime_match_response_with_trace(
                      rt, 1, 1, body, body_n, NULL, &out_name, &traces, &trace_n));
        if (out_name) {
            secs_free(out_name);
        }
        secs_sml_match_traces_free(traces, trace_n);
    }

    secs_free(body);
    secs_sml_runtime_destroy(rt);
}

static void test_sml_runtime_match_response_with_trace_empty_rules(void) {
    secs_sml_runtime_t *rt = NULL;
    expect_ok("secs_sml_runtime_create(trace empty)",
              secs_sml_runtime_create(&rt));

    /* 不含任何 if 规则：traces 应为空且不返回响应名 */
    const char *sml = "req: S1F1 <L>.\n"
                      "rsp: S1F2 <L>.\n";
    expect_ok("secs_sml_runtime_load(trace empty)",
              secs_sml_runtime_load(rt, sml, strlen(sml)));

    secs_ii_item_t *body_item = NULL;
    expect_ok("secs_ii_item_create_list(trace empty)",
              secs_ii_item_create_list(&body_item));

    uint8_t *body = NULL;
    size_t body_n = 0;
    expect_ok("secs_ii_encode(trace empty)",
              secs_ii_encode(body_item, &body, &body_n));
    secs_ii_item_destroy(body_item);

    char *out_name = NULL;
    secs_sml_match_trace_t *traces = NULL;
    size_t trace_n = 123;
    expect_ok("secs_sml_runtime_match_response_with_trace(empty rules)",
              secs_sml_runtime_match_response_with_trace(
                  rt, 1, 1, body, body_n, NULL, &out_name, &traces, &trace_n));
    if (out_name || traces || trace_n != 0) {
        fprintf(stderr,
                "FAIL: match_response_with_trace(empty rules) expected "
                "out_name=NULL traces=NULL trace_n=0\n");
        ++g_failures;
    }
    if (out_name) {
        secs_free(out_name);
    }
    secs_sml_match_traces_free(traces, trace_n);
    secs_free(body);

    secs_sml_runtime_destroy(rt);
}

struct open_args {
    secs_hsms_session_t *sess;
    secs_hsms_connection_t **io_conn;
    secs_error_t out_err;
};

static void *open_passive_thread(void *p) {
    struct open_args *args = (struct open_args *)p;
    args->out_err =
        secs_hsms_session_open_passive_connection(args->sess, args->io_conn);
    return NULL;
}

struct handler_ud {
    secs_protocol_session_t *server_proto;
};

static secs_error_t server_handler(void *user_data,
                                   const secs_data_message_view_t *request,
                                   uint8_t **out_body,
                                   size_t *out_body_n) {
    (void)request;
    struct handler_ud *ud = (struct handler_ud *)user_data;

    /* “恶意/误用”用例：在 io 线程里调用阻塞式 API，应返回 WRONG_THREAD */
    secs_data_message_t dummy;
    memset(&dummy, 0, sizeof(dummy));
    secs_error_t wrong = secs_protocol_session_request(
        ud->server_proto, 9, 9, NULL, 0, 1, &dummy);
    secs_data_message_free(&dummy);

    const uint8_t ok_flag =
        (wrong.value == (int)SECS_C_API_WRONG_THREAD) ? 1u : 0u;

    *out_body_n = 3;
    *out_body = (uint8_t *)secs_malloc(*out_body_n);
    if (!*out_body) {
        secs_error_t oom;
        oom.value = (int)SECS_C_API_OUT_OF_MEMORY;
        oom.category = "secs.c_api";
        return oom;
    }
    (*out_body)[0] = ok_flag;
    (*out_body)[1] = 0xBEu;
    (*out_body)[2] = 0xEFu;

    /* 成功：value==0 即可，category 对成功无强制要求 */
    {
        secs_error_t ok;
        ok.value = 0;
        ok.category = "secs.c_api";
        return ok;
    }
}

static secs_error_t client_echo_handler(void *user_data,
                                       const secs_data_message_view_t *request,
                                       uint8_t **out_body,
                                       size_t *out_body_n) {
    (void)user_data;
    if (!request || !out_body || !out_body_n) {
        secs_error_t err;
        err.value = (int)SECS_C_API_INVALID_ARGUMENT;
        err.category = "secs.c_api";
        return err;
    }

    const size_t n = request->body_n + 1u;
    *out_body_n = n;
    *out_body = (uint8_t *)secs_malloc(n);
    if (!*out_body) {
        secs_error_t oom;
        oom.value = (int)SECS_C_API_OUT_OF_MEMORY;
        oom.category = "secs.c_api";
        return oom;
    }

    if (request->body_n > 0 && request->body) {
        memcpy(*out_body, request->body, request->body_n);
    }
    (*out_body)[n - 1u] = 0x99u;

    {
        secs_error_t ok;
        ok.value = 0;
        ok.category = "secs.c_api";
        return ok;
    }
}

static secs_error_t protocol_append_tag_handler(void *user_data,
                                                const secs_data_message_view_t *request,
                                                uint8_t **out_body,
                                                size_t *out_body_n) {
    if (!user_data || !request || !out_body || !out_body_n) {
        secs_error_t err;
        err.value = (int)SECS_C_API_INVALID_ARGUMENT;
        err.category = "secs.c_api";
        return err;
    }

    const uint8_t tag = *(const uint8_t *)user_data;

    const size_t n = request->body_n + 1u;
    *out_body_n = n;
    *out_body = (uint8_t *)secs_malloc(n);
    if (!*out_body) {
        secs_error_t oom;
        oom.value = (int)SECS_C_API_OUT_OF_MEMORY;
        oom.category = "secs.c_api";
        return oom;
    }

    if (request->body_n > 0 && request->body) {
        memcpy(*out_body, request->body, request->body_n);
    }
    (*out_body)[n - 1u] = tag;

    {
        secs_error_t ok;
        ok.value = 0;
        ok.category = "secs.c_api";
        return ok;
    }
}

static secs_error_t
protocol_decoded_add_one_u2_handler(void *user_data,
                                    const secs_data_message_view_t *request,
                                    const secs_ii_item_t *decoded_body,
                                    secs_ii_item_t **out_item_body) {
    (void)user_data;
    (void)request;
    if (!decoded_body || !out_item_body) {
        secs_error_t err;
        err.value = (int)SECS_C_API_INVALID_ARGUMENT;
        err.category = "secs.c_api";
        return err;
    }

    *out_item_body = NULL;

    uint16_t in = 0;
    secs_error_t e = secs_ii_item_get_u2_at_list_path(decoded_body, &in, NULL, 0);
    if (e.value != 0) {
        return e;
    }

    uint16_t out = (uint16_t)(in + 1u);
    return secs_ii_item_create_u2(&out, 1, out_item_body);
}

struct hsms_wrong_thread_ud {
    secs_hsms_session_t *server_hsms;
};

static secs_error_t
server_handler_hsms_wrong_thread(void *user_data,
                                 const secs_data_message_view_t *request,
                                 uint8_t **out_body,
                                 size_t *out_body_n) {
    (void)request;
    struct hsms_wrong_thread_ud *ud = (struct hsms_wrong_thread_ud *)user_data;

    /* “恶意/误用”用例：在 io 线程里调用 HSMS 的阻塞式 API，也必须返回 WRONG_THREAD
     *（覆盖多个 return-bridge 分支）。 */
    int selected = 0;
    uint32_t sb = 0;

    secs_hsms_data_message_t rx;
    memset(&rx, 0, sizeof(rx));
    secs_hsms_data_message_t reply;
    memset(&reply, 0, sizeof(reply));

    const secs_error_t e_is_selected =
        secs_hsms_session_is_selected(ud->server_hsms, &selected);
    const secs_error_t e_linktest = secs_hsms_session_linktest(ud->server_hsms);
    const secs_error_t e_send_auto =
        secs_hsms_session_send_data_auto_system_bytes(
            ud->server_hsms, 1, 1, 0, NULL, 0, &sb);
    const secs_error_t e_send_with =
        secs_hsms_session_send_data_with_system_bytes(
            ud->server_hsms, 1, 1, 0, 0x12345678u, NULL, 0);
    const secs_error_t e_recv =
        secs_hsms_session_receive_data(ud->server_hsms, 1, &rx);
    const secs_error_t e_req = secs_hsms_session_request_data(
        ud->server_hsms, 1, 1, NULL, 0, 1, &reply);

    /* WRONG_THREAD 下不应分配输出资源；此处仍调用 free 以防未来实现调整。 */
    secs_hsms_data_message_free(&rx);
    secs_hsms_data_message_free(&reply);

    const uint8_t ok_is_selected =
        (e_is_selected.value == (int)SECS_C_API_WRONG_THREAD) ? 1u : 0u;
    const uint8_t ok_linktest =
        (e_linktest.value == (int)SECS_C_API_WRONG_THREAD) ? 1u : 0u;
    const uint8_t ok_send_auto =
        (e_send_auto.value == (int)SECS_C_API_WRONG_THREAD) ? 1u : 0u;
    const uint8_t ok_send_with =
        (e_send_with.value == (int)SECS_C_API_WRONG_THREAD) ? 1u : 0u;
    const uint8_t ok_recv =
        (e_recv.value == (int)SECS_C_API_WRONG_THREAD) ? 1u : 0u;
    const uint8_t ok_req =
        (e_req.value == (int)SECS_C_API_WRONG_THREAD) ? 1u : 0u;

    *out_body_n = 6;
    *out_body = (uint8_t *)secs_malloc(*out_body_n);
    if (!*out_body) {
        secs_error_t oom;
        oom.value = (int)SECS_C_API_OUT_OF_MEMORY;
        oom.category = "secs.c_api";
        return oom;
    }

    (*out_body)[0] = ok_is_selected;
    (*out_body)[1] = ok_linktest;
    (*out_body)[2] = ok_send_auto;
    (*out_body)[3] = ok_send_with;
    (*out_body)[4] = ok_recv;
    (*out_body)[5] = ok_req;

    secs_error_t ok;
    ok.value = 0;
    ok.category = "secs.c_api";
    return ok;
}

static secs_error_t
server_handler_empty(void *user_data,
                     const secs_data_message_view_t *request,
                     uint8_t **out_body,
                     size_t *out_body_n) {
    (void)user_data;
    (void)request;

    /* 返回“空 body”的合法成功响应：用于覆盖 out_n==0 的路径 */
    *out_body = NULL;
    *out_body_n = 0;

    secs_error_t ok;
    ok.value = 0;
    ok.category = "secs.c_api";
    return ok;
}

struct ceid_handler_ud {
    atomic_int *called;
    atomic_int *last_ceid;
    const char *tag;
    int32_t ceid_delta;
};

static secs_error_t
ceid_dispatcher_reply_list_handler(void *user_data,
                                  uint32_t ceid,
                                  const secs_data_message_view_t *request,
                                  uint8_t **out_body,
                                  size_t *out_body_n) {
    (void)request;
    struct ceid_handler_ud *ud = (struct ceid_handler_ud *)user_data;

    if (ud && ud->called) {
        (void)atomic_fetch_add(ud->called, 1);
    }
    if (ud && ud->last_ceid) {
        atomic_store(ud->last_ceid, (int)ceid);
    }

    const char *tag = (ud && ud->tag) ? ud->tag : "ACK";

    /* 默认回显 request CEID；可选 delta 用于构造“CEID 不一致”场景 */
    uint32_t reply_ceid = ceid;
    if (ud && ud->ceid_delta != 0) {
        int64_t tmp = (int64_t)ceid + (int64_t)ud->ceid_delta;
        if (tmp < 0) {
            tmp = 0;
        }
        if (tmp > (int64_t)UINT32_MAX) {
            tmp = (int64_t)UINT32_MAX;
        }
        reply_ceid = (uint32_t)tmp;
    }

    *out_body = NULL;
    *out_body_n = 0;

    secs_ii_item_t *body = NULL;
    secs_ii_item_t *dataid_item = NULL;
    secs_ii_item_t *ceid_item = NULL;
    secs_ii_item_t *tag_item = NULL;
    secs_ii_item_t *empty_list = NULL;

    secs_error_t err = secs_ii_item_create_list(&body);
    if (err.value != 0) {
        return err;
    }

    /* <L <U4 DATAID> <U4 CEID> <A TAG> <L>> */
    {
        uint32_t dataid = 1;
        err = secs_ii_item_create_u4(&dataid, 1, &dataid_item);
        if (err.value != 0) {
            goto cleanup;
        }

        err = secs_ii_item_create_u4(&reply_ceid, 1, &ceid_item);
        if (err.value != 0) {
            goto cleanup;
        }

        err = secs_ii_item_create_ascii(tag, strlen(tag), &tag_item);
        if (err.value != 0) {
            goto cleanup;
        }

        err = secs_ii_item_create_list(&empty_list);
        if (err.value != 0) {
            goto cleanup;
        }

        err = secs_ii_item_list_append(body, dataid_item);
        if (err.value != 0) {
            goto cleanup;
        }
        err = secs_ii_item_list_append(body, ceid_item);
        if (err.value != 0) {
            goto cleanup;
        }
        err = secs_ii_item_list_append(body, tag_item);
        if (err.value != 0) {
            goto cleanup;
        }
        err = secs_ii_item_list_append(body, empty_list);
        if (err.value != 0) {
            goto cleanup;
        }
    }

    err = secs_ii_encode(body, out_body, out_body_n);

cleanup:
    secs_ii_item_destroy(dataid_item);
    secs_ii_item_destroy(ceid_item);
    secs_ii_item_destroy(tag_item);
    secs_ii_item_destroy(empty_list);
    secs_ii_item_destroy(body);
    return err;
}

static secs_error_t build_ceid_request_body(uint32_t ceid,
                                            uint8_t **out_body,
                                            size_t *out_body_n) {
    if (!out_body || !out_body_n) {
        secs_error_t err;
        err.value = (int)SECS_C_API_INVALID_ARGUMENT;
        err.category = "secs.c_api";
        return err;
    }

    *out_body = NULL;
    *out_body_n = 0;

    secs_ii_item_t *body = NULL;
    secs_ii_item_t *dataid_item = NULL;
    secs_ii_item_t *ceid_item = NULL;
    secs_ii_item_t *empty_list = NULL;

    secs_error_t err = secs_ii_item_create_list(&body);
    if (err.value != 0) {
        return err;
    }

    {
        uint32_t dataid = 1;
        err = secs_ii_item_create_u4(&dataid, 1, &dataid_item);
        if (err.value != 0) {
            goto cleanup;
        }
        err = secs_ii_item_create_u4(&ceid, 1, &ceid_item);
        if (err.value != 0) {
            goto cleanup;
        }
        err = secs_ii_item_create_list(&empty_list);
        if (err.value != 0) {
            goto cleanup;
        }

        err = secs_ii_item_list_append(body, dataid_item);
        if (err.value != 0) {
            goto cleanup;
        }
        err = secs_ii_item_list_append(body, ceid_item);
        if (err.value != 0) {
            goto cleanup;
        }
        err = secs_ii_item_list_append(body, empty_list);
        if (err.value != 0) {
            goto cleanup;
        }
    }

    err = secs_ii_encode(body, out_body, out_body_n);

cleanup:
    secs_ii_item_destroy(dataid_item);
    secs_ii_item_destroy(ceid_item);
    secs_ii_item_destroy(empty_list);
    secs_ii_item_destroy(body);
    return err;
}

typedef enum ceid_num_type {
    CEID_NUM_U1 = 1,
    CEID_NUM_U2 = 2,
    CEID_NUM_U4 = 4,
    CEID_NUM_U8 = 8,
} ceid_num_type_t;

static secs_error_t build_ceid_request_body_with_ceid_values(ceid_num_type_t ty,
                                                             const void *values,
                                                             size_t values_n,
                                                             uint8_t **out_body,
                                                             size_t *out_body_n) {
    if (!out_body || !out_body_n) {
        secs_error_t err;
        err.value = (int)SECS_C_API_INVALID_ARGUMENT;
        err.category = "secs.c_api";
        return err;
    }
    if (!values && values_n != 0) {
        secs_error_t err;
        err.value = (int)SECS_C_API_INVALID_ARGUMENT;
        err.category = "secs.c_api";
        return err;
    }

    *out_body = NULL;
    *out_body_n = 0;

    secs_ii_item_t *body = NULL;
    secs_ii_item_t *dataid_item = NULL;
    secs_ii_item_t *ceid_item = NULL;
    secs_ii_item_t *empty_list = NULL;

    secs_error_t err = secs_ii_item_create_list(&body);
    if (err.value != 0) {
        return err;
    }

    {
        uint32_t dataid = 1;
        err = secs_ii_item_create_u4(&dataid, 1, &dataid_item);
        if (err.value != 0) {
            goto cleanup;
        }

        switch (ty) {
        case CEID_NUM_U1:
            err = secs_ii_item_create_u1((const uint8_t *)values, values_n, &ceid_item);
            break;
        case CEID_NUM_U2:
            err = secs_ii_item_create_u2((const uint16_t *)values, values_n, &ceid_item);
            break;
        case CEID_NUM_U4:
            err = secs_ii_item_create_u4((const uint32_t *)values, values_n, &ceid_item);
            break;
        case CEID_NUM_U8:
            err = secs_ii_item_create_u8((const uint64_t *)values, values_n, &ceid_item);
            break;
        default:
            err.value = (int)SECS_C_API_INVALID_ARGUMENT;
            err.category = "secs.c_api";
            break;
        }
        if (err.value != 0) {
            goto cleanup;
        }

        err = secs_ii_item_create_list(&empty_list);
        if (err.value != 0) {
            goto cleanup;
        }

        err = secs_ii_item_list_append(body, dataid_item);
        if (err.value != 0) {
            goto cleanup;
        }
        err = secs_ii_item_list_append(body, ceid_item);
        if (err.value != 0) {
            goto cleanup;
        }
        err = secs_ii_item_list_append(body, empty_list);
        if (err.value != 0) {
            goto cleanup;
        }
    }

    err = secs_ii_encode(body, out_body, out_body_n);

cleanup:
    secs_ii_item_destroy(dataid_item);
    secs_ii_item_destroy(ceid_item);
    secs_ii_item_destroy(empty_list);
    secs_ii_item_destroy(body);
    return err;
}

struct hsms_req_args {
    secs_hsms_session_t *server;
    secs_error_t recv_err;
    secs_error_t send_err;
};

struct hsms_recv_args {
    secs_hsms_session_t *server;
    uint32_t timeout_ms;
    secs_error_t recv_err;
    secs_hsms_data_message_t msg;
};

static void *hsms_receive_thread(void *p) {
    struct hsms_recv_args *a = (struct hsms_recv_args *)p;
    a->recv_err =
        secs_hsms_session_receive_data(a->server, a->timeout_ms, &a->msg);
    return NULL;
}

static void *hsms_request_response_thread(void *p) {
    struct hsms_req_args *a = (struct hsms_req_args *)p;

    secs_hsms_data_message_t req;
    memset(&req, 0, sizeof(req));

    a->recv_err = secs_hsms_session_receive_data(a->server, 1000, &req);
    if (a->recv_err.value == 0) {
        const uint8_t rsp_body[2] = {0xCAu, 0xFEu};
        a->send_err = secs_hsms_session_send_data_with_system_bytes(
            a->server,
            req.stream,
            (uint8_t)(req.function + 1u),
            0,
            req.system_bytes,
            rsp_body,
            sizeof(rsp_body));
    }

    secs_hsms_data_message_free(&req);
    return NULL;
}

static secs_error_t
protocol_bad_handler_returns_error(void *user_data,
                                   const secs_data_message_view_t *request,
                                   uint8_t **out_body,
                                   size_t *out_body_n) {
    (void)user_data;
    (void)request;

    *out_body_n = 1;
    *out_body = (uint8_t *)secs_malloc(1);
    if (*out_body) {
        (*out_body)[0] = 0x42u;
    }

    secs_error_t err;
    err.value = (int)SECS_C_API_INVALID_ARGUMENT;
    err.category = "secs.c_api";
    return err;
}

static secs_error_t
protocol_bad_handler_body_null_nonzero(void *user_data,
                                       const secs_data_message_view_t *request,
                                       uint8_t **out_body,
                                       size_t *out_body_n) {
    (void)user_data;
    (void)request;

    *out_body = NULL;
    *out_body_n = 1;

    secs_error_t ok;
    ok.value = 0;
    ok.category = "secs.c_api";
    return ok;
}

static secs_error_t
protocol_empty_response_handler(void *user_data,
                                const secs_data_message_view_t *request,
                                uint8_t **out_body,
                                size_t *out_body_n) {
    (void)user_data;
    (void)request;
    *out_body = NULL;
    *out_body_n = 0;

    secs_error_t ok;
    ok.value = 0;
    ok.category = "secs.c_api";
    return ok;
}

struct control_stop_ud {
    secs_protocol_session_t *server_proto;
    secs_hsms_session_t *server_hsms;
    atomic_int *called;
};

static secs_error_t
protocol_control_stop_handler(void *user_data,
                              const secs_data_message_view_t *request,
                              uint8_t **out_body,
                              size_t *out_body_n) {
    (void)request;
    struct control_stop_ud *ud = (struct control_stop_ud *)user_data;

    /* 在 io 线程内调用 stop：覆盖 c_api.cpp 的 is_io_thread 分支 */
    (void)secs_protocol_session_stop(ud->server_proto);
    (void)secs_hsms_session_stop(ud->server_hsms);

    atomic_store(ud->called, 1);

    *out_body = NULL;
    *out_body_n = 0;

    secs_error_t ok;
    ok.value = 0;
    ok.category = "secs.c_api";
    return ok;
}

struct poll_once_wrong_thread_ud {
    secs_protocol_session_t *server_proto;
    atomic_int *called;
};

static secs_error_t
protocol_poll_once_wrong_thread_handler(void *user_data,
                                        const secs_data_message_view_t *request,
                                        uint8_t **out_body,
                                        size_t *out_body_n) {
    (void)request;
    struct poll_once_wrong_thread_ud *ud =
        (struct poll_once_wrong_thread_ud *)user_data;

    int handled = 0;
    secs_error_t err = secs_protocol_session_poll_once(ud->server_proto, 1, &handled);
    if (err.value == (int)SECS_C_API_WRONG_THREAD) {
        atomic_store(ud->called, 1);
    } else {
        atomic_store(ud->called, 2);
    }

    *out_body = NULL;
    *out_body_n = 0;

    secs_error_t ok;
    ok.value = 0;
    ok.category = "secs.c_api";
    return ok;
}

static void test_hsms_protocol_loopback(void) {
    secs_context_t *ctx = NULL;
    expect_ok("secs_context_create", secs_context_create(&ctx));

    secs_hsms_connection_t *client_conn = NULL;
    secs_hsms_connection_t *server_conn = NULL;
    expect_ok("secs_hsms_connection_create_memory_duplex",
              secs_hsms_connection_create_memory_duplex(
                  ctx, &client_conn, &server_conn));

    secs_hsms_session_options_t hsms_opt;
    memset(&hsms_opt, 0, sizeof(hsms_opt));
    hsms_opt.session_id = 0x1010;
    hsms_opt.t3_ms = 2000;
    hsms_opt.t5_ms = 200;
    hsms_opt.t6_ms = 2000;
    hsms_opt.t7_ms = 2000;
    hsms_opt.t8_ms = 2000;
    hsms_opt.linktest_interval_ms = 0;
    hsms_opt.auto_reconnect = 0;
    hsms_opt.passive_accept_select = 1;

    secs_hsms_session_t *client_hsms = NULL;
    secs_hsms_session_t *server_hsms = NULL;
    expect_ok("secs_hsms_session_create(client)",
              secs_hsms_session_create(ctx, &hsms_opt, &client_hsms));
    expect_ok("secs_hsms_session_create(server)",
              secs_hsms_session_create(ctx, &hsms_opt, &server_hsms));

    /* 需要并发：被动端会阻塞等待 SELECT，主动端需要同时发起 SELECT */
    pthread_t th;
    struct open_args args;
    memset(&args, 0, sizeof(args));
    args.sess = server_hsms;
    args.io_conn = &server_conn;
    if (pthread_create(&th, NULL, open_passive_thread, &args) != 0) {
        fprintf(stderr, "FAIL: pthread_create\n");
        ++g_failures;
    }

    expect_ok(
        "secs_hsms_session_open_active_connection",
        secs_hsms_session_open_active_connection(client_hsms, &client_conn));

    (void)pthread_join(th, NULL);
    expect_ok("secs_hsms_session_open_passive_connection", args.out_err);

    /* selected 状态校验 */
    {
        int selected = 0;
        expect_ok("secs_hsms_session_is_selected(client)",
                  secs_hsms_session_is_selected(client_hsms, &selected));
        if (!selected) {
            fprintf(stderr, "FAIL: client not selected\n");
            ++g_failures;
        }
        selected = 0;
        expect_ok("secs_hsms_session_is_selected(server)",
                  secs_hsms_session_is_selected(server_hsms, &selected));
        if (!selected) {
            fprintf(stderr, "FAIL: server not selected\n");
            ++g_failures;
        }
    }

    /* HSMS：错误分支覆盖（用“未 selected”的临时会话，避免影响主链路） */
    {
        secs_hsms_session_t *tmp = NULL;
        expect_ok("secs_hsms_session_create(tmp for err branches)",
                  secs_hsms_session_create(ctx, &hsms_opt, &tmp));

        /* receive_data：在无入站数据时应超时返回（不应永久阻塞） */
        {
            secs_hsms_data_message_t rx;
            memset(&rx, 0, sizeof(rx));
            secs_error_t err = secs_hsms_session_receive_data(tmp, 10, &rx);
            expect_err("secs_hsms_session_receive_data(timeout)", err);
            secs_hsms_data_message_free(&rx);
        }

        /* request_data：未 selected 时应返回错误（不触发断线逻辑） */
        {
            const uint8_t body[1] = {0x01u};
            secs_hsms_data_message_t reply;
            memset(&reply, 0, sizeof(reply));
            secs_error_t err = secs_hsms_session_request_data(
                tmp, 4, 1, body, sizeof(body), 10, &reply);
            expect_err("secs_hsms_session_request_data(not selected)", err);
            secs_hsms_data_message_free(&reply);
        }

        secs_hsms_session_destroy(tmp);
    }

    /* HSMS：显式 LINKTEST（覆盖控制事务路径） */
    expect_ok("secs_hsms_session_linktest(client)",
              secs_hsms_session_linktest(client_hsms));

    /* HSMS：发送 W=0 并在对端接收（覆盖 send/receive） */
    {
        const uint8_t body[3] = {0x11u, 0x22u, 0x33u};
        uint32_t sb = 0;
        expect_ok("secs_hsms_session_send_data_auto_system_bytes",
                  secs_hsms_session_send_data_auto_system_bytes(
                      client_hsms, 2, 1, 0, body, sizeof(body), &sb));
        if (sb == 0u) {
            fprintf(stderr, "FAIL: system_bytes should not be 0\n");
            ++g_failures;
        }

        secs_hsms_data_message_t rx;
        memset(&rx, 0, sizeof(rx));
        expect_ok("secs_hsms_session_receive_data",
                  secs_hsms_session_receive_data(server_hsms, 1000, &rx));
        if (rx.stream != 2u || rx.function != 1u || rx.w_bit != 0 ||
            rx.system_bytes != sb) {
            fprintf(stderr, "FAIL: hsms receive meta mismatch\n");
            ++g_failures;
        }
        if (rx.body_n != sizeof(body) ||
            memcmp(rx.body, body, sizeof(body)) != 0) {
            fprintf(stderr, "FAIL: hsms receive body mismatch\n");
            ++g_failures;
        }
        secs_hsms_data_message_free(&rx);
    }

    /* 参数校验：body_bytes==NULL 且 body_n>0 必须快速失败 */
    {
        uint32_t sb = 0;
        secs_error_t err = secs_hsms_session_send_data_auto_system_bytes(
            client_hsms, 1, 1, 0, NULL, 1, &sb);
        expect_err("secs_hsms_session_send_data_auto_system_bytes(NULL,1)",
                   err);
        if (err.value != (int)SECS_C_API_INVALID_ARGUMENT) {
            failf("secs_hsms_session_send_data_auto_system_bytes(NULL,1)", err);
        }

        err = secs_hsms_session_send_data_with_system_bytes(
            client_hsms, 1, 1, 0, 0x12345678u, NULL, 1);
        expect_err("secs_hsms_session_send_data_with_system_bytes(NULL,1)",
                   err);
        if (err.value != (int)SECS_C_API_INVALID_ARGUMENT) {
            failf("secs_hsms_session_send_data_with_system_bytes(NULL,1)", err);
        }

        secs_hsms_data_message_t reply;
        memset(&reply, 0, sizeof(reply));
        err = secs_hsms_session_request_data(
            client_hsms, 1, 1, NULL, 1, 100, &reply);
        expect_err("secs_hsms_session_request_data(NULL,1)", err);
        if (err.value != (int)SECS_C_API_INVALID_ARGUMENT) {
            failf("secs_hsms_session_request_data(NULL,1)", err);
        }
        secs_hsms_data_message_free(&reply);
    }

    /* is_selected 参数校验：sess/out_selected 为空 */
    {
        int selected = 0;
        secs_error_t err =
            secs_hsms_session_is_selected(client_hsms, (int *)NULL);
        expect_err("secs_hsms_session_is_selected(NULL out)", err);
        if (err.value != (int)SECS_C_API_INVALID_ARGUMENT) {
            failf("secs_hsms_session_is_selected(NULL out)", err);
        }

        err = secs_hsms_session_is_selected(NULL, &selected);
        expect_err("secs_hsms_session_is_selected(NULL sess)", err);
        if (err.value != (int)SECS_C_API_INVALID_ARGUMENT) {
            failf("secs_hsms_session_is_selected(NULL sess)", err);
        }
    }

    /* HSMS：out_system_bytes==NULL 分支 */
    {
        const uint8_t body[1] = {0x44u};
        expect_ok("secs_hsms_session_send_data_auto_system_bytes(out=NULL)",
                  secs_hsms_session_send_data_auto_system_bytes(
                      client_hsms, 9, 1, 0, body, sizeof(body), NULL));

        secs_hsms_data_message_t rx;
        memset(&rx, 0, sizeof(rx));
        expect_ok("secs_hsms_session_receive_data(out_system_bytes NULL case)",
                  secs_hsms_session_receive_data(server_hsms, 1000, &rx));
        secs_hsms_data_message_free(&rx);
    }

    /* HSMS：空 body + timeout_ms==0（无超时）路径 */
    {
        struct hsms_recv_args rx_args;
        memset(&rx_args, 0, sizeof(rx_args));
        rx_args.server = server_hsms;
        rx_args.timeout_ms = 0;

        pthread_t rx_th;
        int rx_started =
            pthread_create(&rx_th, NULL, hsms_receive_thread, &rx_args);
        if (rx_started != 0) {
            fprintf(stderr, "FAIL: pthread_create(hsms_receive)\n");
            ++g_failures;
        }

        uint32_t sb = 0;
        expect_ok("secs_hsms_session_send_data_auto_system_bytes(empty)",
                  secs_hsms_session_send_data_auto_system_bytes(
                      client_hsms, 8, 1, 0, NULL, 0, &sb));

        if (rx_started == 0) {
            (void)pthread_join(rx_th, NULL);
            expect_ok("secs_hsms_session_receive_data(timeout=0)",
                      rx_args.recv_err);

            if (rx_args.msg.stream != 8u || rx_args.msg.function != 1u ||
                rx_args.msg.w_bit != 0 || rx_args.msg.system_bytes != sb) {
                fprintf(stderr, "FAIL: hsms receive(empty) meta mismatch\n");
                ++g_failures;
            }
            if (rx_args.msg.body_n != 0u || rx_args.msg.body != NULL) {
                fprintf(stderr,
                        "FAIL: hsms receive(empty) should return body=NULL, "
                        "body_n=0\n");
                ++g_failures;
            }
            secs_hsms_data_message_free(&rx_args.msg);
        }
    }

    /* HSMS：request/response（覆盖 request_data + send_data_with_system_bytes）
     */
    {
        struct hsms_req_args hsms_args;
        memset(&hsms_args, 0, sizeof(hsms_args));
        hsms_args.server = server_hsms;

        pthread_t hsms_th;
        if (pthread_create(
                &hsms_th, NULL, hsms_request_response_thread, &hsms_args) !=
            0) {
            fprintf(stderr, "FAIL: pthread_create(hsms)\n");
            ++g_failures;
        }

        const uint8_t req_body[1] = {0x7Fu};
        secs_hsms_data_message_t reply;
        memset(&reply, 0, sizeof(reply));
        expect_ok(
            "secs_hsms_session_request_data",
            secs_hsms_session_request_data(
                client_hsms, 3, 1, req_body, sizeof(req_body), 1000, &reply));
        if (reply.body_n != 2u || !reply.body || reply.body[0] != 0xCAu ||
            reply.body[1] != 0xFEu) {
            fprintf(stderr, "FAIL: hsms request reply mismatch\n");
            ++g_failures;
        }
        secs_hsms_data_message_free(&reply);

        (void)pthread_join(hsms_th, NULL);
        expect_ok("hsms server receive(req)", hsms_args.recv_err);
        expect_ok("hsms server send(rsp)", hsms_args.send_err);
    }

    /* open_active_ip：非法 IP 应直接失败（覆盖参数解析分支） */
    {
        secs_error_t err =
            secs_hsms_session_open_active_ip(client_hsms, "not_an_ip", 1);
        expect_err("secs_hsms_session_open_active_ip(not_an_ip)", err);
    }

    /* open_passive_ip：参数校验 + 非法 IP 解析（不触发 listen/accept） */
    {
        secs_error_t err =
            secs_hsms_session_open_passive_ip(NULL, "127.0.0.1", 1);
        expect_err("secs_hsms_session_open_passive_ip(NULL sess)", err);
        if (err.value != (int)SECS_C_API_INVALID_ARGUMENT) {
            failf("secs_hsms_session_open_passive_ip(NULL sess)", err);
        }

        err = secs_hsms_session_open_passive_ip(client_hsms, NULL, 1);
        expect_err("secs_hsms_session_open_passive_ip(NULL ip)", err);
        if (err.value != (int)SECS_C_API_INVALID_ARGUMENT) {
            failf("secs_hsms_session_open_passive_ip(NULL ip)", err);
        }

        err = secs_hsms_session_open_passive_ip(client_hsms, "not_an_ip", 1);
        expect_err("secs_hsms_session_open_passive_ip(not_an_ip)", err);
    }

    atomic_int dump_calls;
    atomic_init(&dump_calls, 0);

    secs_protocol_session_options_v2_t client_proto_opt;
    memset(&client_proto_opt, 0, sizeof(client_proto_opt));
    client_proto_opt.t3_ms = 1000;
    client_proto_opt.poll_interval_ms = 5;

    secs_protocol_session_options_v2_t server_proto_opt;
    memset(&server_proto_opt, 0, sizeof(server_proto_opt));
    server_proto_opt.t3_ms = 1000;
    server_proto_opt.poll_interval_ms = 5;
    server_proto_opt.dump_flags = (uint32_t)SECS_PROTOCOL_DUMP_ENABLE;
    server_proto_opt.dump_sink = proto_dump_sink;
    server_proto_opt.dump_sink_user = &dump_calls;

    secs_protocol_session_t *client_proto = NULL;
    secs_protocol_session_t *server_proto = NULL;
    expect_ok(
        "secs_protocol_session_create_from_hsms_v2(client)",
        secs_protocol_session_create_from_hsms_v2(ctx,
                                                  client_hsms,
                                                  hsms_opt.session_id,
                                                  &client_proto_opt,
                                                  &client_proto));
    expect_ok(
        "secs_protocol_session_create_from_hsms_v2(server)",
        secs_protocol_session_create_from_hsms_v2(ctx,
                                                  server_hsms,
                                                  hsms_opt.session_id,
                                                  &server_proto_opt,
                                                  &server_proto));

    /* 参数校验：protocol send/request 的 body 指针/长度不一致必须拒绝 */
    {
        secs_error_t err =
            secs_protocol_session_send(client_proto, 1, 1, NULL, 1);
        expect_err("secs_protocol_session_send(NULL,1)", err);
        if (err.value != (int)SECS_C_API_INVALID_ARGUMENT) {
            failf("secs_protocol_session_send(NULL,1)", err);
        }

        secs_data_message_t reply;
        memset(&reply, 0, sizeof(reply));
        err = secs_protocol_session_request(
            client_proto, 1, 1, NULL, 1, 100, &reply);
        expect_err("secs_protocol_session_request(NULL,1)", err);
        if (err.value != (int)SECS_C_API_INVALID_ARGUMENT) {
            failf("secs_protocol_session_request(NULL,1)", err);
        }
        secs_data_message_free(&reply);

        err = secs_protocol_session_erase_handler(NULL, 1, 1);
        expect_err("secs_protocol_session_erase_handler(NULL)", err);
        if (err.value != (int)SECS_C_API_INVALID_ARGUMENT) {
            failf("secs_protocol_session_erase_handler(NULL)", err);
        }
    }

    /* 注册 handler：验证回包 + “在 io 线程误用阻塞 API 必须拒绝” */
    struct handler_ud ud;
    ud.server_proto = server_proto;
    expect_ok("secs_protocol_session_set_handler",
              secs_protocol_session_set_handler(
                  server_proto, 1, 1, server_handler, &ud));

    /* 在 io 线程内调用 poll_once（阻塞 API）必须返回 WRONG_THREAD */
    {
        atomic_int called;
        atomic_init(&called, 0);

        struct poll_once_wrong_thread_ud pud;
        pud.server_proto = server_proto;
        pud.called = &called;

        expect_ok("secs_protocol_session_set_handler(poll_once wrong_thread)",
                  secs_protocol_session_set_handler(server_proto,
                                                    8,
                                                    7,
                                                    protocol_poll_once_wrong_thread_handler,
                                                    &pud));
        expect_ok("secs_protocol_session_send(poll_once wrong_thread)",
                  secs_protocol_session_send(client_proto, 8, 7, NULL, 0));

        if (!wait_until_atomic_eq(&called, 1, 200, 5 * 1000 * 1000)) {
            fprintf(stderr,
                    "FAIL: poll_once wrong_thread handler not called/validated\n");
            ++g_failures;
        }

        expect_ok("secs_protocol_session_erase_handler(poll_once wrong_thread)",
                  secs_protocol_session_erase_handler(server_proto, 8, 7));
    }

    /* send：W=0 不需要回应（覆盖 send 路径） */
    {
        const uint8_t body[1] = {0xAAu};
        expect_ok(
            "secs_protocol_session_send",
            secs_protocol_session_send(client_proto, 2, 1, body, sizeof(body)));
        if (!wait_until_atomic_gt(&dump_calls, 0, 200, 1000000L)) {
            fprintf(stderr, "FAIL: protocol runtime dump sink not called\n");
            ++g_failures;
        }
        expect_ok("secs_protocol_session_erase_handler(no-op)",
                  secs_protocol_session_erase_handler(server_proto, 2, 2));
    }

    /* 发起 request：期待收到 3 字节，其中 [0]==1 表示 WRONG_THREAD 检测生效 */
    {
        const uint8_t req_body[2] = {0xDEu, 0xADu};
        secs_data_message_t reply;
        memset(&reply, 0, sizeof(reply));

        expect_ok(
            "secs_protocol_session_request",
            secs_protocol_session_request(
                client_proto, 1, 1, req_body, sizeof(req_body), 1000, &reply));

        if (reply.body_n != 3u || !reply.body) {
            fprintf(stderr, "FAIL: reply body invalid\n");
            ++g_failures;
        } else {
            if (reply.body[0] != 1u || reply.body[1] != 0xBEu ||
                reply.body[2] != 0xEFu) {
                fprintf(stderr, "FAIL: reply payload mismatch\n");
                ++g_failures;
            }
        }

        secs_data_message_free(&reply);
    }

    /* CEID dispatcher：按 CEID 分发 handler + request/reply CEID 校验 helper */
    {
        const size_t ceid_path[1] = {1}; /* <L <DATAID> <CEID> ...> */

        secs_ceid_dispatcher_t *cd = NULL;
        expect_ok("secs_ceid_dispatcher_create_list_path",
                  secs_ceid_dispatcher_create_list_path(
                      ceid_path, 1, NULL, 1, &cd));

        atomic_int called_exact;
        atomic_int last_exact;
        atomic_init(&called_exact, 0);
        atomic_init(&last_exact, 0);
        struct ceid_handler_ud exact_ud;
        exact_ud.called = &called_exact;
        exact_ud.last_ceid = &last_exact;
        exact_ud.tag = "ACK";
        exact_ud.ceid_delta = 0;

        atomic_int called_default;
        atomic_int last_default;
        atomic_init(&called_default, 0);
        atomic_init(&last_default, 0);
        struct ceid_handler_ud default_ud;
        default_ud.called = &called_default;
        default_ud.last_ceid = &last_default;
        default_ud.tag = "DEFAULT";
        default_ud.ceid_delta = 0;

        atomic_int called_mismatch;
        atomic_int last_mismatch;
        atomic_init(&called_mismatch, 0);
        atomic_init(&last_mismatch, 0);
        struct ceid_handler_ud mismatch_ud;
        mismatch_ud.called = &called_mismatch;
        mismatch_ud.last_ceid = &last_mismatch;
        mismatch_ud.tag = "MISMATCH";
        mismatch_ud.ceid_delta = 1;

        expect_ok("secs_ceid_dispatcher_set_handler(100)",
                  secs_ceid_dispatcher_set_handler(
                      cd, 100, ceid_dispatcher_reply_list_handler, &exact_ud));
        expect_ok("secs_ceid_dispatcher_set_handler(777)",
                  secs_ceid_dispatcher_set_handler(
                      cd,
                      777,
                      ceid_dispatcher_reply_list_handler,
                      &mismatch_ud));
        expect_ok("secs_ceid_dispatcher_set_default_handler",
                  secs_ceid_dispatcher_set_default_handler(
                      cd, ceid_dispatcher_reply_list_handler, &default_ud));
        expect_ok("secs_protocol_session_set_ceid_dispatcher",
                  secs_protocol_session_set_ceid_dispatcher(
                      server_proto, 6, 11, cd));

        /* 1) 命中 CEID=100 */
        {
            uint8_t *req = NULL;
            size_t req_n = 0;
            expect_ok("build_ceid_request_body(100)",
                      build_ceid_request_body(100, &req, &req_n));

            secs_data_message_t reply;
            memset(&reply, 0, sizeof(reply));
            expect_ok("secs_protocol_session_request(ceid=100)",
                      secs_protocol_session_request(
                          client_proto, 6, 11, req, req_n, 1000, &reply));

            if (atomic_load(&called_exact) != 1 ||
                atomic_load(&last_exact) != 100) {
                fprintf(stderr, "FAIL: CEID exact handler not called as expected\n");
                ++g_failures;
            }

            /* 解码回应并验证 tag */
            {
                size_t consumed = 0;
                secs_ii_item_t *root = NULL;
                expect_ok("secs_ii_decode_one(reply)",
                          secs_ii_decode_one(
                              reply.body, reply.body_n, &consumed, &root));
                secs_ii_item_t *tag_item = NULL;
                expect_ok("secs_ii_item_list_get(tag)",
                          secs_ii_item_list_get(root, 2, &tag_item));
                const char *ptr = NULL;
                size_t n = 0;
                expect_ok("secs_ii_item_ascii_view(tag)",
                          secs_ii_item_ascii_view(tag_item, &ptr, &n));
                if (n != 3u || memcmp(ptr, "ACK", 3u) != 0) {
                    fprintf(stderr, "FAIL: CEID reply tag mismatch (ACK)\n");
                    ++g_failures;
                }
                secs_ii_item_destroy(tag_item);
                secs_ii_item_destroy(root);
            }

            secs_data_message_free(&reply);
            secs_free(req);
        }

        /* 2) 未注册 CEID：走 default */
        {
            uint8_t *req = NULL;
            size_t req_n = 0;
            expect_ok("build_ceid_request_body(123)",
                      build_ceid_request_body(123, &req, &req_n));

            secs_data_message_t reply;
            memset(&reply, 0, sizeof(reply));
            expect_ok("secs_protocol_session_request(ceid=123)",
                      secs_protocol_session_request(
                          client_proto, 6, 11, req, req_n, 1000, &reply));

            if (atomic_load(&called_default) != 1 ||
                atomic_load(&last_default) != 123) {
                fprintf(stderr, "FAIL: CEID default handler not called as expected\n");
                ++g_failures;
            }

            {
                size_t consumed = 0;
                secs_ii_item_t *root = NULL;
                expect_ok("secs_ii_decode_one(reply default)",
                          secs_ii_decode_one(
                              reply.body, reply.body_n, &consumed, &root));
                secs_ii_item_t *tag_item = NULL;
                expect_ok("secs_ii_item_list_get(tag default)",
                          secs_ii_item_list_get(root, 2, &tag_item));
                const char *ptr = NULL;
                size_t n = 0;
                expect_ok("secs_ii_item_ascii_view(tag default)",
                          secs_ii_item_ascii_view(tag_item, &ptr, &n));
                if (n != 7u || memcmp(ptr, "DEFAULT", 7u) != 0) {
                    fprintf(stderr, "FAIL: CEID reply tag mismatch (DEFAULT)\n");
                    ++g_failures;
                }
                secs_ii_item_destroy(tag_item);
                secs_ii_item_destroy(root);
            }

            secs_data_message_free(&reply);
            secs_free(req);
        }

        /* 3) request/reply CEID 校验：成功 */
        {
            uint8_t *req = NULL;
            size_t req_n = 0;
            expect_ok("build_ceid_request_body(100) for verify",
                      build_ceid_request_body(100, &req, &req_n));

            secs_data_message_t reply;
            memset(&reply, 0, sizeof(reply));
            int has_req = 0;
            uint32_t req_ceid = 0;
            int has_rep = 0;
            uint32_t rep_ceid = 0;

            expect_ok("secs_protocol_session_request_with_ceid_list_path(ok)",
                      secs_protocol_session_request_with_ceid_list_path(
                          client_proto,
                          6,
                          11,
                          req,
                          req_n,
                          1000,
                          ceid_path,
                          1,
                          NULL,
                          1,
                          &reply,
                          &has_req,
                          &req_ceid,
                          &has_rep,
                          &rep_ceid));

            if (!has_req || req_ceid != 100u || !has_rep || rep_ceid != 100u) {
                fprintf(stderr, "FAIL: CEID verify values mismatch\n");
                ++g_failures;
            }

            secs_data_message_free(&reply);
            secs_free(req);
        }

        /* 4) request/reply CEID 校验：不一致应返回错误，但仍带回 reply/ceid */
        {
            uint8_t *req = NULL;
            size_t req_n = 0;
            expect_ok("build_ceid_request_body(777) for mismatch",
                      build_ceid_request_body(777, &req, &req_n));

            secs_data_message_t reply;
            memset(&reply, 0, sizeof(reply));
            int has_req = 0;
            uint32_t req_ceid = 0;
            int has_rep = 0;
            uint32_t rep_ceid = 0;

            secs_error_t err = secs_protocol_session_request_with_ceid_list_path(
                client_proto,
                6,
                11,
                req,
                req_n,
                1000,
                ceid_path,
                1,
                NULL,
                1,
                &reply,
                &has_req,
                &req_ceid,
                &has_rep,
                &rep_ceid);
            expect_err("secs_protocol_session_request_with_ceid_list_path(mismatch)",
                       err);
            if (err.value != 4) {
                failf("ceid mismatch expected secs.core invalid_argument", err);
            }
            if (!has_req || req_ceid != 777u || !has_rep || rep_ceid != 778u) {
                fprintf(stderr, "FAIL: CEID mismatch outputs not populated\n");
                ++g_failures;
            }

            secs_data_message_free(&reply);
            secs_free(req);
        }

        /* 5) 覆盖 CEID 提取：U1/U2/U8 标量（extract_u32_scalar 分支） */
        {
            uint8_t *req = NULL;
            size_t req_n = 0;
            uint16_t ceid_u2 = 100;
            expect_ok("build_ceid_request_body_with_ceid_values(U2=100)",
                      build_ceid_request_body_with_ceid_values(
                          CEID_NUM_U2, &ceid_u2, 1, &req, &req_n));

            secs_data_message_t reply;
            memset(&reply, 0, sizeof(reply));
            int has_req = 0;
            uint32_t req_ceid = 0;
            int has_rep = 0;
            uint32_t rep_ceid = 0;

            expect_ok("secs_protocol_session_request_with_ceid_list_path(U2=100)",
                      secs_protocol_session_request_with_ceid_list_path(
                          client_proto,
                          6,
                          11,
                          req,
                          req_n,
                          1000,
                          ceid_path,
                          1,
                          NULL,
                          1,
                          &reply,
                          &has_req,
                          &req_ceid,
                          &has_rep,
                          &rep_ceid));
            if (!has_req || req_ceid != 100u || !has_rep || rep_ceid != 100u) {
                fprintf(stderr, "FAIL: CEID(U2) verify values mismatch\n");
                ++g_failures;
            }

            secs_data_message_free(&reply);
            secs_free(req);
        }
        {
            uint8_t *req = NULL;
            size_t req_n = 0;
            uint8_t ceid_u1 = 100;
            expect_ok("build_ceid_request_body_with_ceid_values(U1=100)",
                      build_ceid_request_body_with_ceid_values(
                          CEID_NUM_U1, &ceid_u1, 1, &req, &req_n));

            secs_data_message_t reply;
            memset(&reply, 0, sizeof(reply));
            int has_req = 0;
            uint32_t req_ceid = 0;
            int has_rep = 0;
            uint32_t rep_ceid = 0;

            expect_ok("secs_protocol_session_request_with_ceid_list_path(U1=100)",
                      secs_protocol_session_request_with_ceid_list_path(
                          client_proto,
                          6,
                          11,
                          req,
                          req_n,
                          1000,
                          ceid_path,
                          1,
                          NULL,
                          1,
                          &reply,
                          &has_req,
                          &req_ceid,
                          &has_rep,
                          &rep_ceid));
            if (!has_req || req_ceid != 100u || !has_rep || rep_ceid != 100u) {
                fprintf(stderr, "FAIL: CEID(U1) verify values mismatch\n");
                ++g_failures;
            }

            secs_data_message_free(&reply);
            secs_free(req);
        }
        {
            uint8_t *req = NULL;
            size_t req_n = 0;
            uint64_t ceid_u8 = 100;
            expect_ok("build_ceid_request_body_with_ceid_values(U8=100)",
                      build_ceid_request_body_with_ceid_values(
                          CEID_NUM_U8, &ceid_u8, 1, &req, &req_n));

            secs_data_message_t reply;
            memset(&reply, 0, sizeof(reply));
            int has_req = 0;
            uint32_t req_ceid = 0;
            int has_rep = 0;
            uint32_t rep_ceid = 0;

            expect_ok("secs_protocol_session_request_with_ceid_list_path(U8=100)",
                      secs_protocol_session_request_with_ceid_list_path(
                          client_proto,
                          6,
                          11,
                          req,
                          req_n,
                          1000,
                          ceid_path,
                          1,
                          NULL,
                          1,
                          &reply,
                          &has_req,
                          &req_ceid,
                          &has_rep,
                          &rep_ceid));
            if (!has_req || req_ceid != 100u || !has_rep || rep_ceid != 100u) {
                fprintf(stderr, "FAIL: CEID(U8) verify values mismatch\n");
                ++g_failures;
            }

            secs_data_message_free(&reply);
            secs_free(req);
        }

        /* 6) CEID 不是标量：应在发送前直接失败（verify_equal=1） */
        {
            uint8_t *req = NULL;
            size_t req_n = 0;
            const uint8_t ceid_u1s[2] = {1, 2};
            expect_ok("build_ceid_request_body_with_ceid_values(U1=[1,2])",
                      build_ceid_request_body_with_ceid_values(
                          CEID_NUM_U1, ceid_u1s, 2, &req, &req_n));

            secs_data_message_t reply;
            memset(&reply, 0, sizeof(reply));
            int has_req = 1;
            uint32_t req_ceid = 0;
            int has_rep = 1;
            uint32_t rep_ceid = 0;

            secs_error_t err = secs_protocol_session_request_with_ceid_list_path(
                client_proto,
                6,
                11,
                req,
                req_n,
                1000,
                ceid_path,
                1,
                NULL,
                1,
                &reply,
                &has_req,
                &req_ceid,
                &has_rep,
                &rep_ceid);
            expect_err("secs_protocol_session_request_with_ceid_list_path(U1 multi)",
                       err);
            if (err.value != 4) {
                failf("ceid multi expected secs.core invalid_argument", err);
            }
            if (reply.body != NULL || reply.body_n != 0) {
                fprintf(stderr,
                        "FAIL: CEID multi expected empty reply (not sent)\n");
                ++g_failures;
                secs_data_message_free(&reply);
            }
            if (has_req || has_rep) {
                fprintf(stderr, "FAIL: CEID multi expected has_req/has_rep==0\n");
                ++g_failures;
            }

            secs_free(req);
        }
        {
            uint8_t *req = NULL;
            size_t req_n = 0;
            const uint16_t ceid_u2s[2] = {100, 101};
            expect_ok("build_ceid_request_body_with_ceid_values(U2=[100,101])",
                      build_ceid_request_body_with_ceid_values(
                          CEID_NUM_U2, ceid_u2s, 2, &req, &req_n));

            secs_data_message_t reply;
            memset(&reply, 0, sizeof(reply));
            int has_req = 1;
            uint32_t req_ceid = 0;
            int has_rep = 1;
            uint32_t rep_ceid = 0;

            secs_error_t err = secs_protocol_session_request_with_ceid_list_path(
                client_proto,
                6,
                11,
                req,
                req_n,
                1000,
                ceid_path,
                1,
                NULL,
                1,
                &reply,
                &has_req,
                &req_ceid,
                &has_rep,
                &rep_ceid);
            expect_err("secs_protocol_session_request_with_ceid_list_path(U2 multi)",
                       err);
            if (err.value != 4) {
                failf("ceid multi expected secs.core invalid_argument", err);
            }
            if (reply.body != NULL || reply.body_n != 0) {
                fprintf(stderr,
                        "FAIL: CEID multi expected empty reply (not sent)\n");
                ++g_failures;
                secs_data_message_free(&reply);
            }
            if (has_req || has_rep) {
                fprintf(stderr, "FAIL: CEID multi expected has_req/has_rep==0\n");
                ++g_failures;
            }

            secs_free(req);
        }
        {
            uint8_t *req = NULL;
            size_t req_n = 0;
            const uint64_t too_big = (uint64_t)UINT32_MAX + 1ULL;
            expect_ok("build_ceid_request_body_with_ceid_values(U8 too big)",
                      build_ceid_request_body_with_ceid_values(
                          CEID_NUM_U8, &too_big, 1, &req, &req_n));

            secs_data_message_t reply;
            memset(&reply, 0, sizeof(reply));
            int has_req = 1;
            uint32_t req_ceid = 0;
            int has_rep = 1;
            uint32_t rep_ceid = 0;

            secs_error_t err = secs_protocol_session_request_with_ceid_list_path(
                client_proto,
                6,
                11,
                req,
                req_n,
                1000,
                ceid_path,
                1,
                NULL,
                1,
                &reply,
                &has_req,
                &req_ceid,
                &has_rep,
                &rep_ceid);
            expect_err("secs_protocol_session_request_with_ceid_list_path(U8 too big)",
                       err);
            if (err.value != 4) {
                failf("ceid too big expected secs.core invalid_argument", err);
            }
            if (reply.body != NULL || reply.body_n != 0) {
                fprintf(stderr,
                        "FAIL: CEID too big expected empty reply (not sent)\n");
                ++g_failures;
                secs_data_message_free(&reply);
            }
            if (has_req || has_rep) {
                fprintf(stderr,
                        "FAIL: CEID too big expected has_req/has_rep==0\n");
                ++g_failures;
            }

            secs_free(req);
        }

        expect_ok("secs_protocol_session_erase_handler(ceid dispatcher)",
                  secs_protocol_session_erase_handler(server_proto, 6, 11));
        expect_ok("secs_ceid_dispatcher_erase_handler(777)",
                  secs_ceid_dispatcher_erase_handler(cd, 777));
        expect_ok("secs_ceid_dispatcher_clear_default_handler",
                  secs_ceid_dispatcher_clear_default_handler(cd));
        secs_ceid_dispatcher_destroy(cd);
    }

    /* 反向验证：server 也可以主动发起 data primary（双方均可主动发送） */
    {
        expect_ok("secs_protocol_session_set_handler(client_echo)",
                  secs_protocol_session_set_handler(
                      client_proto, 2, 1, client_echo_handler, NULL));

        const uint8_t req_body[2] = {0xABu, 0xCDu};
        secs_data_message_t reply;
        memset(&reply, 0, sizeof(reply));

        expect_ok("secs_protocol_session_request(server->client)",
                  secs_protocol_session_request(server_proto,
                                                2,
                                                1,
                                                req_body,
                                                sizeof(req_body),
                                                1000,
                                                &reply));

        if (reply.stream != 2u || reply.function != 2u || reply.w_bit != 0) {
            fprintf(stderr, "FAIL: server->client reply header mismatch\n");
            ++g_failures;
        }
        if (reply.body_n != 3u || !reply.body || reply.body[0] != 0xABu ||
            reply.body[1] != 0xCDu || reply.body[2] != 0x99u) {
            fprintf(stderr, "FAIL: server->client reply body mismatch\n");
            ++g_failures;
        }

        secs_data_message_free(&reply);
        expect_ok("secs_protocol_session_erase_handler(client_echo)",
                  secs_protocol_session_erase_handler(client_proto, 2, 1));
    }

    /* default handler：未注册的 (stream,function) 也可被统一处理 */
    {
        expect_ok("secs_protocol_session_set_default_handler(server_default)",
                  secs_protocol_session_set_default_handler(
                      server_proto, client_echo_handler, NULL));

        const uint8_t req_body[1] = {0x55u};
        secs_data_message_t reply;
        memset(&reply, 0, sizeof(reply));

        expect_ok("secs_protocol_session_request(default handler)",
                  secs_protocol_session_request(
                      client_proto, 20, 1, req_body, sizeof(req_body), 1000, &reply));

        if (reply.stream != 20u || reply.function != 2u || reply.w_bit != 0) {
            fprintf(stderr, "FAIL: default handler reply header mismatch\n");
            ++g_failures;
        }
        if (reply.body_n != 2u || !reply.body || reply.body[0] != 0x55u ||
            reply.body[1] != 0x99u) {
            fprintf(stderr, "FAIL: default handler reply body mismatch\n");
            ++g_failures;
        }

        secs_data_message_free(&reply);
        expect_ok("secs_protocol_session_clear_default_handler(server_default)",
                  secs_protocol_session_clear_default_handler(server_proto));
    }

    /* stream default handler：SxF* fallback（对齐 C++ Router 行为：精确 > stream-default > default） */
    {
        uint8_t tag_exact = 0xE1u;
        uint8_t tag_stream = 0xE2u;
        uint8_t tag_default = 0xD3u;

        expect_ok("secs_protocol_session_set_default_handler(tag_default)",
                  secs_protocol_session_set_default_handler(
                      server_proto, protocol_append_tag_handler, &tag_default));
        expect_ok("secs_protocol_session_set_stream_default_handler(tag_stream)",
                  secs_protocol_session_set_stream_default_handler(
                      server_proto,
                      21,
                      protocol_append_tag_handler,
                      &tag_stream));
        expect_ok("secs_protocol_session_set_handler(tag_exact)",
                  secs_protocol_session_set_handler(
                      server_proto, 21, 1, protocol_append_tag_handler, &tag_exact));

        /* 精确匹配：S21F1 -> tag_exact */
        {
            const uint8_t req_body[1] = {0xAAu};
            secs_data_message_t reply;
            memset(&reply, 0, sizeof(reply));

            expect_ok("secs_protocol_session_request(stream-default exact)",
                      secs_protocol_session_request(
                          client_proto, 21, 1, req_body, sizeof(req_body), 1000, &reply));
            if (reply.stream != 21u || reply.function != 2u || reply.w_bit != 0) {
                fprintf(stderr, "FAIL: stream-default exact reply header mismatch\n");
                ++g_failures;
            }
            if (reply.body_n != 2u || !reply.body || reply.body[0] != 0xAAu ||
                reply.body[1] != tag_exact) {
                fprintf(stderr, "FAIL: stream-default exact reply body mismatch\n");
                ++g_failures;
            }
            secs_data_message_free(&reply);
        }

        /* stream-default：S21F3 -> tag_stream */
        {
            const uint8_t req_body[1] = {0xBBu};
            secs_data_message_t reply;
            memset(&reply, 0, sizeof(reply));

            expect_ok("secs_protocol_session_request(stream-default fallback)",
                      secs_protocol_session_request(
                          client_proto, 21, 3, req_body, sizeof(req_body), 1000, &reply));
            if (reply.stream != 21u || reply.function != 4u || reply.w_bit != 0) {
                fprintf(stderr, "FAIL: stream-default fallback reply header mismatch\n");
                ++g_failures;
            }
            if (reply.body_n != 2u || !reply.body || reply.body[0] != 0xBBu ||
                reply.body[1] != tag_stream) {
                fprintf(stderr, "FAIL: stream-default fallback reply body mismatch\n");
                ++g_failures;
            }
            secs_data_message_free(&reply);
        }

        /* default：S22F1 -> tag_default */
        {
            const uint8_t req_body[1] = {0xCCu};
            secs_data_message_t reply;
            memset(&reply, 0, sizeof(reply));

            expect_ok("secs_protocol_session_request(default fallback)",
                      secs_protocol_session_request(
                          client_proto, 22, 1, req_body, sizeof(req_body), 1000, &reply));
            if (reply.stream != 22u || reply.function != 2u || reply.w_bit != 0) {
                fprintf(stderr, "FAIL: default fallback reply header mismatch\n");
                ++g_failures;
            }
            if (reply.body_n != 2u || !reply.body || reply.body[0] != 0xCCu ||
                reply.body[1] != tag_default) {
                fprintf(stderr, "FAIL: default fallback reply body mismatch\n");
                ++g_failures;
            }
            secs_data_message_free(&reply);
        }

        /* clear stream-default：S21F3 应回退到 default */
        expect_ok("secs_protocol_session_clear_stream_default_handler",
                  secs_protocol_session_clear_stream_default_handler(server_proto, 21));
        {
            const uint8_t req_body[1] = {0xDDu};
            secs_data_message_t reply;
            memset(&reply, 0, sizeof(reply));

            expect_ok("secs_protocol_session_request(stream-default cleared)",
                      secs_protocol_session_request(
                          client_proto, 21, 3, req_body, sizeof(req_body), 1000, &reply));
            if (reply.body_n != 2u || !reply.body || reply.body[0] != 0xDDu ||
                reply.body[1] != tag_default) {
                fprintf(stderr,
                        "FAIL: stream-default cleared reply should fall back to default\n");
                ++g_failures;
            }
            secs_data_message_free(&reply);
        }

        expect_ok("secs_protocol_session_erase_handler(tag_exact)",
                  secs_protocol_session_erase_handler(server_proto, 21, 1));
        expect_ok("secs_protocol_session_clear_default_handler(tag_default)",
                  secs_protocol_session_clear_default_handler(server_proto));
    }

    /* SML default handler：用规则/模板批量定义回包，避免 C 侧写大量分发代码 */
    {
        secs_sml_runtime_t *rt = NULL;
        expect_ok("secs_sml_runtime_create(proto sml)", secs_sml_runtime_create(&rt));

        const char *sml = "s20f1: S20F1 W <L>.\n"
                          "s20f2: S20F2 <L <A \"OK\">>.\n"
                          "s21f1: S21F1 W <L>.\n"
                          "s21f2: S21F2 <L <A \"HELLO\">>.\n"
                          "if (s20f1) s20f2.\n"
                          "if (s21f1) s21f2.\n";
        expect_ok("secs_sml_runtime_load(proto sml)",
                  secs_sml_runtime_load(rt, sml, strlen(sml)));

        uint8_t *exp20 = NULL;
        size_t exp20_n = 0;
        expect_ok("secs_sml_runtime_get_message_body_by_name(s20f2)",
                  secs_sml_runtime_get_message_body_by_name(
                      rt, "s20f2", &exp20, &exp20_n, NULL, NULL, NULL));

        uint8_t *exp21 = NULL;
        size_t exp21_n = 0;
        expect_ok("secs_sml_runtime_get_message_body_by_name(s21f2)",
                  secs_sml_runtime_get_message_body_by_name(
                      rt, "s21f2", &exp21, &exp21_n, NULL, NULL, NULL));

        expect_ok("secs_protocol_session_set_sml_default_handler",
                  secs_protocol_session_set_sml_default_handler(server_proto, rt));

        /* set_sml_default_handler 内部应拷贝 runtime，C 侧可立即销毁 rt */
        secs_sml_runtime_destroy(rt);
        rt = NULL;

        secs_ii_item_t *req_item = NULL;
        expect_ok("secs_ii_item_create_list(sml req)",
                  secs_ii_item_create_list(&req_item));
        uint8_t *req_body = NULL;
        size_t req_body_n = 0;
        expect_ok("secs_ii_encode(sml req)",
                  secs_ii_encode(req_item, &req_body, &req_body_n));
        secs_ii_item_destroy(req_item);

        /* S20F1 -> S20F2 */
        {
            secs_data_message_t reply;
            memset(&reply, 0, sizeof(reply));
            expect_ok("secs_protocol_session_request(sml s20f1)",
                      secs_protocol_session_request(
                          client_proto, 20, 1, req_body, req_body_n, 1000, &reply));
            if (reply.stream != 20u || reply.function != 2u || reply.w_bit != 0) {
                fprintf(stderr, "FAIL: sml s20f1 reply header mismatch\n");
                ++g_failures;
            }
            if (reply.body_n != exp20_n || (exp20_n != 0u && !reply.body) ||
                (exp20_n != 0u && memcmp(reply.body, exp20, exp20_n) != 0)) {
                fprintf(stderr, "FAIL: sml s20f1 reply body mismatch\n");
                ++g_failures;
            }
            secs_data_message_free(&reply);
        }

        /* 兼容：部分设备/模拟器会发送“空 bytes”的 body（body_n=0） */
        {
            secs_data_message_t reply;
            memset(&reply, 0, sizeof(reply));
            expect_ok("secs_protocol_session_request(sml s20f1 empty bytes)",
                      secs_protocol_session_request(
                          client_proto, 20, 1, NULL, 0, 1000, &reply));
            if (reply.stream != 20u || reply.function != 2u || reply.w_bit != 0) {
                fprintf(stderr,
                        "FAIL: sml s20f1(empty bytes) reply header mismatch\n");
                ++g_failures;
            }
            if (reply.body_n != exp20_n || (exp20_n != 0u && !reply.body) ||
                (exp20_n != 0u && memcmp(reply.body, exp20, exp20_n) != 0)) {
                fprintf(stderr,
                        "FAIL: sml s20f1(empty bytes) reply body mismatch\n");
                ++g_failures;
            }
            secs_data_message_free(&reply);
        }

        /* S21F1 -> S21F2 */
        {
            secs_data_message_t reply;
            memset(&reply, 0, sizeof(reply));
            expect_ok("secs_protocol_session_request(sml s21f1)",
                      secs_protocol_session_request(
                          client_proto, 21, 1, req_body, req_body_n, 1000, &reply));
            if (reply.stream != 21u || reply.function != 2u || reply.w_bit != 0) {
                fprintf(stderr, "FAIL: sml s21f1 reply header mismatch\n");
                ++g_failures;
            }
            if (reply.body_n != exp21_n || (exp21_n != 0u && !reply.body) ||
                (exp21_n != 0u && memcmp(reply.body, exp21, exp21_n) != 0)) {
                fprintf(stderr, "FAIL: sml s21f1 reply body mismatch\n");
                ++g_failures;
            }
            secs_data_message_free(&reply);
        }

        /* 未命中规则：应不回包，客户端超时返回错误 */
        {
            secs_data_message_t reply;
            memset(&reply, 0, sizeof(reply));
            expect_err("secs_protocol_session_request(sml no match)",
                       secs_protocol_session_request(
                           client_proto, 22, 1, req_body, req_body_n, 200, &reply));
            secs_data_message_free(&reply);
        }

        secs_free(req_body);
        secs_free(exp20);
        secs_free(exp21);
        expect_ok("secs_protocol_session_clear_default_handler(sml)",
                  secs_protocol_session_clear_default_handler(server_proto));
    }

    /* SML stream default handler：仅对指定 stream 生效 */
    {
        secs_sml_runtime_t *rt = NULL;
        expect_ok("secs_sml_runtime_create(proto sml stream)",
                  secs_sml_runtime_create(&rt));

        const char *sml = "s30f1: S30F1 W <L>.\n"
                          "s30f2: S30F2 <L <A \"OK\">>.\n"
                          "if (s30f1) s30f2.\n";
        expect_ok("secs_sml_runtime_load_cstr(proto sml stream)",
                  secs_sml_runtime_load_cstr(rt, sml));

        uint8_t *exp = NULL;
        size_t exp_n = 0;
        expect_ok("secs_sml_runtime_get_message_body_by_name(s30f2)",
                  secs_sml_runtime_get_message_body_by_name(
                      rt, "s30f2", &exp, &exp_n, NULL, NULL, NULL));

        expect_ok("secs_protocol_session_set_sml_stream_default_handler",
                  secs_protocol_session_set_sml_stream_default_handler(
                      server_proto, 30, rt));
        secs_sml_runtime_destroy(rt);
        rt = NULL;

        /* request body：空 List（推荐发送有效 SECS-II 编码；兼容模式下空 bytes 也会视为 <L[0]>） */
        secs_ii_item_t *req_item = NULL;
        expect_ok("secs_ii_item_create_list(sml stream req)",
                  secs_ii_item_create_list(&req_item));
        uint8_t *req_body = NULL;
        size_t req_body_n = 0;
        expect_ok("secs_ii_encode(sml stream req)",
                  secs_ii_encode(req_item, &req_body, &req_body_n));
        secs_ii_item_destroy(req_item);

        /* S30F1 -> S30F2（命中 stream default） */
        {
            secs_data_message_t reply;
            memset(&reply, 0, sizeof(reply));
            expect_ok("secs_protocol_session_request(sml stream default)",
                      secs_protocol_session_request(
                          client_proto, 30, 1, req_body, req_body_n, 1000, &reply));
            if (reply.stream != 30u || reply.function != 2u || reply.w_bit != 0) {
                fprintf(stderr, "FAIL: sml stream default reply header mismatch\n");
                ++g_failures;
            }
            if (reply.body_n != exp_n || (exp_n != 0u && !reply.body) ||
                (exp_n != 0u && memcmp(reply.body, exp, exp_n) != 0)) {
                fprintf(stderr, "FAIL: sml stream default reply body mismatch\n");
                ++g_failures;
            }
            secs_data_message_free(&reply);
        }

        /* 其它 stream：不应命中，客户端超时返回错误 */
        {
            secs_data_message_t reply;
            memset(&reply, 0, sizeof(reply));
            expect_err("secs_protocol_session_request(sml stream no match)",
                       secs_protocol_session_request(
                           client_proto, 31, 1, req_body, req_body_n, 200, &reply));
            secs_data_message_free(&reply);
        }

        secs_free(req_body);
        secs_free(exp);
        expect_ok("secs_protocol_session_clear_stream_default_handler(sml stream)",
                  secs_protocol_session_clear_stream_default_handler(server_proto, 30));
    }

    /* decoded handler：框架自动 decode/encode（贴近 C++ TypedHandler） */
    {
        /* request body：<U2 41> */
        const uint16_t v = 41u;
        secs_ii_item_t *u2 = NULL;
        expect_ok("secs_ii_item_create_u2(decoded req)",
                  secs_ii_item_create_u2(&v, 1, &u2));

        uint8_t *body = NULL;
        size_t body_n = 0;
        expect_ok("secs_ii_encode(decoded req)", secs_ii_encode(u2, &body, &body_n));
        secs_ii_item_destroy(u2);

        /* 带尾随 bytes：用于 strict_consumed 分支 */
        uint8_t *body_tail = (uint8_t *)secs_malloc(body_n + 1u);
        if (!body_tail) {
            fprintf(stderr, "FAIL: secs_malloc(body_tail) out of memory\n");
            ++g_failures;
        } else {
            memcpy(body_tail, body, body_n);
            body_tail[body_n] = 0xFFu;
        }

        /* strict_consumed=1：正常应答 + 尾随 bytes 应超时 */
        expect_ok("secs_protocol_session_set_decoded_handler(strict)",
                  secs_protocol_session_set_decoded_handler(
                      server_proto,
                      40,
                      1,
                      NULL,
                      1,
                      protocol_decoded_add_one_u2_handler,
                      NULL));
        {
            secs_data_message_t reply;
            memset(&reply, 0, sizeof(reply));
            expect_ok("secs_protocol_session_request(decoded strict ok)",
                      secs_protocol_session_request(
                          client_proto, 40, 1, body, body_n, 1000, &reply));

            size_t consumed = 0;
            secs_ii_item_t *decoded = NULL;
            expect_ok("secs_ii_decode_one(decoded reply)",
                      secs_ii_decode_one(reply.body,
                                         reply.body_n,
                                         &consumed,
                                         &decoded));
            if (consumed != reply.body_n) {
                fprintf(stderr, "FAIL: decoded reply consumed mismatch\n");
                ++g_failures;
            }
            const uint16_t *p = NULL;
            size_t n = 0;
            expect_ok("secs_ii_item_u2_view(decoded reply)",
                      secs_ii_item_u2_view(decoded, &p, &n));
            if (!p || n != 1u || p[0] != 42u) {
                fprintf(stderr, "FAIL: decoded strict reply value mismatch\n");
                ++g_failures;
            }
            secs_ii_item_destroy(decoded);
            secs_data_message_free(&reply);
        }
        if (body_tail) {
            secs_data_message_t reply;
            memset(&reply, 0, sizeof(reply));
            expect_err("secs_protocol_session_request(decoded strict tail -> timeout)",
                       secs_protocol_session_request(
                           client_proto, 40, 1, body_tail, body_n + 1u, 200, &reply));
            secs_data_message_free(&reply);
        }
        expect_ok("secs_protocol_session_erase_handler(decoded strict)",
                  secs_protocol_session_erase_handler(server_proto, 40, 1));

        /* strict_consumed=0：允许尾随 bytes */
        expect_ok("secs_protocol_session_set_decoded_handler(nonstrict)",
                  secs_protocol_session_set_decoded_handler(
                      server_proto,
                      41,
                      1,
                      NULL,
                      0,
                      protocol_decoded_add_one_u2_handler,
                      NULL));
        if (body_tail) {
            secs_data_message_t reply;
            memset(&reply, 0, sizeof(reply));
            expect_ok("secs_protocol_session_request(decoded nonstrict tail ok)",
                      secs_protocol_session_request(
                          client_proto, 41, 1, body_tail, body_n + 1u, 1000, &reply));

            size_t consumed = 0;
            secs_ii_item_t *decoded = NULL;
            expect_ok("secs_ii_decode_one(decoded nonstrict reply)",
                      secs_ii_decode_one(reply.body,
                                         reply.body_n,
                                         &consumed,
                                         &decoded));
            const uint16_t *p = NULL;
            size_t n = 0;
            expect_ok("secs_ii_item_u2_view(decoded nonstrict reply)",
                      secs_ii_item_u2_view(decoded, &p, &n));
            if (!p || n != 1u || p[0] != 42u) {
                fprintf(stderr, "FAIL: decoded nonstrict reply value mismatch\n");
                ++g_failures;
            }
            secs_ii_item_destroy(decoded);
            secs_data_message_free(&reply);
        }
        expect_ok("secs_protocol_session_erase_handler(decoded nonstrict)",
                  secs_protocol_session_erase_handler(server_proto, 41, 1));

        /* decoded stream default：S42F* */
        expect_ok("secs_protocol_session_set_decoded_stream_default_handler",
                  secs_protocol_session_set_decoded_stream_default_handler(
                      server_proto,
                      42,
                      NULL,
                      1,
                      protocol_decoded_add_one_u2_handler,
                      NULL));
        {
            secs_data_message_t reply;
            memset(&reply, 0, sizeof(reply));
            expect_ok("secs_protocol_session_request(decoded stream default)",
                      secs_protocol_session_request(
                          client_proto, 42, 1, body, body_n, 1000, &reply));
            {
                size_t consumed = 0;
                secs_ii_item_t *decoded = NULL;
                expect_ok("secs_ii_decode_one(decoded stream default reply)",
                          secs_ii_decode_one(reply.body,
                                             reply.body_n,
                                             &consumed,
                                             &decoded));
                const uint16_t *p = NULL;
                size_t n = 0;
                expect_ok("secs_ii_item_u2_view(decoded stream default reply)",
                          secs_ii_item_u2_view(decoded, &p, &n));
                if (!p || n != 1u || p[0] != 42u) {
                    fprintf(stderr,
                            "FAIL: decoded stream default reply value mismatch\n");
                    ++g_failures;
                }
                secs_ii_item_destroy(decoded);
            }
            secs_data_message_free(&reply);
        }
        expect_ok("secs_protocol_session_clear_stream_default_handler(decoded stream default)",
                  secs_protocol_session_clear_stream_default_handler(server_proto, 42));

        /* decoded default：全局兜底 */
        expect_ok("secs_protocol_session_set_decoded_default_handler",
                  secs_protocol_session_set_decoded_default_handler(
                      server_proto,
                      NULL,
                      1,
                      protocol_decoded_add_one_u2_handler,
                      NULL));
        {
            secs_data_message_t reply;
            memset(&reply, 0, sizeof(reply));
            expect_ok("secs_protocol_session_request(decoded default)",
                      secs_protocol_session_request(
                          client_proto, 43, 1, body, body_n, 1000, &reply));
            {
                size_t consumed = 0;
                secs_ii_item_t *decoded = NULL;
                expect_ok("secs_ii_decode_one(decoded default reply)",
                          secs_ii_decode_one(reply.body,
                                             reply.body_n,
                                             &consumed,
                                             &decoded));
                const uint16_t *p = NULL;
                size_t n = 0;
                expect_ok("secs_ii_item_u2_view(decoded default reply)",
                          secs_ii_item_u2_view(decoded, &p, &n));
                if (!p || n != 1u || p[0] != 42u) {
                    fprintf(stderr, "FAIL: decoded default reply value mismatch\n");
                    ++g_failures;
                }
                secs_ii_item_destroy(decoded);
            }
            secs_data_message_free(&reply);
        }
        expect_ok("secs_protocol_session_clear_default_handler(decoded default)",
                  secs_protocol_session_clear_default_handler(server_proto));

        secs_free(body);
        if (body_tail) {
            secs_free(body_tail);
        }
    }

    /* 通过 protocol handler 在 io 线程内误用 HSMS 阻塞式 API：必须返回 WRONG_THREAD */
    {
        struct hsms_wrong_thread_ud hud;
        hud.server_hsms = server_hsms;

        expect_ok("secs_protocol_session_set_handler(hsms_wrong_thread)",
                  secs_protocol_session_set_handler(
                      server_proto, 10, 11, server_handler_hsms_wrong_thread, &hud));

        secs_data_message_t reply;
        memset(&reply, 0, sizeof(reply));
        expect_ok("secs_protocol_session_request(hsms wrong thread)",
                  secs_protocol_session_request(
                      client_proto, 10, 11, NULL, 0, 1000, &reply));

        if (reply.body_n != 6u || !reply.body) {
            fprintf(stderr, "FAIL: hsms wrong-thread reply body invalid\n");
            ++g_failures;
        } else {
            for (size_t i = 0; i < reply.body_n; ++i) {
                if (reply.body[i] != 1u) {
                    fprintf(stderr,
                            "FAIL: hsms wrong-thread flag[%zu] expected 1 got "
                            "%u\n",
                            i,
                            (unsigned)reply.body[i]);
                    ++g_failures;
                }
            }
        }
        secs_data_message_free(&reply);

        expect_ok("secs_protocol_session_erase_handler(hsms_wrong_thread)",
                  secs_protocol_session_erase_handler(server_proto, 10, 11));
    }

    /* handler 返回空 body：覆盖 fill_protocol_out_message 的 empty-body 分支 +
     * timeout_ms==0 分支 */
    {
        expect_ok("secs_protocol_session_set_handler(empty)",
                  secs_protocol_session_set_handler(
                      server_proto, 6, 1, server_handler_empty, NULL));

        secs_data_message_t reply;
        memset(&reply, 0, sizeof(reply));
        expect_ok("secs_protocol_session_request(timeout=0, empty body)",
                  secs_protocol_session_request(
                      client_proto, 6, 1, NULL, 0, 0, &reply));

        if (reply.stream != 6u || reply.function != 2u || reply.w_bit != 0) {
            fprintf(stderr, "FAIL: empty reply meta mismatch\n");
            ++g_failures;
        }
        if (reply.body_n != 0u || reply.body != NULL) {
            fprintf(stderr,
                    "FAIL: empty reply should return body=NULL, body_n=0\n");
            ++g_failures;
        }
        secs_data_message_free(&reply);
    }

    /* handler 异常路径：返回错误/返回不一致 out_body/out_n ->
     * 不应回包，客户端应超时 */
    {
        expect_ok(
            "secs_protocol_session_set_handler(bad1)",
            secs_protocol_session_set_handler(
                server_proto, 3, 3, protocol_bad_handler_returns_error, NULL));
        expect_ok("secs_protocol_session_set_handler(bad2)",
                  secs_protocol_session_set_handler(
                      server_proto,
                      4,
                      5,
                      protocol_bad_handler_body_null_nonzero,
                      NULL));

        secs_data_message_t reply;
        memset(&reply, 0, sizeof(reply));
        expect_err("secs_protocol_session_request(timeout bad1)",
                   secs_protocol_session_request(
                       client_proto, 3, 3, NULL, 0, 200, &reply));
        secs_data_message_free(&reply);

        memset(&reply, 0, sizeof(reply));
        expect_err("secs_protocol_session_request(timeout bad2)",
                   secs_protocol_session_request(
                       client_proto, 4, 5, NULL, 0, 200, &reply));
        secs_data_message_free(&reply);
    }

    /* set_handler 参数校验：cb==NULL */
    {
        secs_error_t err =
            secs_protocol_session_set_handler(server_proto, 9, 9, NULL, NULL);
        expect_err("secs_protocol_session_set_handler(NULL)", err);
        if (err.value != (int)SECS_C_API_INVALID_ARGUMENT) {
            fprintf(stderr,
                    "FAIL: set_handler(NULL) should be INVALID_ARGUMENT\n");
            ++g_failures;
        }
    }

    /* 在 io 线程内调用 stop：覆盖 c_api.cpp 的 is_io_thread 分支 */
    {
        atomic_int called;
        atomic_init(&called, 0);

        struct control_stop_ud sud;
        sud.server_proto = server_proto;
        sud.server_hsms = server_hsms;
        sud.called = &called;

        expect_ok("secs_protocol_session_set_handler(control_stop)",
                  secs_protocol_session_set_handler(server_proto,
                                                    7,
                                                    7,
                                                    protocol_control_stop_handler,
                                                    &sud));
        expect_ok("secs_protocol_session_send(control_stop)",
                  secs_protocol_session_send(client_proto, 7, 7, NULL, 0));

        if (!wait_until_atomic_eq(&called, 1, 200, 5 * 1000 * 1000)) {
            fprintf(stderr,
                    "FAIL: control_stop handler not called within timeout\n");
            ++g_failures;
        }
    }

    (void)secs_protocol_session_stop(client_proto);
    (void)secs_protocol_session_stop(server_proto);
    secs_protocol_session_destroy(client_proto);
    secs_protocol_session_destroy(server_proto);

    (void)secs_hsms_session_stop(client_hsms);
    (void)secs_hsms_session_stop(server_hsms);
    secs_hsms_session_destroy(client_hsms);
    secs_hsms_session_destroy(server_hsms);

    secs_context_destroy(ctx);
}

static secs_error_t
secs1_simple_reply_handler(void *user_data,
                           const secs_data_message_view_t *request,
                           uint8_t **out_body,
                           size_t *out_body_n) {
    (void)user_data;
    (void)request;

    if (!out_body || !out_body_n) {
        secs_error_t err;
        err.value = (int)SECS_C_API_INVALID_ARGUMENT;
        err.category = "secs.c_api";
        return err;
    }

    *out_body_n = 2;
    *out_body = (uint8_t *)secs_malloc(*out_body_n);
    if (!*out_body) {
        secs_error_t err;
        err.value = (int)SECS_C_API_OUT_OF_MEMORY;
        err.category = "secs.c_api";
        return err;
    }

    (*out_body)[0] = 0xABu;
    (*out_body)[1] = 0xCDu;

    secs_error_t ok;
    ok.value = 0;
    ok.category = "secs.c_api";
    return ok;
}

struct secs1_poll_args {
    secs_protocol_session_t *sess;
    atomic_int *stop;
    atomic_int *handled_cnt;
    atomic_int *poll_errors;
};

static void *secs1_poll_loop_thread(void *p) {
    struct secs1_poll_args *args = (struct secs1_poll_args *)p;
    if (!args || !args->sess || !args->stop || !args->handled_cnt ||
        !args->poll_errors) {
        return NULL;
    }

    while (atomic_load(args->stop) == 0) {
        int handled = 0;
        secs_error_t err = secs_protocol_session_poll_once(args->sess, 10, &handled);
        if (err.value != 0) {
            (void)atomic_fetch_add(args->poll_errors, 1);
            break;
        }
        if (handled) {
            (void)atomic_fetch_add(args->handled_cnt, 1);
        }
    }
    return NULL;
}

static void test_protocol_session_secs1_memory_duplex(void) {
    secs_context_t *ctx = NULL;
    expect_ok("secs_context_create(secs1 duplex)", secs_context_create(&ctx));

    secs_protocol_session_options_t opt;
    memset(&opt, 0, sizeof(opt));
    opt.t3_ms = 200;
    opt.poll_interval_ms = 1;

    secs_protocol_session_t *host = NULL;
    secs_protocol_session_t *equip = NULL;
    expect_ok("secs_protocol_session_create_from_secs1_memory_duplex",
              secs_protocol_session_create_from_secs1_memory_duplex(
                  ctx, 0x0101, &opt, &host, &equip));

    /* poll_once：无消息时应按 timeout 返回 handled=0 且 err=OK */
    {
        int handled = 1;
        expect_ok("secs_protocol_session_poll_once(timeout, no msg)",
                  secs_protocol_session_poll_once(equip, 5, &handled));
        if (handled != 0) {
            fprintf(stderr, "FAIL: secs1 poll_once(no msg) should handled=0\n");
            ++g_failures;
        }
    }

    expect_ok("secs_protocol_session_set_handler(secs1 equip)",
              secs_protocol_session_set_handler(
                  equip, 1, 1, secs1_simple_reply_handler, NULL));

    atomic_int stop;
    atomic_int handled_cnt;
    atomic_int poll_errors;
    atomic_init(&stop, 0);
    atomic_init(&handled_cnt, 0);
    atomic_init(&poll_errors, 0);

    struct secs1_poll_args args;
    memset(&args, 0, sizeof(args));
    args.sess = equip;
    args.stop = &stop;
    args.handled_cnt = &handled_cnt;
    args.poll_errors = &poll_errors;

    pthread_t th;
    if (pthread_create(&th, NULL, secs1_poll_loop_thread, &args) != 0) {
        fprintf(stderr, "FAIL: pthread_create(secs1 poll loop)\n");
        ++g_failures;
    }

    {
        const uint8_t body[1] = {0x01u};
        secs_data_message_t reply;
        memset(&reply, 0, sizeof(reply));

        expect_ok("secs_protocol_session_request(secs1 host->equip)",
                  secs_protocol_session_request(
                      host, 1, 1, body, sizeof(body), 1000, &reply));

        if (reply.stream != 1u || reply.function != 2u || reply.w_bit != 0) {
            fprintf(stderr, "FAIL: secs1 reply meta mismatch\n");
            ++g_failures;
        }
        if (reply.body_n != 2u || !reply.body || reply.body[0] != 0xABu ||
            reply.body[1] != 0xCDu) {
            fprintf(stderr, "FAIL: secs1 reply body mismatch\n");
            ++g_failures;
        }
        secs_data_message_free(&reply);
    }

    atomic_store(&stop, 1);
    (void)pthread_join(th, NULL);

    if (atomic_load(&poll_errors) != 0) {
        fprintf(stderr, "FAIL: secs1 poll loop got error\n");
        ++g_failures;
    }
    if (atomic_load(&handled_cnt) <= 0) {
        fprintf(stderr, "FAIL: secs1 poll loop handled_cnt should > 0\n");
        ++g_failures;
    }

    (void)secs_protocol_session_stop(host);
    (void)secs_protocol_session_stop(equip);
    secs_protocol_session_destroy(host);
    secs_protocol_session_destroy(equip);
    secs_context_destroy(ctx);
}

static void test_protocol_session_secs1_memory_duplex_v2(void) {
    secs_context_t *ctx = NULL;
    expect_ok("secs_context_create(secs1 duplex v2)", secs_context_create(&ctx));

    secs_protocol_session_options_v2_t opt;
    memset(&opt, 0, sizeof(opt));
    opt.t3_ms = 200;
    opt.poll_interval_ms = 1;
    opt.max_pending_requests = 8;

    secs_protocol_session_t *host = NULL;
    secs_protocol_session_t *equip = NULL;
    expect_ok("secs_protocol_session_create_from_secs1_memory_duplex_v2",
              secs_protocol_session_create_from_secs1_memory_duplex_v2(
                  ctx, 0x0101, &opt, &host, &equip));

    expect_ok("secs_protocol_session_set_handler(secs1 equip v2)",
              secs_protocol_session_set_handler(
                  equip, 1, 1, secs1_simple_reply_handler, NULL));

    atomic_int stop;
    atomic_int handled_cnt;
    atomic_int poll_errors;
    atomic_init(&stop, 0);
    atomic_init(&handled_cnt, 0);
    atomic_init(&poll_errors, 0);

    struct secs1_poll_args args;
    memset(&args, 0, sizeof(args));
    args.sess = equip;
    args.stop = &stop;
    args.handled_cnt = &handled_cnt;
    args.poll_errors = &poll_errors;

    pthread_t th;
    if (pthread_create(&th, NULL, secs1_poll_loop_thread, &args) != 0) {
        fprintf(stderr, "FAIL: pthread_create(secs1 poll loop v2)\n");
        ++g_failures;
    }

    {
        const uint8_t body[1] = {0x02u};
        secs_data_message_t reply;
        memset(&reply, 0, sizeof(reply));
        expect_ok("secs_protocol_session_request(secs1 v2 host->equip)",
                  secs_protocol_session_request(
                      host, 1, 1, body, sizeof(body), 1000, &reply));
        secs_data_message_free(&reply);
    }

    atomic_store(&stop, 1);
    (void)pthread_join(th, NULL);

    if (atomic_load(&poll_errors) != 0) {
        fprintf(stderr, "FAIL: secs1 v2 poll loop got error\n");
        ++g_failures;
    }
    if (atomic_load(&handled_cnt) <= 0) {
        fprintf(stderr, "FAIL: secs1 v2 poll loop handled_cnt should > 0\n");
        ++g_failures;
    }

    (void)secs_protocol_session_stop(host);
    (void)secs_protocol_session_stop(equip);
    secs_protocol_session_destroy(host);
    secs_protocol_session_destroy(equip);
    secs_context_destroy(ctx);
}

static void test_protocol_session_secs1_serial_pty_smoke(void) {
#if !defined(__unix__) && !defined(__APPLE__)
    return;
#else
    secs_context_t *ctx = NULL;
    expect_ok("secs_context_create(secs1 serial)", secs_context_create(&ctx));

    /* 即便 PTY 不可用，也至少覆盖 SerialPortLink::open 的失败分支（bad path）。 */
    {
        const char *bad_path = "/dev/secs_lib_no_such_tty";

        secs_protocol_session_options_t opt;
        memset(&opt, 0, sizeof(opt));
        opt.t3_ms = 200;
        opt.poll_interval_ms = 1;

        secs_protocol_session_t *sess = NULL;
        expect_err("secs_protocol_session_create_from_secs1_serial(bad path)",
                   secs_protocol_session_create_from_secs1_serial(
                       ctx, bad_path, 0, 0x0101, 0, &opt, &sess));
        secs_protocol_session_destroy(sess);
    }
    {
        const char *bad_path = "/dev/secs_lib_no_such_tty";

        secs_protocol_session_options_v2_t opt;
        memset(&opt, 0, sizeof(opt));
        opt.t3_ms = 200;
        opt.poll_interval_ms = 1;
        opt.max_pending_requests = 8;

        secs_protocol_session_t *sess = NULL;
        expect_err("secs_protocol_session_create_from_secs1_serial_v2(bad path)",
                   secs_protocol_session_create_from_secs1_serial_v2(
                       ctx, bad_path, 0, 0x0101, 0, &opt, &sess));
        secs_protocol_session_destroy(sess);
    }

    /* 使用普通文件触发 set_option 失败：覆盖 SerialPortLink::open 的 close_on_error 分支 */
    {
        char path[] = "/tmp/secs_lib_serialXXXXXX";
        const int fd = mkstemp(path);
        if (fd >= 0) {
            (void)close(fd);

            secs_protocol_session_options_t opt;
            memset(&opt, 0, sizeof(opt));
            opt.t3_ms = 200;
            opt.poll_interval_ms = 1;

            secs_protocol_session_t *sess = NULL;
            expect_err("secs_protocol_session_create_from_secs1_serial(tmpfile)",
                       secs_protocol_session_create_from_secs1_serial(
                           ctx, path, 0, 0x0101, 0, &opt, &sess));
            secs_protocol_session_destroy(sess);

            (void)unlink(path);
        }
    }
    {
        char path[] = "/tmp/secs_lib_serialXXXXXX";
        const int fd = mkstemp(path);
        if (fd >= 0) {
            (void)close(fd);

            secs_protocol_session_options_v2_t opt;
            memset(&opt, 0, sizeof(opt));
            opt.t3_ms = 200;
            opt.poll_interval_ms = 1;
            opt.max_pending_requests = 8;

            secs_protocol_session_t *sess = NULL;
            expect_err("secs_protocol_session_create_from_secs1_serial_v2(tmpfile)",
                       secs_protocol_session_create_from_secs1_serial_v2(
                           ctx, path, 0, 0x0101, 0, &opt, &sess));
            secs_protocol_session_destroy(sess);

            (void)unlink(path);
        }
    }

    struct pty_pair p;
    const int has_pty = create_pty_pair(&p);

    /* v1：覆盖 make_proto_options(non-null) + SerialPortLink::open */
    {
        secs_protocol_session_options_t opt;
        memset(&opt, 0, sizeof(opt));
        opt.t3_ms = 200;
        opt.poll_interval_ms = 1;

        secs_protocol_session_t *sess = NULL;
        if (has_pty) {
            expect_ok("secs_protocol_session_create_from_secs1_serial(pty)",
                      secs_protocol_session_create_from_secs1_serial(
                          ctx, p.slave_path, 0, 0x0101, 0, &opt, &sess));

            int handled = 1;
            expect_ok("secs_protocol_session_poll_once(secs1 serial timeout)",
                      secs_protocol_session_poll_once(sess, 5, &handled));
            if (handled != 0) {
                fprintf(stderr,
                        "FAIL: secs1 serial poll_once(timeout) should handled=0\n");
                ++g_failures;
            }

            (void)secs_protocol_session_stop(sess);
            secs_protocol_session_destroy(sess);
        }
    }

    /* v2：覆盖 create_from_secs1_serial_v2 分支（含 max_pending_requests 赋值） */
    {
        secs_protocol_session_options_v2_t opt;
        memset(&opt, 0, sizeof(opt));
        opt.t3_ms = 200;
        opt.poll_interval_ms = 1;
        opt.max_pending_requests = 8;

        secs_protocol_session_t *sess = NULL;
        if (has_pty) {
            expect_ok("secs_protocol_session_create_from_secs1_serial_v2(pty)",
                      secs_protocol_session_create_from_secs1_serial_v2(
                          ctx, p.slave_path, 0, 0x0101, 0, &opt, &sess));

            int handled = 1;
            expect_ok("secs_protocol_session_poll_once(secs1 serial v2 timeout)",
                      secs_protocol_session_poll_once(sess, 5, &handled));
            if (handled != 0) {
                fprintf(stderr,
                        "FAIL: secs1 serial v2 poll_once(timeout) should handled=0\n");
                ++g_failures;
            }

            (void)secs_protocol_session_stop(sess);
            secs_protocol_session_destroy(sess);
        }
    }

    secs_context_destroy(ctx);
    if (has_pty) {
        destroy_pty_pair(&p);
    }
#endif
}

int main(void) {
    test_version_and_error_message();
    test_error_message_category_mapping();
    test_log_set_level_smoke();
    test_context_create_with_options_smoke();
    test_invalid_argument_fast_fail();
    test_hsms_session_create_v2_smoke();
    test_ii_encode_decode_and_malicious();
    test_ii_all_types_and_views();
    test_ii_list_builder_helpers();
    test_ii_builder();
    test_ii_extraction_helpers();
    test_ii_builder_add_helpers();
    test_ii_more_view_get_at_path_and_list_path();
    test_ii_clone_and_list_path_array_helpers();
    test_sml_runtime_basic();
    test_sml_runtime_placeholders();
    test_sml_render_context_lifecycle();
    test_sml_runtime_encode_message_body_with_context();
    test_sml_runtime_match_response_with_context();
    test_sml_runtime_match_response_empty_body();
    test_sml_runtime_match_response_with_capture();
    test_sml_runtime_match_response_with_trace();
    test_sml_runtime_match_response_with_trace_empty_rules();
    test_hsms_open_passive_ip_invalid_cases();
    test_hsms_open_ip_smoke();
    test_protocol_session_secs1_memory_duplex();
    test_protocol_session_secs1_memory_duplex_v2();
    test_protocol_session_secs1_serial_pty_smoke();
    test_hsms_protocol_loopback();

    if (g_failures == 0) {
        return 0;
    }
    fprintf(stderr, "FAILED: %d assertions\n", g_failures);
    return 1;
}
