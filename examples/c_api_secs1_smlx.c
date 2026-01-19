/*
 * @file c_api_secs1_smlx.c
 * @brief C API 示例：SECS-I（串口）+ SMLX（Rule-Based）
 *
 * 说明：
 * - 与 C++ 示例 `secs1_smlx` 保持“功能对应”；
 * - 读取 `ceid_demo.sml`：
 *   - server：match_response_with_capture + 注入变量 + encode_message_body 自动回包
 *   - client：用 req_* 模板渲染请求 body，并发送 request
 *
 * 角色：
 * - server：Equipment 端（默认 reverse_bit=1）
 * - client：Host 端（默认 reverse_bit=0）
 * - loopback：使用 secs_protocol_session_create_from_secs1_memory_duplex 在同进程跑通端到端
 *
 * 用法：
 *   ./c_api_secs1_smlx --role server   --serial <COMx|/dev/tty*> --baud 9600 --device-id 0x0001 --sml ceid_demo.sml
 *   ./c_api_secs1_smlx --role client   --serial <COMx|/dev/tty*> --baud 9600 --device-id 0x0001 --sml ceid_demo.sml
 *   ./c_api_secs1_smlx --role loopback --device-id 0x0001 --sml ceid_demo.sml
 */

#include <secs/c_api.h>

#include <inttypes.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <signal.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <pthread.h>
#endif

enum role {
    ROLE_SERVER = 0,
    ROLE_CLIENT = 1,
    ROLE_LOOPBACK = 2,
};

struct options {
    enum role role;
    const char *serial;
    int baud;
    uint16_t device_id;
    int reverse_bit;
    int reverse_bit_set;
    const char *sml_path;
};

static void print_usage(const char *argv0) {
    fprintf(stderr,
            "用法:\n"
            "  %s --role <server|client|loopback> [options]\n\n"
            "选项:\n"
            "  --role <server|client|loopback>\n"
            "  --serial <name>      串口名（Windows: COM5/COM10；Linux: /dev/ttyUSB0）\n"
            "  --baud <i32>         波特率（默认 9600；虚拟串口可忽略）\n"
            "  --device-id <u16>    DeviceID（支持 0x 前缀，默认 0x0001）\n"
            "  --reverse-bit <0|1>  覆盖 R-bit 方向位（默认：server=1, client=0）\n"
            "  --sml <path>         SMLX 文件路径（默认 ceid_demo.sml）\n"
            "  -h, --help           显示帮助\n\n"
            "示例（Windows/com0com）:\n"
            "  %s --role server --serial COM5 --baud 9600 --device-id 0x0001 --sml ceid_demo.sml\n"
            "  %s --role client --serial COM6 --baud 9600 --device-id 0x0001 --sml ceid_demo.sml\n"
            "  %s --role loopback --device-id 0x0001 --sml ceid_demo.sml\n",
            argv0,
            argv0,
            argv0,
            argv0);
}

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
    secs_free(msg);
    return 0;
}

static atomic_int g_stop_flag;
static void on_signal(int sig) {
    (void)sig;
    atomic_store(&g_stop_flag, 1);
}

static int parse_u16(const char *s, uint16_t *out) {
    if (!s || !out) {
        return 0;
    }
    int base = 10;
    if (strlen(s) > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        s += 2;
    }
    char *end = NULL;
    unsigned long v = strtoul(s, &end, base);
    if (!end || *end != '\0' || v > 0xFFFFul) {
        return 0;
    }
    *out = (uint16_t)v;
    return 1;
}

static int parse_i32(const char *s, int *out) {
    if (!s || !out) {
        return 0;
    }
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (!end || *end != '\0') {
        return 0;
    }
    if (v < INT32_MIN || v > INT32_MAX) {
        return 0;
    }
    *out = (int)v;
    return 1;
}

static int parse_role(const char *s, enum role *out) {
    if (!s || !out) {
        return 0;
    }
    if (strcmp(s, "server") == 0) {
        *out = ROLE_SERVER;
        return 1;
    }
    if (strcmp(s, "client") == 0) {
        *out = ROLE_CLIENT;
        return 1;
    }
    if (strcmp(s, "loopback") == 0) {
        *out = ROLE_LOOPBACK;
        return 1;
    }
    return 0;
}

static int parse_reverse_bit01(const char *s, int *out) {
    if (!s || !out) {
        return 0;
    }
    if (strcmp(s, "0") == 0) {
        *out = 0;
        return 1;
    }
    if (strcmp(s, "1") == 0) {
        *out = 1;
        return 1;
    }
    return 0;
}

