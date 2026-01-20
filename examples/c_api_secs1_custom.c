/*
 * @file c_api_secs1_custom.c
 * @brief C API 示例：SECS-I（串口）+ 自定义请求-响应（Code-First）
 *
 * 目标：
 * - 与 C++ 示例 `secs1_custom` 保持“功能对应”：
 *   Host(client) 发送多条 S6F11(W=1) CEID 查询，Equipment(server) 回 S6F12；
 * - 演示 C 语言如何在 SECS-I（半双工）上使用 `secs_protocol_session_*`：
 *   - client：直接用 secs_protocol_session_request（内部会驱动收发）
 *   - server：主循环使用 secs_protocol_session_poll_once 处理入站并触发 handler
 *
 * 角色：
 * - server：Equipment 端（默认 reverse_bit=1）
 * - client：Host 端（默认 reverse_bit=0）
 * - loopback：使用 secs_protocol_session_create_from_secs1_memory_duplex 在同进程跑通端到端
 *
 * 用法：
 *   ./c_api_secs1_custom --role server   --serial <COMx|/dev/tty*> --baud 9600 --device-id 0x0001
 *   ./c_api_secs1_custom --role client   --serial <COMx|/dev/tty*> --baud 9600 --device-id 0x0001
 *   ./c_api_secs1_custom --role loopback --device-id 0x0001
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

enum item_style {
    ITEM_STYLE_LEGACY = 0,
    ITEM_STYLE_BUILDER = 1,
};

struct options {
    enum role role;
    const char *serial;
    int baud;
    uint16_t device_id;
    int reverse_bit;
    int reverse_bit_set;
    enum item_style item_style;
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
            "  --item-style <legacy|builder>\n"
            "                      SECS-II Item 构造写法（默认 builder；builder 更适合嵌套 List）\n"
            "  -h, --help           显示帮助\n\n"
            "示例（Windows/com0com）:\n"
            "  %s --role server --serial COM5 --baud 9600 --device-id 0x0001\n"
            "  %s --role client --serial COM6 --baud 9600 --device-id 0x0001\n"
            "  %s --role loopback --device-id 0x0001\n",
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

static int parse_item_style(const char *s, enum item_style *out) {
    if (!s || !out) {
        return 0;
    }
    if (strcmp(s, "legacy") == 0) {
        *out = ITEM_STYLE_LEGACY;
        return 1;
    }
    if (strcmp(s, "builder") == 0) {
        *out = ITEM_STYLE_BUILDER;
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
    opt.item_style = ITEM_STYLE_BUILDER;

    for (int i = 1; i < argc; ++i) {
        const char *a = argv[i];

        int need_value = 0;
        const char *v = NULL;
        if (strcmp(a, "--role") == 0 ||
            strcmp(a, "--serial") == 0 ||
            strcmp(a, "--baud") == 0 ||
            strcmp(a, "--device-id") == 0 ||
            strcmp(a, "--reverse-bit") == 0 ||
            strcmp(a, "--item-style") == 0) {
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
        if (strcmp(a, "--item-style") == 0) {
            if (!parse_item_style(v, &opt.item_style)) {
                fprintf(stderr, "非法 item-style: %s\n", v);
                return 0;
            }
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

/* ------------------------------ 业务：S6F11 / S6F12 ------------------------------ */

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

static secs_error_t decode_s6f11_fields(const secs_data_message_view_t *req,
                                       uint16_t *out_dataid,
                                       uint16_t *out_ceid) {
    *out_dataid = 0;
    *out_ceid = 0;

    size_t consumed = 0;
    secs_ii_item_t *root = NULL;
    secs_error_t err = secs_ii_decode_one(req->body, req->body_n, &consumed, &root);
    if (!secs_error_is_ok(err)) {
        secs_ii_item_destroy(root);
        return err;
    }

    uint16_t dataid = 0;
    uint16_t ceid = 0;
    err = secs_ii_item_get_u2_at_path(root, &dataid, 1, (size_t)0);
    if (!secs_error_is_ok(err)) {
        secs_ii_item_destroy(root);
        return err;
    }
    err = secs_ii_item_get_u2_at_path(root, &ceid, 1, (size_t)1);
    if (!secs_error_is_ok(err)) {
        secs_ii_item_destroy(root);
        return err;
    }

    secs_ii_item_destroy(root);
    *out_dataid = dataid;
    *out_ceid = ceid;
    return (secs_error_t){0, NULL};
}

