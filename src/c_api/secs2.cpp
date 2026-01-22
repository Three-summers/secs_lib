#include "c_api/internal.hpp"

// ----------------------------- SECS-II：Item（数据项）
// -----------------------------

static secs_error_t new_item(secs::ii::Item v,
                             secs_ii_item_t **out_item) noexcept {
    if (!out_item) {
        return c_api_err(SECS_C_API_INVALID_ARGUMENT);
    }
    *out_item = nullptr;

    auto *item = new (std::nothrow) secs_ii_item(std::move(v));
    if (!item) {
        return c_api_err(SECS_C_API_OUT_OF_MEMORY);
    }
    *out_item = item;
    return ok();
}

secs_error_t secs_ii_item_create_list(secs_ii_item_t **out_item) {
    return guard_error([&]() -> secs_error_t {
        return new_item(secs::ii::Item::list({}), out_item);
    });
}

secs_error_t secs_ii_item_create_ascii(const char *bytes,
                                       size_t n,
                                       secs_ii_item_t **out_item) {
    return guard_error([&]() -> secs_error_t {
        if (!bytes && n != 0) {
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        }
        std::string s;
        if (bytes && n != 0) {
            s.assign(bytes, bytes + n);
        }
        return new_item(secs::ii::Item::ascii(std::move(s)), out_item);
    });
}

secs_error_t secs_ii_item_create_ascii_cstr(const char *value,
                                            secs_ii_item_t **out_item) {
    return guard_error([&]() -> secs_error_t {
        if (!value) {
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        }
        return secs_ii_item_create_ascii(value, std::strlen(value), out_item);
    });
}

secs_error_t secs_ii_item_create_binary(const uint8_t *bytes,
                                        size_t n,
                                        secs_ii_item_t **out_item) {
    return guard_error([&]() -> secs_error_t {
        if (!bytes && n != 0) {
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        }
        std::vector<byte> v = bytes_to_vec(bytes, n);
        return new_item(secs::ii::Item::binary(std::move(v)), out_item);
    });
}

secs_error_t secs_ii_item_create_boolean(const uint8_t *values01,
                                         size_t n,
                                         secs_ii_item_t **out_item) {
    return guard_error([&]() -> secs_error_t {
        if (!values01 && n != 0) {
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        }
        std::vector<bool> v;
        v.reserve(n);
        for (size_t i = 0; i < n; ++i) {
            v.push_back(values01[i] != 0);
        }
        return new_item(secs::ii::Item::boolean(std::move(v)), out_item);
    });
}

secs_error_t
secs_ii_item_create_i1(const int8_t *v, size_t n, secs_ii_item_t **out_item) {
    return guard_error([&]() -> secs_error_t {
        if (!v && n != 0)
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        // C 语言调用约定：允许 v==NULL 且 n==0；此时必须构造空
        // vector，且不能做空指针算术。
        std::vector<std::int8_t> out;
        if (n != 0) {
            out.assign(v, v + n);
        }
        return new_item(secs::ii::Item::i1(std::move(out)), out_item);
    });
}

secs_error_t
secs_ii_item_create_i2(const int16_t *v, size_t n, secs_ii_item_t **out_item) {
    return guard_error([&]() -> secs_error_t {
        if (!v && n != 0)
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        std::vector<std::int16_t> out;
        if (n != 0) {
            out.assign(v, v + n);
        }
        return new_item(secs::ii::Item::i2(std::move(out)), out_item);
    });
}

secs_error_t
secs_ii_item_create_i4(const int32_t *v, size_t n, secs_ii_item_t **out_item) {
    return guard_error([&]() -> secs_error_t {
        if (!v && n != 0)
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        std::vector<std::int32_t> out;
        if (n != 0) {
            out.assign(v, v + n);
        }
        return new_item(secs::ii::Item::i4(std::move(out)), out_item);
    });
}

secs_error_t
secs_ii_item_create_i8(const int64_t *v, size_t n, secs_ii_item_t **out_item) {
    return guard_error([&]() -> secs_error_t {
        if (!v && n != 0)
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        std::vector<std::int64_t> out;
        if (n != 0) {
            out.assign(v, v + n);
        }
        return new_item(secs::ii::Item::i8(std::move(out)), out_item);
    });
}

secs_error_t
secs_ii_item_create_u1(const uint8_t *v, size_t n, secs_ii_item_t **out_item) {
    return guard_error([&]() -> secs_error_t {
        if (!v && n != 0)
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        std::vector<std::uint8_t> out;
        if (n != 0) {
            out.assign(v, v + n);
        }
        return new_item(secs::ii::Item::u1(std::move(out)), out_item);
    });
}

secs_error_t
secs_ii_item_create_u2(const uint16_t *v, size_t n, secs_ii_item_t **out_item) {
    return guard_error([&]() -> secs_error_t {
        if (!v && n != 0)
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        std::vector<std::uint16_t> out;
        if (n != 0) {
            out.assign(v, v + n);
        }
        return new_item(secs::ii::Item::u2(std::move(out)), out_item);
    });
}

secs_error_t
secs_ii_item_create_u4(const uint32_t *v, size_t n, secs_ii_item_t **out_item) {
    return guard_error([&]() -> secs_error_t {
        if (!v && n != 0)
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        std::vector<std::uint32_t> out;
        if (n != 0) {
            out.assign(v, v + n);
        }
        return new_item(secs::ii::Item::u4(std::move(out)), out_item);
    });
}

secs_error_t
secs_ii_item_create_u8(const uint64_t *v, size_t n, secs_ii_item_t **out_item) {
    return guard_error([&]() -> secs_error_t {
        if (!v && n != 0)
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        std::vector<std::uint64_t> out;
        if (n != 0) {
            out.assign(v, v + n);
        }
        return new_item(secs::ii::Item::u8(std::move(out)), out_item);
    });
}

secs_error_t
secs_ii_item_create_f4(const float *v, size_t n, secs_ii_item_t **out_item) {
    return guard_error([&]() -> secs_error_t {
        if (!v && n != 0)
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        std::vector<float> out;
        if (n != 0) {
            out.assign(v, v + n);
        }
        return new_item(secs::ii::Item::f4(std::move(out)), out_item);
    });
}

secs_error_t
secs_ii_item_create_f8(const double *v, size_t n, secs_ii_item_t **out_item) {
    return guard_error([&]() -> secs_error_t {
        if (!v && n != 0)
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        std::vector<double> out;
        if (n != 0) {
            out.assign(v, v + n);
        }
        return new_item(secs::ii::Item::f8(std::move(out)), out_item);
    });
}

void secs_ii_item_destroy(secs_ii_item_t *item) {
    guard_void([&]() { delete item; });
}

secs_error_t secs_ii_item_clone(const secs_ii_item_t *src,
                                secs_ii_item_t **out_item) {
    return guard_error([&]() -> secs_error_t {
        if (!src) {
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        }
        return new_item(src->item, out_item);
    });
}