static int parse_args(int argc, char **argv, struct options *out_opt) {
    struct options opt;
    opt.role = ROLE_LOOPBACK;
    opt.serial = "";
    opt.baud = 9600;
    opt.device_id = 0x0001;
    opt.reverse_bit = 0;
    opt.reverse_bit_set = 0;
    opt.sml_path = "ceid_demo.sml";

    for (int i = 1; i < argc; ++i) {
        const char *a = argv[i];

        int need_value = 0;
        const char *v = NULL;
        if (strcmp(a, "--role") == 0 ||
            strcmp(a, "--serial") == 0 ||
            strcmp(a, "--baud") == 0 ||
            strcmp(a, "--device-id") == 0 ||
            strcmp(a, "--reverse-bit") == 0 ||
            strcmp(a, "--sml") == 0) {
            need_value = 1;
        }

        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            return 0;
        }

        if (need_value) {
            if (i + 1 >= argc) {
                fprintf(stderr, "缺少参数值: %s\n", a);
                return 0;
            }
            v = argv[++i];
        }

        if (strcmp(a, "--role") == 0) {
            if (!parse_role(v, &opt.role)) {
                fprintf(stderr, "非法 role: %s\n", v);
                return 0;
            }
            continue;
        }
        if (strcmp(a, "--serial") == 0) {
            opt.serial = v;
            continue;
        }
        if (strcmp(a, "--baud") == 0) {
            int b = 0;
            if (!parse_i32(v, &b) || b <= 0) {
                fprintf(stderr, "非法 baud: %s\n", v);
                return 0;
            }
            opt.baud = b;
            continue;
        }
        if (strcmp(a, "--device-id") == 0) {
            if (!parse_u16(v, &opt.device_id)) {
                fprintf(stderr, "非法 device-id: %s\n", v);
                return 0;
            }
            continue;
        }
        if (strcmp(a, "--reverse-bit") == 0) {
            int rb = 0;
            if (!parse_reverse_bit01(v, &rb)) {
                fprintf(stderr, "非法 reverse-bit: %s\n", v);
                return 0;
            }
            opt.reverse_bit = rb;
            opt.reverse_bit_set = 1;
            continue;
        }
        if (strcmp(a, "--sml") == 0) {
            opt.sml_path = v;
            continue;
        }

        fprintf(stderr, "未知参数: %s\n", a);
        return 0;
    }

    if (!opt.reverse_bit_set) {
        if (opt.role == ROLE_SERVER) {
            opt.reverse_bit = 1;
        } else if (opt.role == ROLE_CLIENT) {
            opt.reverse_bit = 0;
        }
    }

    if ((opt.role == ROLE_SERVER || opt.role == ROLE_CLIENT) &&
        (!opt.serial || opt.serial[0] == '\0')) {
        fprintf(stderr, "缺少 --serial（server/client 需要真实串口或虚拟串口）\n");
        return 0;
    }

    *out_opt = opt;
    return 1;
}

/* ------------------------------ SMLX：业务变量注入 ------------------------------ */

struct device_data {
    const char *device_name;
    uint8_t status_code;
    uint32_t uptime_seconds;

    float t1;
    float t2;
    float t3;

    uint16_t alarm_count;
    const char *alarm_msg_1;
    const char *alarm_msg_2;

    uint32_t total_count;
    uint32_t good_count;
    uint32_t bad_count;
};

static void init_device_data(struct device_data *d) {
    d->device_name = "EQUIPMENT-001";
    d->status_code = 1;
    d->uptime_seconds = 12345;
    d->t1 = 25.5f;
    d->t2 = 26.3f;
    d->t3 = 24.8f;
    d->alarm_count = 2;
    d->alarm_msg_1 = "High temperature warning";
    d->alarm_msg_2 = "Low pressure alert";
    d->total_count = 10000;
    d->good_count = 9850;
    d->bad_count = 150;
}