static secs_error_t encode_s6f11_body_legacy(uint16_t dataid,
                                            uint16_t ceid,
                                            uint8_t **out_body,
                                            size_t *out_body_n) {
    *out_body = NULL;
    *out_body_n = 0;

    secs_error_t err = (secs_error_t){0, NULL};
    secs_ii_item_t *root = NULL;
    secs_ii_item_t *params = NULL;

    if (!secs_error_is_ok(err = secs_ii_item_create_list(&root))) {
        goto cleanup;
    }
    if (!secs_error_is_ok(err = secs_ii_item_list_append_u2(root, dataid))) {
        goto cleanup;
    }
    if (!secs_error_is_ok(err = secs_ii_item_list_append_u2(root, ceid))) {
        goto cleanup;
    }

    if (!secs_error_is_ok(err = secs_ii_item_create_list(&params))) {
        goto cleanup;
    }
    if (!secs_error_is_ok(err = secs_ii_item_list_append_take(root, &params))) {
        goto cleanup;
    }

    if (!secs_error_is_ok(err = secs_ii_encode(root, out_body, out_body_n))) {
        goto cleanup;
    }

cleanup:
    secs_ii_item_destroy(params);
    secs_ii_item_destroy(root);
    return err;
}

static secs_error_t encode_s6f11_body_builder(uint16_t dataid,
                                             uint16_t ceid,
                                             uint8_t **out_body,
                                             size_t *out_body_n) {
    *out_body = NULL;
    *out_body_n = 0;

    secs_error_t err = (secs_error_t){0, NULL};
    secs_ii_builder_t *b = NULL;
    secs_ii_item_t *root = NULL;

    if (!secs_error_is_ok(err = secs_ii_builder_create(&b))) {
        goto cleanup;
    }
    if (!secs_error_is_ok(err = secs_ii_builder_list_begin(b))) {
        goto cleanup;
    }
    if (!secs_error_is_ok(err = secs_ii_builder_add_u2(b, dataid))) {
        goto cleanup;
    }
    if (!secs_error_is_ok(err = secs_ii_builder_add_u2(b, ceid))) {
        goto cleanup;
    }
    if (!secs_error_is_ok(err = secs_ii_builder_list_begin(b))) {
        goto cleanup;
    }
    if (!secs_error_is_ok(err = secs_ii_builder_list_end(b))) {
        goto cleanup;
    }
    if (!secs_error_is_ok(err = secs_ii_builder_list_end(b))) {
        goto cleanup;
    }

    if (!secs_error_is_ok(err = secs_ii_builder_finalize(b, &root))) {
        goto cleanup;
    }
    if (!secs_error_is_ok(err = secs_ii_encode(root, out_body, out_body_n))) {
        goto cleanup;
    }

cleanup:
    secs_ii_item_destroy(root);
    secs_ii_builder_destroy(b);
    if (!secs_error_is_ok(err)) {
        secs_free(*out_body);
        *out_body = NULL;
        *out_body_n = 0;
    }
    return err;
}

static secs_error_t encode_s6f11_body_custom(uint16_t dataid,
                                            uint16_t ceid,
                                            enum item_style style,
                                            uint8_t **out_body,
                                            size_t *out_body_n) {
    if (style == ITEM_STYLE_BUILDER) {
        return encode_s6f11_body_builder(dataid, ceid, out_body, out_body_n);
    }
    return encode_s6f11_body_legacy(dataid, ceid, out_body, out_body_n);
}