secs_error_t secs_ii_item_get_type(const secs_ii_item_t *item,
                                   secs_ii_item_type_t *out_type) {
    return guard_error([&]() -> secs_error_t {
        if (!item || !out_type) {
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        }

        const auto &s = item->item.storage();
        if (std::holds_alternative<secs::ii::List>(s))
            *out_type = SECS_II_ITEM_LIST;
        else if (std::holds_alternative<secs::ii::ASCII>(s))
            *out_type = SECS_II_ITEM_ASCII;
        else if (std::holds_alternative<secs::ii::Binary>(s))
            *out_type = SECS_II_ITEM_BINARY;
        else if (std::holds_alternative<secs::ii::Boolean>(s))
            *out_type = SECS_II_ITEM_BOOLEAN;
        else if (std::holds_alternative<secs::ii::I1>(s))
            *out_type = SECS_II_ITEM_I1;
        else if (std::holds_alternative<secs::ii::I2>(s))
            *out_type = SECS_II_ITEM_I2;
        else if (std::holds_alternative<secs::ii::I4>(s))
            *out_type = SECS_II_ITEM_I4;
        else if (std::holds_alternative<secs::ii::I8>(s))
            *out_type = SECS_II_ITEM_I8;
        else if (std::holds_alternative<secs::ii::U1>(s))
            *out_type = SECS_II_ITEM_U1;
        else if (std::holds_alternative<secs::ii::U2>(s))
            *out_type = SECS_II_ITEM_U2;
        else if (std::holds_alternative<secs::ii::U4>(s))
            *out_type = SECS_II_ITEM_U4;
        else if (std::holds_alternative<secs::ii::U8>(s))
            *out_type = SECS_II_ITEM_U8;
        else if (std::holds_alternative<secs::ii::F4>(s))
            *out_type = SECS_II_ITEM_F4;
        else if (std::holds_alternative<secs::ii::F8>(s))
            *out_type = SECS_II_ITEM_F8;
        else
            return c_api_err(SECS_C_API_EXCEPTION);

        return ok();
    });
}

secs_error_t secs_ii_item_list_size(const secs_ii_item_t *item, size_t *out_n) {
    return guard_error([&]() -> secs_error_t {
        if (!item || !out_n)
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        const auto *list = item->item.get_if<secs::ii::List>();
        if (!list)
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        *out_n = list->size();
        return ok();
    });
}

secs_error_t secs_ii_item_list_get(const secs_ii_item_t *item,
                                   size_t index,
                                   secs_ii_item_t **out_child) {
    return guard_error([&]() -> secs_error_t {
        if (!item || !out_child)
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        *out_child = nullptr;
        const auto *list = item->item.get_if<secs::ii::List>();
        if (!list)
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        if (index >= list->size())
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        return new_item((*list)[index], out_child);
    });
}

secs_error_t secs_ii_item_list_append(secs_ii_item_t *list,
                                      const secs_ii_item_t *elem) {
    return guard_error([&]() -> secs_error_t {
        if (!list || !elem)
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        auto *l = list->item.get_if<secs::ii::List>();
        if (!l)
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        l->push_back(elem->item);
        return ok();
    });
}

// ----------------------------- SECS-II：List 构建便捷 API（P1）
// -----------------------------

namespace {

[[nodiscard]] secs_error_t ii_list_push(secs_ii_item_t *list,
                                        secs::ii::Item elem) noexcept {
    if (!list) {
        return c_api_err(SECS_C_API_INVALID_ARGUMENT);
    }
    auto *l = list->item.get_if<secs::ii::List>();
    if (!l) {
        return c_api_err(SECS_C_API_INVALID_ARGUMENT);
    }
    l->push_back(std::move(elem));
    return ok();
}

} // namespace

secs_error_t secs_ii_item_list_append_take(secs_ii_item_t *list,
                                           secs_ii_item_t **io_elem) {
    return guard_error([&]() -> secs_error_t {
        if (!io_elem || !*io_elem) {
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        }

        // take 语义：确保无论成功/失败/抛异常都能销毁并置空。
        std::unique_ptr<secs_ii_item_t, void (*)(secs_ii_item_t *)> owned(
            *io_elem, secs_ii_item_destroy);
        *io_elem = nullptr;

        if (!list) {
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        }
        auto *l = list->item.get_if<secs::ii::List>();
        if (!l) {
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        }
        l->push_back(owned->item);
        return ok();
    });
}

secs_error_t secs_ii_item_list_append_ascii(secs_ii_item_t *list,
                                            const char *value) {
    return guard_error([&]() -> secs_error_t {
        if (!value) {
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        }
        return ii_list_push(list, secs::ii::Item::ascii(std::string(value)));
    });
}

secs_error_t secs_ii_item_list_append_ascii_n(secs_ii_item_t *list,
                                              const char *bytes,
                                              size_t n) {
    return guard_error([&]() -> secs_error_t {
        if (!bytes && n != 0) {
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        }
        std::string s;
        if (n != 0) {
            s.assign(bytes, bytes + n);
        }
        return ii_list_push(list, secs::ii::Item::ascii(std::move(s)));
    });
}

secs_error_t secs_ii_item_list_append_binary(secs_ii_item_t *list,
                                             const uint8_t *bytes,
                                             size_t n) {
    return guard_error([&]() -> secs_error_t {
        if (!bytes && n != 0) {
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        }
        return ii_list_push(list,
                            secs::ii::Item::binary(bytes_to_vec(bytes, n)));
    });
}

secs_error_t secs_ii_item_list_append_boolean(secs_ii_item_t *list,
                                              uint8_t value01) {
    return guard_error([&]() -> secs_error_t {
        if (value01 != 0 && value01 != 1) {
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        }
        std::vector<bool> v;
        v.push_back(value01 != 0);
        return ii_list_push(list, secs::ii::Item::boolean(std::move(v)));
    });
}

secs_error_t secs_ii_item_list_append_boolean_values(secs_ii_item_t *list,
                                                     const uint8_t *values01,
                                                     size_t n) {
    return guard_error([&]() -> secs_error_t {
        if (!values01 && n != 0) {
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        }
        std::vector<bool> v;
        v.reserve(n);
        for (size_t i = 0; i < n; ++i) {
            if (values01[i] != 0 && values01[i] != 1) {
                return c_api_err(SECS_C_API_INVALID_ARGUMENT);
            }
            v.push_back(values01[i] != 0);
        }
        return ii_list_push(list, secs::ii::Item::boolean(std::move(v)));
    });
}

#define SECS_II_LIST_APPEND_SCALAR_IMPL(c_type, fn_suffix, make_item)            \
    secs_error_t secs_ii_item_list_append_##fn_suffix(secs_ii_item_t *list,      \
                                                      c_type value) {           \
        return guard_error([&]() -> secs_error_t {                               \
            return ii_list_push(list, make_item({value}));                       \
        });                                                                      \
    }

