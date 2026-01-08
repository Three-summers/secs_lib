/*
 * @file test_c_api.c
 * @brief C ABI（secs/c_api.h）最小可用性与健壮性测试（包含“恶意输入”用例）。
 *
 * 说明：
 * - 本文件用 C 编译器编译，确保头文件对 C 语言可用；
 * - 但链接阶段必须使用 C++ 链接器（底层实现为 C++20）。
 */

#include "secs/c_api.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

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
    }

    /* SML 参数校验 */
    {
        secs_sml_runtime_t *rt = NULL;
        expect_ok("secs_sml_runtime_create(valid)",
                  secs_sml_runtime_create(&rt));
        expect_err("secs_sml_runtime_load(NULL)",
                   secs_sml_runtime_load(NULL, "x", 1));
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
        expect_err(
            "secs_protocol_session_set_sml_default_handler(NULL)",
            secs_protocol_session_set_sml_default_handler(NULL, NULL));
        expect_err("secs_protocol_session_send(NULL)",
                   secs_protocol_session_send(NULL, 1, 1, NULL, 0));
        expect_err("secs_protocol_session_request(NULL)",
                   secs_protocol_session_request(NULL, 1, 1, NULL, 0, 1, NULL));
        secs_protocol_session_destroy(NULL);
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

static void test_sml_runtime_basic(void) {
    secs_sml_runtime_t *rt = NULL;
    expect_ok("secs_sml_runtime_create", secs_sml_runtime_create(&rt));

    const char *sml = "s1f1: S1F1 W <L>.\n"
                      "s1f2: S1F2 <L <A \"Hello\">>.\n"
                      "if (s1f1) s1f2.\n";
    expect_ok("secs_sml_runtime_load",
              secs_sml_runtime_load(rt, sml, strlen(sml)));

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

int main(void) {
    test_version_and_error_message();
    test_error_message_category_mapping();
    test_log_set_level_smoke();
    test_invalid_argument_fast_fail();
    test_hsms_session_create_v2_smoke();
    test_ii_encode_decode_and_malicious();
    test_ii_all_types_and_views();
    test_sml_runtime_basic();
    test_hsms_open_passive_ip_invalid_cases();
    test_hsms_protocol_loopback();

    if (g_failures == 0) {
        return 0;
    }
    fprintf(stderr, "FAILED: %d assertions\n", g_failures);
    return 1;
}