static secs_error_t encode_s6f12_body_legacy(uint16_t dataid,
                                            uint16_t ceid,
                                            const struct device_data *d,
                                            uint8_t **out_body,
                                            size_t *out_body_n) {
    *out_body = NULL;
    *out_body_n = 0;

    secs_error_t err = (secs_error_t){0, NULL};
    secs_ii_item_t *root = NULL;
    secs_ii_item_t *data = NULL;
    secs_ii_item_t *nested = NULL;

    if (!secs_error_is_ok(err = secs_ii_item_create_list(&root))) {
        goto cleanup;
    }
    if (!secs_error_is_ok(err = secs_ii_item_list_append_u2(root, dataid))) {
        goto cleanup;
    }
    if (!secs_error_is_ok(err = secs_ii_item_list_append_u2(root, ceid))) {
        goto cleanup;
    }

    if (!secs_error_is_ok(err = secs_ii_item_create_list(&data))) {
        goto cleanup;
    }

    if (ceid == 0x1001u) {
        if (!secs_error_is_ok(
                err = secs_ii_item_list_append_ascii(data, d->device_name))) {
            goto cleanup;
        }
        if (!secs_error_is_ok(
                err = secs_ii_item_list_append_u1(data, d->status_code))) {
            goto cleanup;
        }
        if (!secs_error_is_ok(
                err = secs_ii_item_list_append_u4(data, d->uptime_seconds))) {
            goto cleanup;
        }
    } else if (ceid == 0x1002u) {
        float temps[3];
        temps[0] = d->t1;
        temps[1] = d->t2;
        temps[2] = d->t3;
        float avg = (temps[0] + temps[1] + temps[2]) / 3.0f;

        if (!secs_error_is_ok(
                err = secs_ii_item_list_append_ascii(data, "Temperature Sensors"))) {
            goto cleanup;
        }
        if (!secs_error_is_ok(err = secs_ii_item_create_list(&nested))) {
            goto cleanup;
        }
        if (!secs_error_is_ok(err = secs_ii_item_list_append_f4_values(nested, temps, 3))) {
            goto cleanup;
        }
        if (!secs_error_is_ok(err = secs_ii_item_list_append_take(data, &nested))) {
            goto cleanup;
        }
        if (!secs_error_is_ok(err = secs_ii_item_list_append_f4(data, avg))) {
            goto cleanup;
        }
    } else if (ceid == 0x1003u) {
        if (!secs_error_is_ok(err = secs_ii_item_list_append_u2(data, d->alarm_count))) {
            goto cleanup;
        }
        if (!secs_error_is_ok(err = secs_ii_item_create_list(&nested))) {
            goto cleanup;
        }
        if (!secs_error_is_ok(err = secs_ii_item_list_append_ascii(nested, d->alarm_msg_1))) {
            goto cleanup;
        }
        if (!secs_error_is_ok(err = secs_ii_item_list_append_ascii(nested, d->alarm_msg_2))) {
            goto cleanup;
        }
        if (!secs_error_is_ok(err = secs_ii_item_list_append_take(data, &nested))) {
            goto cleanup;
        }
    } else if (ceid == 0x1004u) {
        float yield = 0.0f;
        if (d->total_count != 0u) {
            yield = (float)d->good_count / (float)d->total_count;
        }

        if (!secs_error_is_ok(err = secs_ii_item_list_append_u4(data, d->total_count))) {
            goto cleanup;
        }
        if (!secs_error_is_ok(err = secs_ii_item_list_append_u4(data, d->good_count))) {
            goto cleanup;
        }
        if (!secs_error_is_ok(err = secs_ii_item_list_append_u4(data, d->bad_count))) {
            goto cleanup;
        }
        if (!secs_error_is_ok(err = secs_ii_item_list_append_f4(data, yield))) {
            goto cleanup;
        }
    } else {
        if (!secs_error_is_ok(err = secs_ii_item_list_append_ascii(data, "UNKNOWN_CEID"))) {
            goto cleanup;
        }
    }

    if (!secs_error_is_ok(err = secs_ii_item_list_append_take(root, &data))) {
        goto cleanup;
    }

    if (!secs_error_is_ok(err = secs_ii_encode(root, out_body, out_body_n))) {
        goto cleanup;
    }

cleanup:
    secs_ii_item_destroy(nested);
    secs_ii_item_destroy(data);
    secs_ii_item_destroy(root);
    return err;
}