SECS_II_LIST_APPEND_SCALAR_IMPL(int8_t, i1, secs::ii::Item::i1)
SECS_II_LIST_APPEND_SCALAR_IMPL(int16_t, i2, secs::ii::Item::i2)
SECS_II_LIST_APPEND_SCALAR_IMPL(int32_t, i4, secs::ii::Item::i4)
SECS_II_LIST_APPEND_SCALAR_IMPL(int64_t, i8, secs::ii::Item::i8)
SECS_II_LIST_APPEND_SCALAR_IMPL(uint8_t, u1, secs::ii::Item::u1)
SECS_II_LIST_APPEND_SCALAR_IMPL(uint16_t, u2, secs::ii::Item::u2)
SECS_II_LIST_APPEND_SCALAR_IMPL(uint32_t, u4, secs::ii::Item::u4)
SECS_II_LIST_APPEND_SCALAR_IMPL(uint64_t, u8, secs::ii::Item::u8)
SECS_II_LIST_APPEND_SCALAR_IMPL(float, f4, secs::ii::Item::f4)
SECS_II_LIST_APPEND_SCALAR_IMPL(double, f8, secs::ii::Item::f8)

#undef SECS_II_LIST_APPEND_SCALAR_IMPL

#define SECS_II_LIST_APPEND_VALUES_IMPL(c_type, fn_suffix, make_item)            \
    secs_error_t secs_ii_item_list_append_##fn_suffix##_values(                  \
        secs_ii_item_t *list, const c_type *values, size_t n) {                  \
        return guard_error([&]() -> secs_error_t {                               \
            if (!values && n != 0) {                                             \
                return c_api_err(SECS_C_API_INVALID_ARGUMENT);                   \
            }                                                                    \
            std::vector<c_type> out;                                             \
            if (n != 0) {                                                        \
                out.assign(values, values + n);                                  \
            }                                                                    \
            return ii_list_push(list, make_item(std::move(out)));                \
        });                                                                      \
    }

SECS_II_LIST_APPEND_VALUES_IMPL(int8_t, i1, secs::ii::Item::i1)
SECS_II_LIST_APPEND_VALUES_IMPL(int16_t, i2, secs::ii::Item::i2)
SECS_II_LIST_APPEND_VALUES_IMPL(int32_t, i4, secs::ii::Item::i4)
SECS_II_LIST_APPEND_VALUES_IMPL(int64_t, i8, secs::ii::Item::i8)
SECS_II_LIST_APPEND_VALUES_IMPL(uint8_t, u1, secs::ii::Item::u1)
SECS_II_LIST_APPEND_VALUES_IMPL(uint16_t, u2, secs::ii::Item::u2)
SECS_II_LIST_APPEND_VALUES_IMPL(uint32_t, u4, secs::ii::Item::u4)
SECS_II_LIST_APPEND_VALUES_IMPL(uint64_t, u8, secs::ii::Item::u8)
SECS_II_LIST_APPEND_VALUES_IMPL(float, f4, secs::ii::Item::f4)
SECS_II_LIST_APPEND_VALUES_IMPL(double, f8, secs::ii::Item::f8)

#undef SECS_II_LIST_APPEND_VALUES_IMPL

// ----------------------------- SECS-II：Item Builder（P1）
// -----------------------------

namespace {

[[nodiscard]] secs_error_t ii_builder_fail(secs_ii_builder_t *b,
                                          secs_error_t err) noexcept {
    if (!b) {
        return c_api_err(SECS_C_API_INVALID_ARGUMENT);
    }
    if (secs_error_is_ok(b->first_err)) {
        b->first_err = err;
    }
    return b->first_err;
}

[[nodiscard]] secs_error_t ii_builder_ok_or_err(secs_ii_builder_t *b) noexcept {
    if (!b) {
        return c_api_err(SECS_C_API_INVALID_ARGUMENT);
    }
    if (b->finalized) {
        return c_api_err(SECS_C_API_INVALID_ARGUMENT);
    }
    if (!secs_error_is_ok(b->first_err)) {
        return b->first_err;
    }
    return ok();
}

[[nodiscard]] secs_error_t ii_builder_append(secs_ii_builder_t *b,
                                            secs::ii::Item elem) noexcept {
    if (!b) {
        return c_api_err(SECS_C_API_INVALID_ARGUMENT);
    }
    if (b->finalized) {
        return c_api_err(SECS_C_API_INVALID_ARGUMENT);
    }
    if (!secs_error_is_ok(b->first_err)) {
        return b->first_err;
    }

    if (!b->list_stack.empty()) {
        auto *list = b->list_stack.back().get_if<secs::ii::List>();
        if (!list) {
            return ii_builder_fail(b, c_api_err(SECS_C_API_INVALID_ARGUMENT));
        }
        list->push_back(std::move(elem));
        return ok();
    }

    if (b->root.has_value()) {
        return ii_builder_fail(b, c_api_err(SECS_C_API_INVALID_ARGUMENT));
    }
    b->root = std::move(elem);
    return ok();
}

} // namespace

secs_error_t secs_ii_builder_create(secs_ii_builder_t **out_builder) {
    return guard_error([&]() -> secs_error_t {
        if (!out_builder) {
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        }
        *out_builder = nullptr;
        auto *b = new (std::nothrow) secs_ii_builder();
        if (!b) {
            return c_api_err(SECS_C_API_OUT_OF_MEMORY);
        }
        *out_builder = b;
        return ok();
    });
}

void secs_ii_builder_destroy(secs_ii_builder_t *builder) {
    guard_void([&]() {
        if (!builder) {
            return;
        }
        delete builder;
    });
}

secs_error_t secs_ii_builder_list_begin(secs_ii_builder_t *builder) {
    return guard_error([&]() -> secs_error_t {
        auto st = ii_builder_ok_or_err(builder);
        if (!secs_error_is_ok(st)) {
            return st;
        }
        builder->list_stack.push_back(secs::ii::Item::list({}));
        return ok();
    });
}

secs_error_t secs_ii_builder_list_end(secs_ii_builder_t *builder) {
    return guard_error([&]() -> secs_error_t {
        auto st = ii_builder_ok_or_err(builder);
        if (!secs_error_is_ok(st)) {
            return st;
        }
        if (builder->list_stack.empty()) {
            return ii_builder_fail(builder, c_api_err(SECS_C_API_INVALID_ARGUMENT));
        }
        secs::ii::Item finished = std::move(builder->list_stack.back());
        builder->list_stack.pop_back();
        return ii_builder_append(builder, std::move(finished));
    });
}

secs_error_t secs_ii_builder_add_item(secs_ii_builder_t *builder,
                                      const secs_ii_item_t *item) {
    return guard_error([&]() -> secs_error_t {
        auto st = ii_builder_ok_or_err(builder);
        if (!secs_error_is_ok(st)) {
            return st;
        }
        if (!item) {
            return ii_builder_fail(builder, c_api_err(SECS_C_API_INVALID_ARGUMENT));
        }
        return ii_builder_append(builder, item->item);
    });
}

secs_error_t secs_ii_builder_add_item_take(secs_ii_builder_t *builder,
                                           secs_ii_item_t **io_item) {
    return guard_error([&]() -> secs_error_t {
        if (!io_item || !*io_item) {
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        }

        // take 语义：确保无论成功/失败/抛异常都能销毁并置空。
        std::unique_ptr<secs_ii_item_t, void (*)(secs_ii_item_t *)> owned(
            *io_item, secs_ii_item_destroy);
        *io_item = nullptr;

        auto st = ii_builder_ok_or_err(builder);
        if (!secs_error_is_ok(st)) {
            return st;
        }

        return ii_builder_append(builder, std::move(owned->item));
    });
}