static secs_error_t fill_context_for_response(const char *response_name,
                                              const struct device_data *d,
                                              secs_sml_render_context_t *ctx) {
    secs_error_t err = (secs_error_t){0, NULL};

    if (strcmp(response_name, "status_response") == 0) {
        if (!secs_error_is_ok(err = secs_sml_render_context_set_ascii(
                                   ctx, "DEVICE_NAME", d->device_name))) {
            return err;
        }
        if (!secs_error_is_ok(err = secs_sml_render_context_set_u1(
                                   ctx, "STATUS_CODE", d->status_code))) {
            return err;
        }
        if (!secs_error_is_ok(err = secs_sml_render_context_set_u4(
                                   ctx, "UPTIME_SECONDS", d->uptime_seconds))) {
            return err;
        }
        return err;
    }

    if (strcmp(response_name, "temperature_response") == 0) {
        float avg = (d->t1 + d->t2 + d->t3) / 3.0f;
        if (!secs_error_is_ok(err = secs_sml_render_context_set_f4(ctx, "TEMP_SENSOR_1", d->t1))) {
            return err;
        }
        if (!secs_error_is_ok(err = secs_sml_render_context_set_f4(ctx, "TEMP_SENSOR_2", d->t2))) {
            return err;
        }
        if (!secs_error_is_ok(err = secs_sml_render_context_set_f4(ctx, "TEMP_SENSOR_3", d->t3))) {
            return err;
        }
        if (!secs_error_is_ok(err = secs_sml_render_context_set_f4(ctx, "TEMP_AVG", avg))) {
            return err;
        }
        return err;
    }

    if (strcmp(response_name, "alarm_response") == 0) {
        if (!secs_error_is_ok(err = secs_sml_render_context_set_u2(
                                   ctx, "ALARM_COUNT", d->alarm_count))) {
            return err;
        }
        if (!secs_error_is_ok(err = secs_sml_render_context_set_ascii(
                                   ctx, "ALARM_MSG_1", d->alarm_msg_1))) {
            return err;
        }
        if (!secs_error_is_ok(err = secs_sml_render_context_set_ascii(
                                   ctx, "ALARM_MSG_2", d->alarm_msg_2))) {
            return err;
        }
        return err;
    }

    if (strcmp(response_name, "production_response") == 0) {
        float yield = 0.0f;
        if (d->total_count != 0u) {
            yield = (float)d->good_count / (float)d->total_count;
        }
        if (!secs_error_is_ok(err = secs_sml_render_context_set_u4(ctx, "TOTAL_COUNT", d->total_count))) {
            return err;
        }
        if (!secs_error_is_ok(err = secs_sml_render_context_set_u4(ctx, "GOOD_COUNT", d->good_count))) {
            return err;
        }
        if (!secs_error_is_ok(err = secs_sml_render_context_set_u4(ctx, "BAD_COUNT", d->bad_count))) {
            return err;
        }
        if (!secs_error_is_ok(err = secs_sml_render_context_set_f4(ctx, "YIELD_RATE", yield))) {
            return err;
        }
        return err;
    }

    return err;
}

struct server_state {
    const secs_sml_runtime_t *rt;
    struct device_data d;
};

static secs_error_t s6f11_smlx_handler(void *user_data,
                                       const secs_data_message_view_t *req,
                                       uint8_t **out_body,
                                       size_t *out_body_n) {
    struct server_state *st = (struct server_state *)user_data;
    *out_body = NULL;
    *out_body_n = 0;

    printf("[server][handler] recv S%uF%u W=%d SB=0x%08" PRIX32 " body=%zu\n",
           req->stream,
           req->function,
           req->w_bit,
           req->system_bytes,
           req->body_n);

    if (!req->w_bit) {
        return (secs_error_t){0, NULL};
    }

    char *rsp_name = NULL;
    secs_sml_render_context_t *captures = NULL;

    secs_error_t err = secs_sml_runtime_match_response_with_capture(
        st->rt, req->stream, req->function, req->body, req->body_n, NULL, &rsp_name, &captures);
    if (!secs_error_is_ok(err)) {
        secs_sml_render_context_destroy(captures);
        secs_free(rsp_name);
        return err;
    }

    if (!rsp_name) {
        secs_sml_render_context_destroy(captures);
        return (secs_error_t){(int)SECS_C_API_INVALID_ARGUMENT, "secs.c_api"};
    }

    err = fill_context_for_response(rsp_name, &st->d, captures);
    if (!secs_error_is_ok(err)) {
        secs_sml_render_context_destroy(captures);
        secs_free(rsp_name);
        return err;
    }

    uint8_t *rsp_body = NULL;
    size_t rsp_body_n = 0;
    uint8_t out_stream = 0;
    uint8_t out_function = 0;
    int out_w_bit = 0;

    err = secs_sml_runtime_encode_message_body(
        st->rt,
        rsp_name,
        captures,
        &rsp_body,
        &rsp_body_n,
        &out_stream,
        &out_function,
        &out_w_bit);

    secs_sml_render_context_destroy(captures);
    secs_free(rsp_name);

    if (!secs_error_is_ok(err)) {
        secs_free(rsp_body);
        return err;
    }

    if (out_stream != req->stream || out_function != (uint8_t)(req->function + 1u) || out_w_bit) {
        secs_free(rsp_body);
        return (secs_error_t){(int)SECS_C_API_INVALID_ARGUMENT, "secs.c_api"};
    }

    *out_body = rsp_body;
    *out_body_n = rsp_body_n;
    return (secs_error_t){0, NULL};
}