static secs_error_t encode_s6f12_body_builder(uint16_t dataid,
                                             uint16_t ceid,
                                             const struct device_data *d,
                                             uint8_t **out_body,
                                             size_t *out_body_n) {
    *out_body = NULL;
    *out_body_n = 0;

    secs_error_t err = (secs_error_t){0, NULL};
    secs_ii_builder_t *b = NULL;
    secs_ii_item_t *root = NULL;
    secs_ii_item_t *tmp = NULL;

    if (!secs_error_is_ok(err = secs_ii_builder_create(&b))) {
        goto cleanup;
    }
    if (!secs_error_is_ok(err = secs_ii_builder_list_begin(b))) {
        goto cleanup;
    }
    if (!secs_error_is_ok(err = secs_ii_builder_add_u2(b, dataid))) {
        goto cleanup;
    }
    if (!secs_error_is_ok(err = secs_ii_builder_add_u2(b, ceid))) {
        goto cleanup;
    }
    if (!secs_error_is_ok(err = secs_ii_builder_list_begin(b))) {
        goto cleanup;
    }

    if (ceid == 0x1001u) {
        if (!secs_error_is_ok(err = secs_ii_builder_add_ascii(b, d->device_name))) {
            goto cleanup;
        }
        if (!secs_error_is_ok(err = secs_ii_builder_add_u1(b, d->status_code))) {
            goto cleanup;
        }
        if (!secs_error_is_ok(err = secs_ii_builder_add_u4(b, d->uptime_seconds))) {
            goto cleanup;
        }
    } else if (ceid == 0x1002u) {
        float temps[3];
        temps[0] = d->t1;
        temps[1] = d->t2;
        temps[2] = d->t3;
        float avg = (temps[0] + temps[1] + temps[2]) / 3.0f;

        if (!secs_error_is_ok(err = secs_ii_builder_add_ascii(b, "Temperature Sensors"))) {
            goto cleanup;
        }
        if (!secs_error_is_ok(err = secs_ii_builder_list_begin(b))) {
            goto cleanup;
        }
        if (!secs_error_is_ok(err = secs_ii_item_create_f4(temps, 3, &tmp))) {
            goto cleanup;
        }
        if (!secs_error_is_ok(err = secs_ii_builder_add_item_take(b, &tmp))) {
            goto cleanup;
        }
        if (!secs_error_is_ok(err = secs_ii_builder_list_end(b))) {
            goto cleanup;
        }
        if (!secs_error_is_ok(err = secs_ii_builder_add_f4(b, avg))) {
            goto cleanup;
        }
    } else if (ceid == 0x1003u) {
        if (!secs_error_is_ok(err = secs_ii_builder_add_u2(b, d->alarm_count))) {
            goto cleanup;
        }
        if (!secs_error_is_ok(err = secs_ii_builder_list_begin(b))) {
            goto cleanup;
        }
        if (!secs_error_is_ok(err = secs_ii_builder_add_ascii(b, d->alarm_msg_1))) {
            goto cleanup;
        }
        if (!secs_error_is_ok(err = secs_ii_builder_add_ascii(b, d->alarm_msg_2))) {
            goto cleanup;
        }
        if (!secs_error_is_ok(err = secs_ii_builder_list_end(b))) {
            goto cleanup;
        }
    } else if (ceid == 0x1004u) {
        float yield = 0.0f;
        if (d->total_count != 0u) {
            yield = (float)d->good_count / (float)d->total_count;
        }

        if (!secs_error_is_ok(err = secs_ii_builder_add_u4(b, d->total_count))) {
            goto cleanup;
        }
        if (!secs_error_is_ok(err = secs_ii_builder_add_u4(b, d->good_count))) {
            goto cleanup;
        }
        if (!secs_error_is_ok(err = secs_ii_builder_add_u4(b, d->bad_count))) {
            goto cleanup;
        }
        if (!secs_error_is_ok(err = secs_ii_builder_add_f4(b, yield))) {
            goto cleanup;
        }
    } else {
        if (!secs_error_is_ok(err = secs_ii_builder_add_ascii(b, "UNKNOWN_CEID"))) {
            goto cleanup;
        }
    }

    if (!secs_error_is_ok(err = secs_ii_builder_list_end(b))) {
        goto cleanup;
    }
    if (!secs_error_is_ok(err = secs_ii_builder_list_end(b))) {
        goto cleanup;
    }

    if (!secs_error_is_ok(err = secs_ii_builder_finalize(b, &root))) {
        goto cleanup;
    }
    if (!secs_error_is_ok(err = secs_ii_encode(root, out_body, out_body_n))) {
        goto cleanup;
    }

cleanup:
    secs_ii_item_destroy(tmp);
    secs_ii_item_destroy(root);
    secs_ii_builder_destroy(b);
    if (!secs_error_is_ok(err)) {
        secs_free(*out_body);
        *out_body = NULL;
        *out_body_n = 0;
    }
    return err;
}

static secs_error_t encode_s6f12_body_custom(uint16_t dataid,
                                            uint16_t ceid,
                                            const struct device_data *d,
                                            enum item_style style,
                                            uint8_t **out_body,
                                            size_t *out_body_n) {
    if (style == ITEM_STYLE_BUILDER) {
        return encode_s6f12_body_builder(dataid, ceid, d, out_body, out_body_n);
    }
    return encode_s6f12_body_legacy(dataid, ceid, d, out_body, out_body_n);
}