secs_error_t secs_ii_builder_add_ascii(secs_ii_builder_t *builder,
                                       const char *value) {
    return guard_error([&]() -> secs_error_t {
        auto st = ii_builder_ok_or_err(builder);
        if (!secs_error_is_ok(st)) {
            return st;
        }
        if (!value) {
            return ii_builder_fail(builder, c_api_err(SECS_C_API_INVALID_ARGUMENT));
        }
        return ii_builder_append(builder, secs::ii::Item::ascii(std::string(value)));
    });
}

secs_error_t secs_ii_builder_add_ascii_n(secs_ii_builder_t *builder,
                                         const char *bytes,
                                         size_t n) {
    return guard_error([&]() -> secs_error_t {
        auto st = ii_builder_ok_or_err(builder);
        if (!secs_error_is_ok(st)) {
            return st;
        }
        if (!bytes && n != 0) {
            return ii_builder_fail(builder, c_api_err(SECS_C_API_INVALID_ARGUMENT));
        }
        std::string s;
        if (n != 0) {
            s.assign(bytes, bytes + n);
        }
        return ii_builder_append(builder, secs::ii::Item::ascii(std::move(s)));
    });
}

secs_error_t secs_ii_builder_add_binary(secs_ii_builder_t *builder,
                                        const uint8_t *bytes,
                                        size_t n) {
    return guard_error([&]() -> secs_error_t {
        auto st = ii_builder_ok_or_err(builder);
        if (!secs_error_is_ok(st)) {
            return st;
        }
        if (!bytes && n != 0) {
            return ii_builder_fail(builder, c_api_err(SECS_C_API_INVALID_ARGUMENT));
        }
        return ii_builder_append(builder,
                                 secs::ii::Item::binary(bytes_to_vec(bytes, n)));
    });
}

secs_error_t secs_ii_builder_add_boolean(secs_ii_builder_t *builder,
                                         uint8_t value01) {
    return guard_error([&]() -> secs_error_t {
        auto st = ii_builder_ok_or_err(builder);
        if (!secs_error_is_ok(st)) {
            return st;
        }
        if (value01 != 0 && value01 != 1) {
            return ii_builder_fail(builder, c_api_err(SECS_C_API_INVALID_ARGUMENT));
        }
        std::vector<bool> v;
        v.push_back(value01 != 0);
        return ii_builder_append(builder, secs::ii::Item::boolean(std::move(v)));
    });
}

secs_error_t secs_ii_builder_add_boolean_values(secs_ii_builder_t *builder,
                                                const uint8_t *values01,
                                                size_t n) {
    return guard_error([&]() -> secs_error_t {
        auto st = ii_builder_ok_or_err(builder);
        if (!secs_error_is_ok(st)) {
            return st;
        }
        if (!values01 && n != 0) {
            return ii_builder_fail(builder, c_api_err(SECS_C_API_INVALID_ARGUMENT));
        }
        std::vector<bool> v;
        v.reserve(n);
        for (size_t i = 0; i < n; ++i) {
            if (values01[i] != 0 && values01[i] != 1) {
                return ii_builder_fail(builder,
                                       c_api_err(SECS_C_API_INVALID_ARGUMENT));
            }
            v.push_back(values01[i] != 0);
        }
        return ii_builder_append(builder, secs::ii::Item::boolean(std::move(v)));
    });
}

#define SECS_II_BUILDER_ADD_SCALAR_IMPL(c_type, fn_suffix, make_item)            \
    secs_error_t secs_ii_builder_add_##fn_suffix(secs_ii_builder_t *builder,     \
                                                 c_type value) {                \
        return guard_error([&]() -> secs_error_t {                               \
            auto st = ii_builder_ok_or_err(builder);                             \
            if (!secs_error_is_ok(st)) {                                        \
                return st;                                                       \
            }                                                                    \
            return ii_builder_append(builder, make_item({value}));               \
        });                                                                      \
    }

SECS_II_BUILDER_ADD_SCALAR_IMPL(int8_t, i1, secs::ii::Item::i1)
SECS_II_BUILDER_ADD_SCALAR_IMPL(int16_t, i2, secs::ii::Item::i2)
SECS_II_BUILDER_ADD_SCALAR_IMPL(int32_t, i4, secs::ii::Item::i4)
SECS_II_BUILDER_ADD_SCALAR_IMPL(int64_t, i8, secs::ii::Item::i8)
SECS_II_BUILDER_ADD_SCALAR_IMPL(uint8_t, u1, secs::ii::Item::u1)
SECS_II_BUILDER_ADD_SCALAR_IMPL(uint16_t, u2, secs::ii::Item::u2)
SECS_II_BUILDER_ADD_SCALAR_IMPL(uint32_t, u4, secs::ii::Item::u4)
SECS_II_BUILDER_ADD_SCALAR_IMPL(uint64_t, u8, secs::ii::Item::u8)
SECS_II_BUILDER_ADD_SCALAR_IMPL(float, f4, secs::ii::Item::f4)
SECS_II_BUILDER_ADD_SCALAR_IMPL(double, f8, secs::ii::Item::f8)

#undef SECS_II_BUILDER_ADD_SCALAR_IMPL

secs_error_t secs_ii_builder_finalize(secs_ii_builder_t *builder,
                                      secs_ii_item_t **out_item) {
    return guard_error([&]() -> secs_error_t {
        if (!out_item) {
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        }
        *out_item = nullptr;

        if (!builder) {
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        }
        if (builder->finalized) {
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        }
        if (!secs_error_is_ok(builder->first_err)) {
            return builder->first_err;
        }

        if (!builder->list_stack.empty()) {
            return ii_builder_fail(builder, c_api_err(SECS_C_API_INVALID_ARGUMENT));
        }
        if (!builder->root.has_value()) {
            return ii_builder_fail(builder, c_api_err(SECS_C_API_INVALID_ARGUMENT));
        }

        // 为了在 OOM 时不破坏 builder->root，这里先分配句柄，再 move root。
        std::unique_ptr<secs_ii_item_t, void (*)(secs_ii_item_t *)> owned(
            new (std::nothrow) secs_ii_item(secs::ii::Item::list({})),
            secs_ii_item_destroy);
        if (!owned) {
            return ii_builder_fail(builder, c_api_err(SECS_C_API_OUT_OF_MEMORY));
        }
        owned->item = std::move(*builder->root);
        builder->root.reset();
        builder->finalized = true;

        *out_item = owned.release();
        return ok();
    });
}

secs_error_t secs_ii_item_ascii_view(const secs_ii_item_t *item,
                                     const char **out_ptr,
                                     size_t *out_n) {
    return guard_error([&]() -> secs_error_t {
        if (!item || !out_ptr || !out_n)
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        const auto *v = item->item.get_if<secs::ii::ASCII>();
        if (!v)
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        *out_ptr = v->value.data();
        *out_n = v->value.size();
        return ok();
    });
}

