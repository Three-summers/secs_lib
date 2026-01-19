/*
 * C API（C ABI）示例：SECS-II Item 的构造 / 编码 / 解码
 *
 * 目标：
 * - 演示纯 C 代码如何通过 `#include <secs/c_api.h>` 使用本库的 SECS-II 编解码；
 * - 展示不透明句柄（secs_ii_item_t）+ 统一错误码（secs_error_t）+ 统一释放（secs_free）。
 *
 * 注意：
 * - 本文件用 C 编译器编译；
 * - 但链接阶段需要 C++ 链接器（底层实现为 C++20），CMake 已为该示例处理。
 */

#include <secs/c_api.h>

#include <inttypes.h>
#include <stddef.h>
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

int main(void) {
    printf("=== C API SECS-II 编解码示例 ===\n\n");
    printf("secs version: %s\n\n", secs_version_string());

    int exit_code = 1;
    secs_ii_item_t *root = NULL;
    uint8_t *encoded = NULL;
    size_t encoded_n = 0;
    secs_ii_item_t *decoded = NULL;

    /* 1) 构造一个 List：[ASCII("Hello SECS"), U2([0x1234, 0xABCD])] */
    if (!ensure_ok("secs_ii_item_create_list",
                   secs_ii_item_create_list(&root))) {
        goto cleanup;
    }

    {
        secs_ii_item_t *ascii = NULL;
        secs_ii_item_t *u2 = NULL;
        const char *hello = "Hello SECS";
        const uint16_t u2_values[] = {0x1234u, 0xABCDu};

        if (!ensure_ok("secs_ii_item_create_ascii",
                       secs_ii_item_create_ascii(hello,
                                                 strlen(hello),
                                                 &ascii))) {
            secs_ii_item_destroy(ascii);
            goto cleanup;
        }
        if (!ensure_ok("secs_ii_item_create_u2",
                       secs_ii_item_create_u2(u2_values,
                                              sizeof(u2_values) /
                                                  sizeof(u2_values[0]),
                                              &u2))) {
            secs_ii_item_destroy(ascii);
            secs_ii_item_destroy(u2);
            goto cleanup;
        }

        if (!ensure_ok("secs_ii_item_list_append(ascii)",
                       secs_ii_item_list_append(root, ascii))) {
            secs_ii_item_destroy(ascii);
            secs_ii_item_destroy(u2);
            goto cleanup;
        }
        if (!ensure_ok("secs_ii_item_list_append(u2)",
                       secs_ii_item_list_append(root, u2))) {
            secs_ii_item_destroy(ascii);
            secs_ii_item_destroy(u2);
            goto cleanup;
        }

        /* list_append 会拷贝 elem，因此可立即释放临时 child。 */
        secs_ii_item_destroy(ascii);
        secs_ii_item_destroy(u2);
    }

    /* 2) 编码为 on-wire bytes（返回的 encoded 需用 secs_free 释放） */
    if (!ensure_ok("secs_ii_encode", secs_ii_encode(root, &encoded, &encoded_n))) {
        goto cleanup;
    }
    printf("[编码] bytes=%zu，前 32B：", encoded_n);
    dump_hex_prefix(encoded, encoded_n, 32);

    /* 3) 解码一条 Item（decode_one：流式接口，返回 consumed） */
    {
        size_t consumed = 0;
        if (!ensure_ok("secs_ii_decode_one",
                       secs_ii_decode_one(encoded,
                                          encoded_n,
                                          &consumed,
                                          &decoded))) {
            goto cleanup;
        }
        printf("[解码] consumed=%zu\n", consumed);
    }

    /* 4) 访问解码结果 */
    {
        secs_ii_item_type_t ty;
        if (!ensure_ok("secs_ii_item_get_type",
                       secs_ii_item_get_type(decoded, &ty))) {
            goto cleanup;
        }
        if (ty != SECS_II_ITEM_LIST) {
            fprintf(stderr, "[失败] decode 结果不是 List\n");
            goto cleanup;
        }

        size_t n = 0;
        if (!ensure_ok("secs_ii_item_list_size",
                       secs_ii_item_list_size(decoded, &n))) {
            goto cleanup;
        }
        printf("[访问] List size=%zu\n", n);

        for (size_t i = 0; i < n; ++i) {
            secs_ii_item_t *child = NULL;
            if (!ensure_ok("secs_ii_item_list_get",
                           secs_ii_item_list_get(decoded, i, &child))) {
                secs_ii_item_destroy(child);
                goto cleanup;
            }

            secs_ii_item_type_t cty;
            if (!ensure_ok("secs_ii_item_get_type(child)",
                           secs_ii_item_get_type(child, &cty))) {
                secs_ii_item_destroy(child);
                goto cleanup;
            }

            if (cty == SECS_II_ITEM_ASCII) {
                const char *p = NULL;
                size_t m = 0;
                if (!ensure_ok("secs_ii_item_ascii_view",
                               secs_ii_item_ascii_view(child, &p, &m))) {
                    secs_ii_item_destroy(child);
                    goto cleanup;
                }
                printf("  - [%zu] ASCII(%zu): ", i, m);
                if (p && m != 0) {
                    printf("%.*s\n", (int)m, p);
                } else {
                    printf("(empty)\n");
                }
            } else if (cty == SECS_II_ITEM_U2) {
                const uint16_t *p = NULL;
                size_t m = 0;
                if (!ensure_ok("secs_ii_item_u2_view",
                               secs_ii_item_u2_view(child, &p, &m))) {
                    secs_ii_item_destroy(child);
                    goto cleanup;
                }
                printf("  - [%zu] U2(%zu):", i, m);
                for (size_t j = 0; j < m; ++j) {
                    printf(" 0x%04" PRIX16, (uint16_t)p[j]);
                }
                putchar('\n');
            } else {
                printf("  - [%zu] type=%d\n", i, (int)cty);
            }

            secs_ii_item_destroy(child);
        }
    }

    exit_code = 0;

cleanup:
    secs_ii_item_destroy(decoded);
    secs_ii_item_destroy(root);
    if (encoded) {
        secs_free(encoded);
    }
    return exit_code;
}