struct server_state {
    struct device_data d;
    enum item_style item_style;
};

static secs_error_t s6f11_handler(void *user_data,
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

    if (!req->w_bit) {
        *out_body = NULL;
        *out_body_n = 0;
        return (secs_error_t){0, NULL};
    }

    uint16_t dataid = 0;
    uint16_t ceid = 0;
    secs_error_t err = decode_s6f11_fields(req, &dataid, &ceid);
    if (!secs_error_is_ok(err)) {
        return err;
    }

    printf("[server][handler] parsed DATAID=%u CEID=0x%04X\n",
           (unsigned)dataid,
           (unsigned)ceid);

    return encode_s6f12_body_custom(
        dataid, ceid, &st->d, st->item_style, out_body, out_body_n);
}

static int run_client_requests(secs_protocol_session_t *proto,
                               enum item_style item_style) {
    const uint16_t ceids[4] = {0x1001, 0x1002, 0x1003, 0x1004};

    for (size_t i = 0; i < 4; ++i) {
        const uint16_t dataid = (uint16_t)(i + 1u);
        const uint16_t ceid = ceids[i];

        uint8_t *req_body = NULL;
        size_t req_body_n = 0;

        secs_error_t err = encode_s6f11_body_custom(
            dataid, ceid, item_style, &req_body, &req_body_n);
        if (!secs_error_is_ok(err)) {
            return 1;
        }

        printf("\n[client] request S6F11 CEID=0x%04X DATAID=%u\n",
               (unsigned)ceid,
               (unsigned)dataid);

        secs_data_message_t reply;
        memset(&reply, 0, sizeof(reply));
        if (!ensure_ok("secs_protocol_session_request",
                       secs_protocol_session_request(
                           proto, 6, 11, req_body, req_body_n, 3000, &reply))) {
            secs_data_message_free(&reply);
            secs_free(req_body);
            return 1;
        }

        printf("[client] recv S%uF%u W=%d SB=0x%08" PRIX32 " body=%zu\n",
               reply.stream,
               reply.function,
               reply.w_bit,
               reply.system_bytes,
               reply.body_n);

        secs_data_message_free(&reply);

        secs_free(req_body);
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

static int run_server(const struct options *opt) {
    printf("=== C API SECS-I Custom Server ===\n\n");
    printf("serial: %s baud=%d device_id=0x%04X reverse_bit=%d\n\n",
           opt->serial,
           opt->baud,
           (unsigned)opt->device_id,
           opt->reverse_bit);

    int exit_code = 1;
    secs_context_t *ctx = NULL;
    secs_protocol_session_t *proto = NULL;

    struct server_state st;
    init_device_data(&st.d);
    st.item_style = opt->item_style;

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
                       proto, 6, 11, s6f11_handler, &st))) {
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

static int run_client(const struct options *opt) {
    printf("=== C API SECS-I Custom Client ===\n\n");
    printf("serial: %s baud=%d device_id=0x%04X reverse_bit=%d\n\n",
           opt->serial,
           opt->baud,
           (unsigned)opt->device_id,
           opt->reverse_bit);

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

    exit_code = run_client_requests(proto, opt->item_style);
    if (exit_code == 0) {
        printf("\nPASS\n");
    }

cleanup:
    secs_protocol_session_stop(proto);
    secs_protocol_session_destroy(proto);
    secs_context_destroy(ctx);
    return exit_code;
}

static int run_loopback(const struct options *opt) {
    printf("=== C API SECS-I Custom Loopback ===\n\n");
    printf("device_id=0x%04X\n\n", (unsigned)opt->device_id);

    int exit_code = 1;
    secs_context_t *ctx = NULL;
    secs_protocol_session_t *host = NULL;
    secs_protocol_session_t *equip = NULL;

    struct server_state st;
    init_device_data(&st.d);
    st.item_style = opt->item_style;

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
                       equip, 6, 11, s6f11_handler, &st))) {
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

    exit_code = run_client_requests(host, opt->item_style);

    /* 结束设备端 poll loop 并等待线程退出 */
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

    int rc = 2;
    switch (opt.role) {
    case ROLE_SERVER:
        rc = run_server(&opt);
        break;
    case ROLE_CLIENT:
        rc = run_client(&opt);
        break;
    case ROLE_LOOPBACK:
        rc = run_loopback(&opt);
        break;
    }
    return rc;
}