secs_error_t secs_ii_item_binary_view(const secs_ii_item_t *item,
                                      const uint8_t **out_ptr,
                                      size_t *out_n) {
    return guard_error([&]() -> secs_error_t {
        if (!item || !out_ptr || !out_n)
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        const auto *v = item->item.get_if<secs::ii::Binary>();
        if (!v)
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        *out_ptr = reinterpret_cast<const uint8_t *>(v->value.data());
        *out_n = v->value.size();
        return ok();
    });
}

secs_error_t secs_ii_item_boolean_copy(const secs_ii_item_t *item,
                                       uint8_t **out_values01,
                                       size_t *out_n) {
    return guard_error([&]() -> secs_error_t {
        if (!item || !out_values01 || !out_n)
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        *out_values01 = nullptr;
        *out_n = 0;
        const auto *v = item->item.get_if<secs::ii::Boolean>();
        if (!v)
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);

        if (v->values.empty()) {
            return ok();
        }
        auto *buf = static_cast<uint8_t *>(secs_malloc(v->values.size()));
        if (!buf)
            return c_api_err(SECS_C_API_OUT_OF_MEMORY);
        for (size_t i = 0; i < v->values.size(); ++i) {
            buf[i] = v->values[i] ? 1u : 0u;
        }
        *out_values01 = buf;
        *out_n = v->values.size();
        return ok();
    });
}

#define SECS_II_VIEW_IMPL(c_type, cpp_type, tag)                               \
    secs_error_t secs_ii_item_##tag##_view(                                    \
        const secs_ii_item_t *item, const c_type **out_ptr, size_t *out_n) {   \
        if (!item || !out_ptr || !out_n)                                       \
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);                     \
        const auto *v = item->item.get_if<cpp_type>();                         \
        if (!v)                                                                \
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);                     \
        *out_ptr = reinterpret_cast<const c_type *>(v->values.data());         \
        *out_n = v->values.size();                                             \
        return ok();                                                           \
    }

SECS_II_VIEW_IMPL(int8_t, secs::ii::I1, i1)
SECS_II_VIEW_IMPL(int16_t, secs::ii::I2, i2)
SECS_II_VIEW_IMPL(int32_t, secs::ii::I4, i4)
SECS_II_VIEW_IMPL(int64_t, secs::ii::I8, i8)
SECS_II_VIEW_IMPL(uint8_t, secs::ii::U1, u1)
SECS_II_VIEW_IMPL(uint16_t, secs::ii::U2, u2)
SECS_II_VIEW_IMPL(uint32_t, secs::ii::U4, u4)
SECS_II_VIEW_IMPL(uint64_t, secs::ii::U8, u8)
SECS_II_VIEW_IMPL(float, secs::ii::F4, f4)
SECS_II_VIEW_IMPL(double, secs::ii::F8, f8)

#undef SECS_II_VIEW_IMPL

// ----------------------------- SECS-II：提取便捷 API（P1）
// -----------------------------

namespace {

[[nodiscard]] const secs::ii::Item *
ii_item_at_path_va(const secs_ii_item_t *root,
                   size_t depth,
                   va_list *ap,
                   secs_error_t &out_err) noexcept {
    if (!root) {
        out_err = c_api_err(SECS_C_API_INVALID_ARGUMENT);
        return nullptr;
    }
    if (!ap) {
        out_err = c_api_err(SECS_C_API_INVALID_ARGUMENT);
        return nullptr;
    }

    const secs::ii::Item *cur = &root->item;
    for (size_t i = 0; i < depth; ++i) {
        const size_t idx = va_arg(*ap, size_t);
        const auto *list = cur->get_if<secs::ii::List>();
        if (!list) {
            out_err = c_api_err(SECS_C_API_INVALID_ARGUMENT);
            return nullptr;
        }
        if (idx >= list->size()) {
            out_err = c_api_err(SECS_C_API_INVALID_ARGUMENT);
            return nullptr;
        }
        cur = &(*list)[idx];
    }

    out_err = ok();
    return cur;
}

[[nodiscard]] const secs::ii::Item *
ii_item_at_list_path(const secs_ii_item_t *root,
                     const size_t *indices,
                     size_t indices_n,
                     secs_error_t &out_err) noexcept {
    if (!root) {
        out_err = c_api_err(SECS_C_API_INVALID_ARGUMENT);
        return nullptr;
    }
    if (!indices && indices_n != 0) {
        out_err = c_api_err(SECS_C_API_INVALID_ARGUMENT);
        return nullptr;
    }

    const secs::ii::Item *cur = &root->item;
    for (size_t i = 0; i < indices_n; ++i) {
        const size_t idx = indices[i];
        const auto *list = cur->get_if<secs::ii::List>();
        if (!list) {
            out_err = c_api_err(SECS_C_API_INVALID_ARGUMENT);
            return nullptr;
        }
        if (idx >= list->size()) {
            out_err = c_api_err(SECS_C_API_INVALID_ARGUMENT);
            return nullptr;
        }
        cur = &(*list)[idx];
    }

    out_err = ok();
    return cur;
}

} // namespace

secs_error_t secs_ii_item_get_ascii_at(const secs_ii_item_t *list,
                                       size_t index,
                                       const char **out_ptr,
                                       size_t *out_n) {
    return guard_error([&]() -> secs_error_t {
        if (!list || !out_ptr || !out_n) {
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        }
        *out_ptr = nullptr;
        *out_n = 0;

        const auto *l = list->item.get_if<secs::ii::List>();
        if (!l) {
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        }
        if (index >= l->size()) {
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        }
        const auto *a = (*l)[index].get_if<secs::ii::ASCII>();
        if (!a) {
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        }
        *out_ptr = a->value.data();
        *out_n = a->value.size();
        return ok();
    });
}

// ---- view_*_at_path ---------------------------------------------------------

secs_error_t secs_ii_item_ascii_view_at_path(const secs_ii_item_t *root,
                                             const char **out_ptr,
                                             size_t *out_n,
                                             size_t depth,
                                             ...) {
    if (!root || !out_ptr || !out_n) {
        return c_api_err(SECS_C_API_INVALID_ARGUMENT);
    }
    *out_ptr = nullptr;
    *out_n = 0;

    va_list ap;
    va_start(ap, depth);
    const auto ret = guard_error([&]() -> secs_error_t {
        secs_error_t err = ok();
        const auto *item = ii_item_at_path_va(root, depth, &ap, err);
        if (!secs_error_is_ok(err) || !item) {
            return err;
        }

        const auto *a = item->get_if<secs::ii::ASCII>();
        if (!a) {
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        }
        *out_ptr = a->value.data();
        *out_n = a->value.size();
        return ok();
    });
    va_end(ap);
    return ret;
}

secs_error_t secs_ii_item_binary_view_at_path(const secs_ii_item_t *root,
                                              const uint8_t **out_ptr,
                                              size_t *out_n,
                                              size_t depth,
                                              ...) {
    if (!root || !out_ptr || !out_n) {
        return c_api_err(SECS_C_API_INVALID_ARGUMENT);
    }
    *out_ptr = nullptr;
    *out_n = 0;

    va_list ap;
    va_start(ap, depth);
    const auto ret = guard_error([&]() -> secs_error_t {
        secs_error_t err = ok();
        const auto *item = ii_item_at_path_va(root, depth, &ap, err);
        if (!secs_error_is_ok(err) || !item) {
            return err;
        }

        const auto *b = item->get_if<secs::ii::Binary>();
        if (!b) {
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        }
        *out_ptr = reinterpret_cast<const uint8_t *>(b->value.data());
        *out_n = b->value.size();
        return ok();
    });
    va_end(ap);
    return ret;
}