static int run_client_requests(secs_protocol_session_t *proto,
                               const secs_sml_runtime_t *rt) {
    struct req_def {
        const char *name;
        uint16_t ceid;
    };
    static const struct req_def reqs[] = {
        {"req_status", 0x1001},
        {"req_temperature", 0x1002},
        {"req_alarm", 0x1003},
        {"req_production", 0x1004},
    };

    for (size_t i = 0; i < 4; ++i) {
        const struct req_def *r = &reqs[i];
        const uint16_t dataid = (uint16_t)(i + 1u);

        secs_error_t err = (secs_error_t){0, NULL};
        secs_sml_render_context_t *ctx = NULL;
        uint8_t *req_body = NULL;
        size_t req_body_n = 0;
        uint8_t stream = 0;
        uint8_t function = 0;
        int w_bit = 0;

        if (!secs_error_is_ok(err = secs_sml_render_context_create(&ctx))) {
            goto cleanup_one;
        }
        if (!secs_error_is_ok(err = secs_sml_render_context_set_u2(ctx, "DATAID", dataid))) {
            goto cleanup_one;
        }

        err = secs_sml_runtime_encode_message_body(
            rt,
            r->name,
            ctx,
            &req_body,
            &req_body_n,
            &stream,
            &function,
            &w_bit);
        if (!secs_error_is_ok(err)) {
            goto cleanup_one;
        }

        if (stream != 6u || function != 11u || !w_bit) {
            fprintf(stderr, "[失败] request template mismatch: %s\n", r->name);
            err = (secs_error_t){(int)SECS_C_API_INVALID_ARGUMENT, "secs.c_api"};
            goto cleanup_one;
        }

        printf("\n[client] request %s CEID=0x%04X DATAID=%u\n",
               r->name,
               (unsigned)r->ceid,
               (unsigned)dataid);

        secs_data_message_t reply;
        memset(&reply, 0, sizeof(reply));
        if (!ensure_ok("secs_protocol_session_request",
                       secs_protocol_session_request(
                           proto, stream, function, req_body, req_body_n, 3000, &reply))) {
            secs_data_message_free(&reply);
            err = (secs_error_t){(int)SECS_C_API_INVALID_ARGUMENT, "secs.c_api"};
            goto cleanup_one;
        }

        printf("[client] recv S%uF%u W=%d SB=0x%08" PRIX32 " body=%zu\n",
               reply.stream,
               reply.function,
               reply.w_bit,
               reply.system_bytes,
               reply.body_n);
        secs_data_message_free(&reply);

cleanup_one:
        secs_free(req_body);
        secs_sml_render_context_destroy(ctx);
        if (!secs_error_is_ok(err)) {
            ensure_ok("client_requests", err);
            return 1;
        }
    }

    return 0;
}

/* ------------------------------ server poll loop（线程） ------------------------------ */

struct poll_args {
    secs_protocol_session_t *sess;
    atomic_int *stop;
    secs_error_t out_err;
};

#if defined(_WIN32)
static DWORD WINAPI poll_thread_win(LPVOID p) {
    struct poll_args *args = (struct poll_args *)p;
    args->out_err = (secs_error_t){0, NULL};

    while (atomic_load(args->stop) == 0) {
        int handled = 0;
        secs_error_t err = secs_protocol_session_poll_once(args->sess, 50, &handled);
        if (!secs_error_is_ok(err)) {
            args->out_err = err;
            break;
        }
    }
    return 0;
}
#else
static void *poll_thread(void *p) {
    struct poll_args *args = (struct poll_args *)p;
    args->out_err = (secs_error_t){0, NULL};

    while (atomic_load(args->stop) == 0) {
        int handled = 0;
        secs_error_t err = secs_protocol_session_poll_once(args->sess, 50, &handled);
        if (!secs_error_is_ok(err)) {
            args->out_err = err;
            break;
        }
    }
    return NULL;
}
#endif