secs_error_t secs_ii_item_boolean_copy_at_path(const secs_ii_item_t *root,
                                               uint8_t **out_values01,
                                               size_t *out_n,
                                               size_t depth,
                                               ...) {
    if (!root || !out_values01 || !out_n) {
        return c_api_err(SECS_C_API_INVALID_ARGUMENT);
    }
    *out_values01 = nullptr;
    *out_n = 0;

    va_list ap;
    va_start(ap, depth);
    const auto ret = guard_error([&]() -> secs_error_t {
        secs_error_t err = ok();
        const auto *item = ii_item_at_path_va(root, depth, &ap, err);
        if (!secs_error_is_ok(err) || !item) {
            return err;
        }

        const auto *v = item->get_if<secs::ii::Boolean>();
        if (!v) {
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        }
        if (v->values.empty()) {
            return ok();
        }

        auto *buf = static_cast<uint8_t *>(secs_malloc(v->values.size()));
        if (!buf) {
            return c_api_err(SECS_C_API_OUT_OF_MEMORY);
        }
        for (size_t i = 0; i < v->values.size(); ++i) {
            buf[i] = v->values[i] ? 1u : 0u;
        }
        *out_values01 = buf;
        *out_n = v->values.size();
        return ok();
    });
    va_end(ap);
    return ret;
}

#define SECS_II_VIEW_AT_PATH_IMPL(c_type, cpp_type, tag)                         \
    secs_error_t secs_ii_item_##tag##_view_at_path(                              \
        const secs_ii_item_t *root,                                              \
        const c_type **out_ptr,                                                  \
        size_t *out_n,                                                           \
        size_t depth,                                                            \
        ...) {                                                                   \
        if (!root || !out_ptr || !out_n) {                                       \
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);                       \
        }                                                                        \
        *out_ptr = nullptr;                                                      \
        *out_n = 0;                                                              \
                                                                                \
        va_list ap;                                                              \
        va_start(ap, depth);                                                     \
        const auto ret = guard_error([&]() -> secs_error_t {                      \
            secs_error_t err = ok();                                             \
            const auto *item = ii_item_at_path_va(root, depth, &ap, err);        \
            if (!secs_error_is_ok(err) || !item) {                               \
                return err;                                                      \
            }                                                                    \
                                                                                \
            const auto *v = item->get_if<cpp_type>();                            \
            if (!v) {                                                            \
                return c_api_err(SECS_C_API_INVALID_ARGUMENT);                   \
            }                                                                    \
            *out_ptr = reinterpret_cast<const c_type *>(v->values.data());       \
            *out_n = v->values.size();                                           \
            return ok();                                                         \
        });                                                                      \
        va_end(ap);                                                              \
        return ret;                                                              \
    }

SECS_II_VIEW_AT_PATH_IMPL(int8_t, secs::ii::I1, i1)
SECS_II_VIEW_AT_PATH_IMPL(int16_t, secs::ii::I2, i2)
SECS_II_VIEW_AT_PATH_IMPL(int32_t, secs::ii::I4, i4)
SECS_II_VIEW_AT_PATH_IMPL(int64_t, secs::ii::I8, i8)
SECS_II_VIEW_AT_PATH_IMPL(uint8_t, secs::ii::U1, u1)
SECS_II_VIEW_AT_PATH_IMPL(uint16_t, secs::ii::U2, u2)
SECS_II_VIEW_AT_PATH_IMPL(uint32_t, secs::ii::U4, u4)
SECS_II_VIEW_AT_PATH_IMPL(uint64_t, secs::ii::U8, u8)
SECS_II_VIEW_AT_PATH_IMPL(float, secs::ii::F4, f4)
SECS_II_VIEW_AT_PATH_IMPL(double, secs::ii::F8, f8)

#undef SECS_II_VIEW_AT_PATH_IMPL

// ---- get_*_at_path（标量） ---------------------------------------------------

secs_error_t secs_ii_item_get_u2_at_path(const secs_ii_item_t *root,
                                        uint16_t *out_val,
                                        size_t depth,
                                        ...) {
    if (!root || !out_val) {
        return c_api_err(SECS_C_API_INVALID_ARGUMENT);
    }
    *out_val = 0;

    va_list ap;
    va_start(ap, depth);
    const auto ret = guard_error([&]() -> secs_error_t {
        secs_error_t err = ok();
        const auto *item = ii_item_at_path_va(root, depth, &ap, err);
        if (!secs_error_is_ok(err) || !item) {
            return err;
        }

        const auto *u2 = item->get_if<secs::ii::U2>();
        if (!u2 || u2->values.size() != 1) {
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        }
        *out_val = u2->values[0];
        return ok();
    });
    va_end(ap);
    return ret;
}

#define SECS_II_GET_SCALAR_AT_PATH_IMPL(c_type, cpp_type, tag, convert_expr)     \
    secs_error_t secs_ii_item_get_##tag##_at_path(const secs_ii_item_t *root,    \
                                                  c_type *out_val,              \
                                                  size_t depth,                 \
                                                  ...) {                        \
        if (!root || !out_val) {                                                 \
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);                       \
        }                                                                        \
        *out_val = {};                                                           \
                                                                                \
        va_list ap;                                                              \
        va_start(ap, depth);                                                     \
        const auto ret = guard_error([&]() -> secs_error_t {                      \
            secs_error_t err = ok();                                             \
            const auto *item = ii_item_at_path_va(root, depth, &ap, err);        \
            if (!secs_error_is_ok(err) || !item) {                               \
                return err;                                                      \
            }                                                                    \
                                                                                \
            const auto *v = item->get_if<cpp_type>();                            \
            if (!v || v->values.size() != 1) {                                   \
                return c_api_err(SECS_C_API_INVALID_ARGUMENT);                   \
            }                                                                    \
            *out_val = (convert_expr);                                           \
            return ok();                                                         \
        });                                                                      \
        va_end(ap);                                                              \
        return ret;                                                              \
    }

SECS_II_GET_SCALAR_AT_PATH_IMPL(int8_t, secs::ii::I1, i1, v->values[0])
SECS_II_GET_SCALAR_AT_PATH_IMPL(int16_t, secs::ii::I2, i2, v->values[0])
SECS_II_GET_SCALAR_AT_PATH_IMPL(int32_t, secs::ii::I4, i4, v->values[0])
SECS_II_GET_SCALAR_AT_PATH_IMPL(int64_t, secs::ii::I8, i8, v->values[0])
SECS_II_GET_SCALAR_AT_PATH_IMPL(uint8_t, secs::ii::U1, u1, v->values[0])
SECS_II_GET_SCALAR_AT_PATH_IMPL(uint32_t, secs::ii::U4, u4, v->values[0])
SECS_II_GET_SCALAR_AT_PATH_IMPL(uint64_t, secs::ii::U8, u8, v->values[0])
SECS_II_GET_SCALAR_AT_PATH_IMPL(float, secs::ii::F4, f4, v->values[0])
SECS_II_GET_SCALAR_AT_PATH_IMPL(double, secs::ii::F8, f8, v->values[0])