static int read_file_all(const char *path, char **out_buf, size_t *out_n) {
    *out_buf = NULL;
    *out_n = 0;

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        fprintf(stderr, "[失败] fopen(%s)\n", path);
        return 0;
    }

    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return 0;
    }
    long end = ftell(fp);
    if (end < 0) {
        fclose(fp);
        return 0;
    }
    if (fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return 0;
    }

    size_t n = (size_t)end;
    char *buf = (char *)secs_malloc(n + 1);
    if (!buf) {
        fclose(fp);
        return 0;
    }

    size_t rd = fread(buf, 1, n, fp);
    fclose(fp);
    if (rd != n) {
        secs_free(buf);
        return 0;
    }
    buf[n] = '\0';

    *out_buf = buf;
    *out_n = n;
    return 1;
}

static int load_sml_runtime(const char *path, secs_sml_runtime_t **out_rt) {
    *out_rt = NULL;
    secs_sml_runtime_t *rt = NULL;
    char *buf = NULL;
    size_t n = 0;

    if (!read_file_all(path, &buf, &n)) {
        fprintf(stderr, "[失败] read_file_all(%s)\n", path);
        goto cleanup;
    }

    if (!ensure_ok("secs_sml_runtime_create", secs_sml_runtime_create(&rt))) {
        goto cleanup;
    }
    if (!ensure_ok("secs_sml_runtime_load", secs_sml_runtime_load(rt, buf, n))) {
        goto cleanup;
    }

    secs_free(buf);
    *out_rt = rt;
    return 1;

cleanup:
    secs_free(buf);
    secs_sml_runtime_destroy(rt);
    return 0;
}

static int run_server(const struct options *opt, const secs_sml_runtime_t *rt) {
    printf("=== C API SECS-I SMLX Server ===\n\n");
    printf("serial: %s baud=%d device_id=0x%04X reverse_bit=%d\n",
           opt->serial,
           opt->baud,
           (unsigned)opt->device_id,
           opt->reverse_bit);
    printf("sml: %s\n\n", opt->sml_path);

    int exit_code = 1;
    secs_context_t *ctx = NULL;
    secs_protocol_session_t *proto = NULL;

    struct server_state st;
    st.rt = rt;
    init_device_data(&st.d);

    if (!ensure_ok("secs_context_create", secs_context_create(&ctx))) {
        goto cleanup;
    }

    secs_protocol_session_options_t proto_opt;
    memset(&proto_opt, 0, sizeof(proto_opt));
    proto_opt.t3_ms = 3000;
    proto_opt.poll_interval_ms = 10;

    if (!ensure_ok("secs_protocol_session_create_from_secs1_serial",
                   secs_protocol_session_create_from_secs1_serial(
                       ctx,
                       opt->serial,
                       opt->baud,
                       opt->device_id,
                       opt->reverse_bit,
                       &proto_opt,
                       &proto))) {
        goto cleanup;
    }

    if (!ensure_ok("secs_protocol_session_set_handler(S6F11)",
                   secs_protocol_session_set_handler(
                       proto, 6, 11, s6f11_smlx_handler, &st))) {
        goto cleanup;
    }

    printf("[server] ready (Ctrl+C to exit)\n");
    atomic_store(&g_stop_flag, 0);
    (void)signal(SIGINT, on_signal);
    (void)signal(SIGTERM, on_signal);
    while (atomic_load(&g_stop_flag) == 0) {
        int handled = 0;
        secs_error_t err = secs_protocol_session_poll_once(proto, 100, &handled);
        if (!secs_error_is_ok(err)) {
            ensure_ok("secs_protocol_session_poll_once", err);
            goto cleanup;
        }
    }
    exit_code = 0;

cleanup:
    secs_protocol_session_stop(proto);
    secs_protocol_session_destroy(proto);
    secs_context_destroy(ctx);
    return exit_code;
}