#undef SECS_II_GET_SCALAR_AT_PATH_IMPL

secs_error_t secs_ii_item_get_boolean_at_path(const secs_ii_item_t *root,
                                             uint8_t *out_val01,
                                             size_t depth,
                                             ...) {
    if (!root || !out_val01) {
        return c_api_err(SECS_C_API_INVALID_ARGUMENT);
    }
    *out_val01 = 0;

    va_list ap;
    va_start(ap, depth);
    const auto ret = guard_error([&]() -> secs_error_t {
        secs_error_t err = ok();
        const auto *item = ii_item_at_path_va(root, depth, &ap, err);
        if (!secs_error_is_ok(err) || !item) {
            return err;
        }

        const auto *v = item->get_if<secs::ii::Boolean>();
        if (!v || v->values.size() != 1) {
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        }
        *out_val01 = v->values[0] ? 1u : 0u;
        return ok();
    });
    va_end(ap);
    return ret;
}

// ---- *_at_list_path（array 版本，避免 C varargs UB） ---------------------------

secs_error_t secs_ii_item_ascii_view_at_list_path(const secs_ii_item_t *root,
                                                  const char **out_ptr,
                                                  size_t *out_n,
                                                  const size_t *indices,
                                                  size_t indices_n) {
    if (!root || !out_ptr || !out_n) {
        return c_api_err(SECS_C_API_INVALID_ARGUMENT);
    }
    *out_ptr = nullptr;
    *out_n = 0;

    return guard_error([&]() -> secs_error_t {
        secs_error_t err = ok();
        const auto *item = ii_item_at_list_path(root, indices, indices_n, err);
        if (!secs_error_is_ok(err) || !item) {
            return err;
        }

        const auto *a = item->get_if<secs::ii::ASCII>();
        if (!a) {
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        }
        *out_ptr = a->value.data();
        *out_n = a->value.size();
        return ok();
    });
}

secs_error_t secs_ii_item_binary_view_at_list_path(const secs_ii_item_t *root,
                                                   const uint8_t **out_ptr,
                                                   size_t *out_n,
                                                   const size_t *indices,
                                                   size_t indices_n) {
    if (!root || !out_ptr || !out_n) {
        return c_api_err(SECS_C_API_INVALID_ARGUMENT);
    }
    *out_ptr = nullptr;
    *out_n = 0;

    return guard_error([&]() -> secs_error_t {
        secs_error_t err = ok();
        const auto *item = ii_item_at_list_path(root, indices, indices_n, err);
        if (!secs_error_is_ok(err) || !item) {
            return err;
        }

        const auto *b = item->get_if<secs::ii::Binary>();
        if (!b) {
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        }
        *out_ptr = reinterpret_cast<const uint8_t *>(b->value.data());
        *out_n = b->value.size();
        return ok();
    });
}

secs_error_t secs_ii_item_boolean_copy_at_list_path(const secs_ii_item_t *root,
                                                    uint8_t **out_values01,
                                                    size_t *out_n,
                                                    const size_t *indices,
                                                    size_t indices_n) {
    if (!root || !out_values01 || !out_n) {
        return c_api_err(SECS_C_API_INVALID_ARGUMENT);
    }
    *out_values01 = nullptr;
    *out_n = 0;

    return guard_error([&]() -> secs_error_t {
        secs_error_t err = ok();
        const auto *item = ii_item_at_list_path(root, indices, indices_n, err);
        if (!secs_error_is_ok(err) || !item) {
            return err;
        }

        const auto *v = item->get_if<secs::ii::Boolean>();
        if (!v) {
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        }
        if (v->values.empty()) {
            return ok();
        }

        auto *buf = static_cast<uint8_t *>(secs_malloc(v->values.size()));
        if (!buf) {
            return c_api_err(SECS_C_API_OUT_OF_MEMORY);
        }
        for (size_t i = 0; i < v->values.size(); ++i) {
            buf[i] = v->values[i] ? 1u : 0u;
        }
        *out_values01 = buf;
        *out_n = v->values.size();
        return ok();
    });
}

#define SECS_II_VIEW_AT_LIST_PATH_IMPL(c_type, cpp_type, tag)                   \
    secs_error_t secs_ii_item_##tag##_view_at_list_path(                        \
        const secs_ii_item_t *root,                                             \
        const c_type **out_ptr,                                                 \
        size_t *out_n,                                                          \
        const size_t *indices,                                                  \
        size_t indices_n) {                                                     \
        if (!root || !out_ptr || !out_n) {                                      \
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);                      \
        }                                                                       \
        *out_ptr = nullptr;                                                     \
        *out_n = 0;                                                             \
                                                                                \
        return guard_error([&]() -> secs_error_t {                               \
            secs_error_t err = ok();                                            \
            const auto *item =                                                  \
                ii_item_at_list_path(root, indices, indices_n, err);            \
            if (!secs_error_is_ok(err) || !item) {                              \
                return err;                                                     \
            }                                                                   \
                                                                                \
            const auto *v = item->get_if<cpp_type>();                           \
            if (!v) {                                                           \
                return c_api_err(SECS_C_API_INVALID_ARGUMENT);                  \
            }                                                                   \
            *out_ptr = reinterpret_cast<const c_type *>(v->values.data());      \
            *out_n = v->values.size();                                          \
            return ok();                                                        \
        });                                                                     \
    }

SECS_II_VIEW_AT_LIST_PATH_IMPL(int8_t, secs::ii::I1, i1)
SECS_II_VIEW_AT_LIST_PATH_IMPL(int16_t, secs::ii::I2, i2)
SECS_II_VIEW_AT_LIST_PATH_IMPL(int32_t, secs::ii::I4, i4)
SECS_II_VIEW_AT_LIST_PATH_IMPL(int64_t, secs::ii::I8, i8)
SECS_II_VIEW_AT_LIST_PATH_IMPL(uint8_t, secs::ii::U1, u1)
SECS_II_VIEW_AT_LIST_PATH_IMPL(uint16_t, secs::ii::U2, u2)
SECS_II_VIEW_AT_LIST_PATH_IMPL(uint32_t, secs::ii::U4, u4)
SECS_II_VIEW_AT_LIST_PATH_IMPL(uint64_t, secs::ii::U8, u8)
SECS_II_VIEW_AT_LIST_PATH_IMPL(float, secs::ii::F4, f4)
SECS_II_VIEW_AT_LIST_PATH_IMPL(double, secs::ii::F8, f8)

#undef SECS_II_VIEW_AT_LIST_PATH_IMPL