static int run_client(const struct options *opt, const secs_sml_runtime_t *rt) {
    printf("=== C API SECS-I SMLX Client ===\n\n");
    printf("serial: %s baud=%d device_id=0x%04X reverse_bit=%d\n",
           opt->serial,
           opt->baud,
           (unsigned)opt->device_id,
           opt->reverse_bit);
    printf("sml: %s\n\n", opt->sml_path);

    int exit_code = 1;
    secs_context_t *ctx = NULL;
    secs_protocol_session_t *proto = NULL;

    if (!ensure_ok("secs_context_create", secs_context_create(&ctx))) {
        goto cleanup;
    }

    secs_protocol_session_options_t proto_opt;
    memset(&proto_opt, 0, sizeof(proto_opt));
    proto_opt.t3_ms = 3000;
    proto_opt.poll_interval_ms = 10;

    if (!ensure_ok("secs_protocol_session_create_from_secs1_serial",
                   secs_protocol_session_create_from_secs1_serial(
                       ctx,
                       opt->serial,
                       opt->baud,
                       opt->device_id,
                       opt->reverse_bit,
                       &proto_opt,
                       &proto))) {
        goto cleanup;
    }

    exit_code = run_client_requests(proto, rt);
    if (exit_code == 0) {
        printf("\nPASS\n");
    }

cleanup:
    secs_protocol_session_stop(proto);
    secs_protocol_session_destroy(proto);
    secs_context_destroy(ctx);
    return exit_code;
}

static int run_loopback(const struct options *opt, const secs_sml_runtime_t *rt) {
    printf("=== C API SECS-I SMLX Loopback ===\n\n");
    printf("device_id=0x%04X\n", (unsigned)opt->device_id);
    printf("sml: %s\n\n", opt->sml_path);

    int exit_code = 1;
    secs_context_t *ctx = NULL;
    secs_protocol_session_t *host = NULL;
    secs_protocol_session_t *equip = NULL;

    struct server_state st;
    st.rt = rt;
    init_device_data(&st.d);

    if (!ensure_ok("secs_context_create", secs_context_create(&ctx))) {
        goto cleanup;
    }

    secs_protocol_session_options_t proto_opt;
    memset(&proto_opt, 0, sizeof(proto_opt));
    proto_opt.t3_ms = 3000;
    proto_opt.poll_interval_ms = 10;

    if (!ensure_ok("secs_protocol_session_create_from_secs1_memory_duplex",
                   secs_protocol_session_create_from_secs1_memory_duplex(
                       ctx, opt->device_id, &proto_opt, &host, &equip))) {
        goto cleanup;
    }

    if (!ensure_ok("secs_protocol_session_set_handler(equip S6F11)",
                   secs_protocol_session_set_handler(
                       equip, 6, 11, s6f11_smlx_handler, &st))) {
        goto cleanup;
    }

    struct poll_args args;
    memset(&args, 0, sizeof(args));
    args.sess = equip;
    args.stop = &g_stop_flag;

    atomic_store(&g_stop_flag, 0);
#if defined(_WIN32)
    HANDLE th = CreateThread(NULL, 0, poll_thread_win, &args, 0, NULL);
    if (!th) {
        fprintf(stderr, "[失败] CreateThread\n");
        goto cleanup;
    }
#else
    pthread_t th;
    if (pthread_create(&th, NULL, poll_thread, &args) != 0) {
        fprintf(stderr, "[失败] pthread_create\n");
        goto cleanup;
    }
#endif

    exit_code = run_client_requests(host, rt);

    atomic_store(&g_stop_flag, 1);
    secs_protocol_session_stop(host);
    secs_protocol_session_stop(equip);

#if defined(_WIN32)
    (void)WaitForSingleObject(th, INFINITE);
    CloseHandle(th);
#else
    (void)pthread_join(th, NULL);
#endif

    if (!secs_error_is_ok(args.out_err)) {
        ensure_ok("equip poll loop", args.out_err);
        exit_code = 1;
    }

    if (exit_code == 0) {
        printf("\nPASS\n");
    }

cleanup:
    secs_protocol_session_stop(host);
    secs_protocol_session_stop(equip);
    secs_protocol_session_destroy(host);
    secs_protocol_session_destroy(equip);
    secs_context_destroy(ctx);
    return exit_code;
}

int main(int argc, char **argv) {
    struct options opt;
    if (!parse_args(argc, argv, &opt)) {
        print_usage(argv[0]);
        return 2;
    }

    secs_sml_runtime_t *rt = NULL;
    if (!load_sml_runtime(opt.sml_path, &rt)) {
        fprintf(stderr, "ERROR: failed to load SML: %s\n", opt.sml_path);
        return 2;
    }

    int rc = 2;
    switch (opt.role) {
    case ROLE_SERVER:
        rc = run_server(&opt, rt);
        break;
    case ROLE_CLIENT:
        rc = run_client(&opt, rt);
        break;
    case ROLE_LOOPBACK:
        rc = run_loopback(&opt, rt);
        break;
    }

    secs_sml_runtime_destroy(rt);
    return rc;
}