secs_error_t secs_ii_item_get_u2_at_list_path(const secs_ii_item_t *root,
                                             uint16_t *out_val,
                                             const size_t *indices,
                                             size_t indices_n) {
    if (!root || !out_val) {
        return c_api_err(SECS_C_API_INVALID_ARGUMENT);
    }
    *out_val = 0;

    return guard_error([&]() -> secs_error_t {
        secs_error_t err = ok();
        const auto *item = ii_item_at_list_path(root, indices, indices_n, err);
        if (!secs_error_is_ok(err) || !item) {
            return err;
        }

        const auto *u2 = item->get_if<secs::ii::U2>();
        if (!u2 || u2->values.size() != 1) {
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        }
        *out_val = u2->values[0];
        return ok();
    });
}

#define SECS_II_GET_SCALAR_AT_LIST_PATH_IMPL(c_type, cpp_type, tag, convert_expr) \
    secs_error_t secs_ii_item_get_##tag##_at_list_path(                          \
        const secs_ii_item_t *root,                                              \
        c_type *out_val,                                                         \
        const size_t *indices,                                                   \
        size_t indices_n) {                                                      \
        if (!root || !out_val) {                                                 \
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);                       \
        }                                                                        \
        *out_val = {};                                                           \
                                                                                 \
        return guard_error([&]() -> secs_error_t {                                \
            secs_error_t err = ok();                                             \
            const auto *item =                                                   \
                ii_item_at_list_path(root, indices, indices_n, err);             \
            if (!secs_error_is_ok(err) || !item) {                               \
                return err;                                                      \
            }                                                                    \
                                                                                 \
            const auto *v = item->get_if<cpp_type>();                            \
            if (!v || v->values.size() != 1) {                                   \
                return c_api_err(SECS_C_API_INVALID_ARGUMENT);                   \
            }                                                                    \
            *out_val = (convert_expr);                                           \
            return ok();                                                         \
        });                                                                      \
    }

SECS_II_GET_SCALAR_AT_LIST_PATH_IMPL(int8_t, secs::ii::I1, i1, v->values[0])
SECS_II_GET_SCALAR_AT_LIST_PATH_IMPL(int16_t, secs::ii::I2, i2, v->values[0])
SECS_II_GET_SCALAR_AT_LIST_PATH_IMPL(int32_t, secs::ii::I4, i4, v->values[0])
SECS_II_GET_SCALAR_AT_LIST_PATH_IMPL(int64_t, secs::ii::I8, i8, v->values[0])
SECS_II_GET_SCALAR_AT_LIST_PATH_IMPL(uint8_t, secs::ii::U1, u1, v->values[0])
SECS_II_GET_SCALAR_AT_LIST_PATH_IMPL(uint32_t, secs::ii::U4, u4, v->values[0])
SECS_II_GET_SCALAR_AT_LIST_PATH_IMPL(uint64_t, secs::ii::U8, u8, v->values[0])
SECS_II_GET_SCALAR_AT_LIST_PATH_IMPL(float, secs::ii::F4, f4, v->values[0])
SECS_II_GET_SCALAR_AT_LIST_PATH_IMPL(double, secs::ii::F8, f8, v->values[0])

#undef SECS_II_GET_SCALAR_AT_LIST_PATH_IMPL

secs_error_t secs_ii_item_get_boolean_at_list_path(const secs_ii_item_t *root,
                                                  uint8_t *out_val01,
                                                  const size_t *indices,
                                                  size_t indices_n) {
    if (!root || !out_val01) {
        return c_api_err(SECS_C_API_INVALID_ARGUMENT);
    }
    *out_val01 = 0;

    return guard_error([&]() -> secs_error_t {
        secs_error_t err = ok();
        const auto *item = ii_item_at_list_path(root, indices, indices_n, err);
        if (!secs_error_is_ok(err) || !item) {
            return err;
        }

        const auto *v = item->get_if<secs::ii::Boolean>();
        if (!v || v->values.size() != 1) {
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        }
        *out_val01 = v->values[0] ? 1u : 0u;
        return ok();
    });
}

secs_error_t
secs_ii_encode(const secs_ii_item_t *item, uint8_t **out_bytes, size_t *out_n) {
    return guard_error([&]() -> secs_error_t {
        if (!item || !out_bytes || !out_n)
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        *out_bytes = nullptr;
        *out_n = 0;

        std::vector<byte> out;
        const auto ec = secs::ii::encode(item->item, out);
        if (ec) {
            return from_error_code(ec);
        }
        if (out.empty()) {
            return ok();
        }

        auto *buf = static_cast<uint8_t *>(secs_malloc(out.size()));
        if (!buf) {
            return c_api_err(SECS_C_API_OUT_OF_MEMORY);
        }
        std::memcpy(buf, out.data(), out.size());
        *out_bytes = buf;
        *out_n = out.size();
        return ok();
    });
}

secs_error_t secs_ii_decode_one(const uint8_t *in_bytes,
                                size_t in_n,
                                size_t *out_consumed,
                                secs_ii_item_t **out_item) {
    return guard_error([&]() -> secs_error_t {
        if (!in_bytes && in_n != 0)
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        if (!out_consumed || !out_item)
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        *out_consumed = 0;
        *out_item = nullptr;

        secs::ii::Item decoded{secs::ii::List{}};
        std::size_t consumed = 0;
        const auto ec = secs::ii::decode_one(
            bytes_view{reinterpret_cast<const byte *>(in_bytes), in_n},
            decoded,
            consumed);
        if (ec) {
            return from_error_code(ec);
        }

        auto *h = new (std::nothrow) secs_ii_item(std::move(decoded));
        if (!h) {
            return c_api_err(SECS_C_API_OUT_OF_MEMORY);
        }
        *out_consumed = consumed;
        *out_item = h;
        return ok();
    });
}

void secs_ii_decode_limits_init_default(secs_ii_decode_limits_t *out_limits) {
    guard_void([&]() {
        if (!out_limits) {
            return;
        }
        const secs::ii::DecodeLimits d{};
        out_limits->max_depth = d.max_depth;
        out_limits->max_list_items = d.max_list_items;
        out_limits->max_payload_bytes = d.max_payload_bytes;
        out_limits->max_total_items = d.max_total_items;
        out_limits->max_total_bytes = d.max_total_bytes;
    });
}

secs_error_t secs_ii_decode_one_with_limits(const uint8_t *in_bytes,
                                            size_t in_n,
                                            const secs_ii_decode_limits_t *limits,
                                            size_t *out_consumed,
                                            secs_ii_item_t **out_item) {
    return guard_error([&]() -> secs_error_t {
        if (!in_bytes && in_n != 0) {
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        }
        if (!out_consumed || !out_item) {
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        }
        *out_consumed = 0;
        *out_item = nullptr;

        secs::ii::Item decoded{secs::ii::List{}};
        std::size_t consumed = 0;
        const auto ec = secs::ii::decode_one(
            bytes_view{reinterpret_cast<const byte *>(in_bytes), in_n},
            decoded,
            consumed,
            make_decode_limits(limits));
        if (ec) {
            return from_error_code(ec);
        }

        auto *h = new (std::nothrow) secs_ii_item(std::move(decoded));
        if (!h) {
            return c_api_err(SECS_C_API_OUT_OF_MEMORY);
        }
        *out_consumed = consumed;
        *out_item = h;
        return ok();
    });
}

