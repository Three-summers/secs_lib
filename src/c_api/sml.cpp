#include "c_api/internal.hpp"

// ----------------------------- SML 运行时 -----------------------------

secs_error_t secs_sml_runtime_create(secs_sml_runtime_t **out_rt) {
    return guard_error([&]() -> secs_error_t {
        if (!out_rt)
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        *out_rt = nullptr;

        auto *rt = new (std::nothrow) secs_sml_runtime{};
        if (!rt)
            return c_api_err(SECS_C_API_OUT_OF_MEMORY);
        *out_rt = rt;
        return ok();
    });
}

void secs_sml_runtime_destroy(secs_sml_runtime_t *rt) {
    guard_void([&]() { delete rt; });
}

secs_error_t secs_sml_runtime_load(secs_sml_runtime_t *rt,
                                   const char *source,
                                   size_t source_n) {
    return guard_error([&]() -> secs_error_t {
        if (!rt)
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        if (!source && source_n != 0)
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        const std::string_view sv{source ? source : "", source ? source_n : 0};
        return from_error_code(rt->rt.load(sv));
    });
}

secs_error_t secs_sml_runtime_load_cstr(secs_sml_runtime_t *rt,
                                        const char *source) {
    return guard_error([&]() -> secs_error_t {
        if (!rt || !source) {
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        }
        return secs_sml_runtime_load(rt, source, std::strlen(source));
    });
}

secs_error_t secs_sml_runtime_match_response(const secs_sml_runtime_t *rt,
                                             uint8_t stream,
                                             uint8_t function,
                                             const uint8_t *body_bytes,
                                             size_t body_n,
                                             char **out_name) {
    return guard_error([&]() -> secs_error_t {
        if (!rt || !out_name)
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        if (!body_bytes && body_n != 0)
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        *out_name = nullptr;

        secs::ii::Item decoded{secs::ii::List{}};
        std::size_t consumed = 0;
        if (body_n != 0) {
            auto dec_ec = secs::ii::decode_one(
                bytes_view{reinterpret_cast<const byte *>(body_bytes), body_n},
                decoded,
                consumed);
            if (dec_ec) {
                return from_error_code(dec_ec);
            }
        }

        auto matched = rt->rt.match_response(stream, function, decoded);
        if (!matched.has_value()) {
            return ok();
        }

        auto *s = static_cast<char *>(secs_malloc(matched->size() + 1));
        if (!s) {
            return c_api_err(SECS_C_API_OUT_OF_MEMORY);
        }
        std::memcpy(s, matched->data(), matched->size());
        s[matched->size()] = '\0';
        *out_name = s;
        return ok();
    });
}

secs_error_t
secs_sml_runtime_get_message_body_by_name(const secs_sml_runtime_t *rt,
                                          const char *name,
                                          uint8_t **out_body_bytes,
                                          size_t *out_body_n,
                                          uint8_t *out_stream,
                                          uint8_t *out_function,
                                          int *out_w_bit) {
    return guard_error([&]() -> secs_error_t {
        if (!rt || !name || !out_body_bytes || !out_body_n)
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        *out_body_bytes = nullptr;
        *out_body_n = 0;

        const auto *msg = rt->rt.get_message(std::string_view{name});
        if (!msg) {
            return c_api_err(SECS_C_API_NOT_FOUND);
        }

        // C API 暂不提供“注入变量”的接口：这里使用空上下文渲染。
        // 若消息模板包含占位符，将返回 sml.render/missing_variable。
        secs::sml::RenderContext ctx{};
        secs::ii::Item rendered{secs::ii::List{}};
        const auto render_ec = secs::sml::render_item(msg->item, ctx, rendered);
        if (render_ec) {
            return from_error_code(render_ec);
        }

        std::vector<byte> out;
        auto ec = secs::ii::encode(rendered, out);
        if (ec) {
            return from_error_code(ec);
        }

        if (!out.empty()) {
            auto *buf = static_cast<uint8_t *>(secs_malloc(out.size()));
            if (!buf)
                return c_api_err(SECS_C_API_OUT_OF_MEMORY);
            std::memcpy(buf, out.data(), out.size());
            *out_body_bytes = buf;
            *out_body_n = out.size();
        }

        if (out_stream)
            *out_stream = msg->stream;
        if (out_function)
            *out_function = msg->function;
        if (out_w_bit)
            *out_w_bit = msg->w_bit ? 1 : 0;
        return ok();
    });
}

// ----------------------------- SML RenderContext -----------------------------

namespace {

static bool sml_render_ctx_short_circuit(secs_sml_render_context_t *ctx,
                                        secs_error_t &out) noexcept {
    if (!ctx) {
        out = c_api_err(SECS_C_API_INVALID_ARGUMENT);
        return true;
    }
    if (ctx->sticky_enabled && !secs_error_is_ok(ctx->sticky_err)) {
        out = ctx->sticky_err;
        return true;
    }
    return false;
}

static secs_error_t sml_render_ctx_remember(secs_sml_render_context_t *ctx,
                                           secs_error_t err) noexcept {
    if (!ctx) {
        return err;
    }
    if (ctx->sticky_enabled && secs_error_is_ok(ctx->sticky_err) &&
        !secs_error_is_ok(err)) {
        ctx->sticky_err = err;
    }
    return err;
}

} // namespace

secs_error_t secs_sml_render_context_create(secs_sml_render_context_t **out_ctx) {
    return guard_error([&]() -> secs_error_t {
        if (!out_ctx) {
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        }
        *out_ctx = nullptr;

        auto *ctx = new (std::nothrow) secs_sml_render_context{};
        if (!ctx) {
            return c_api_err(SECS_C_API_OUT_OF_MEMORY);
        }
        *out_ctx = ctx;
        return ok();
    });
}

void secs_sml_render_context_destroy(secs_sml_render_context_t *ctx) {
    guard_void([&]() { delete ctx; });
}

void secs_sml_render_context_clear(secs_sml_render_context_t *ctx) {
    guard_void([&]() {
        if (!ctx) {
            return;
        }
        ctx->ctx.clear();
        ctx->sticky_err = ok();
    });
}

secs_error_t secs_sml_render_context_begin(secs_sml_render_context_t *ctx) {
    return guard_error([&]() -> secs_error_t {
        if (!ctx) {
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        }
        ctx->sticky_enabled = true;
        ctx->sticky_err = ok();
        return ok();
    });
}

secs_error_t secs_sml_render_context_end(secs_sml_render_context_t *ctx) {
    return guard_error([&]() -> secs_error_t {
        if (!ctx) {
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        }
        const auto err = ctx->sticky_err;
        ctx->sticky_enabled = false;
        ctx->sticky_err = ok();
        return err;
    });
}

secs_error_t secs_sml_render_context_set(secs_sml_render_context_t *ctx,
                                         const char *name,
                                         const secs_ii_item_t *value) {
    return guard_error([&]() -> secs_error_t {
        secs_error_t sticky{};
        if (sml_render_ctx_short_circuit(ctx, sticky)) {
            return sticky;
        }
        if (!name || !value) {
            return sml_render_ctx_remember(ctx,
                                           c_api_err(SECS_C_API_INVALID_ARGUMENT));
        }
        try {
            ctx->ctx.set(std::string{name}, value->item);
        } catch (const std::bad_alloc &) {
            return sml_render_ctx_remember(ctx,
                                           c_api_err(SECS_C_API_OUT_OF_MEMORY));
        } catch (...) {
            return sml_render_ctx_remember(ctx, c_api_err(SECS_C_API_EXCEPTION));
        }
        return ok();
    });
}

secs_error_t secs_sml_render_context_set_ascii(secs_sml_render_context_t *ctx,
                                               const char *name,
                                               const char *value) {
    return guard_error([&]() -> secs_error_t {
        secs_error_t sticky{};
        if (sml_render_ctx_short_circuit(ctx, sticky)) {
            return sticky;
        }
        if (!name || !value) {
            return sml_render_ctx_remember(ctx,
                                           c_api_err(SECS_C_API_INVALID_ARGUMENT));
        }

        secs_ii_item_t *tmp = nullptr;
        const auto err =
            secs_ii_item_create_ascii(value, std::strlen(value), &tmp);
        if (!secs_error_is_ok(err)) {
            secs_ii_item_destroy(tmp);
            return sml_render_ctx_remember(ctx, err);
        }

        const auto set_err = secs_sml_render_context_set(ctx, name, tmp);
        secs_ii_item_destroy(tmp);
        return set_err;
    });
}

secs_error_t secs_sml_render_context_set_binary(secs_sml_render_context_t *ctx,
                                                const char *name,
                                                const uint8_t *bytes,
                                                size_t n) {
    return guard_error([&]() -> secs_error_t {
        secs_error_t sticky{};
        if (sml_render_ctx_short_circuit(ctx, sticky)) {
            return sticky;
        }
        if (!name) {
            return sml_render_ctx_remember(ctx,
                                           c_api_err(SECS_C_API_INVALID_ARGUMENT));
        }
        if (!bytes && n != 0) {
            return sml_render_ctx_remember(ctx,
                                           c_api_err(SECS_C_API_INVALID_ARGUMENT));
        }

        secs_ii_item_t *tmp = nullptr;
        const auto err = secs_ii_item_create_binary(bytes, n, &tmp);
        if (!secs_error_is_ok(err)) {
            secs_ii_item_destroy(tmp);
            return sml_render_ctx_remember(ctx, err);
        }

        const auto set_err = secs_sml_render_context_set(ctx, name, tmp);
        secs_ii_item_destroy(tmp);
        return set_err;
    });
}

secs_error_t secs_sml_render_context_set_boolean(secs_sml_render_context_t *ctx,
                                                 const char *name,
                                                 uint8_t value01) {
    return guard_error([&]() -> secs_error_t {
        secs_error_t sticky{};
        if (sml_render_ctx_short_circuit(ctx, sticky)) {
            return sticky;
        }
        if (!name) {
            return sml_render_ctx_remember(ctx,
                                           c_api_err(SECS_C_API_INVALID_ARGUMENT));
        }
        if (value01 != 0 && value01 != 1) {
            return sml_render_ctx_remember(ctx,
                                           c_api_err(SECS_C_API_INVALID_ARGUMENT));
        }

        secs_ii_item_t *tmp = nullptr;
        const auto err = secs_ii_item_create_boolean(&value01, 1, &tmp);
        if (!secs_error_is_ok(err)) {
            secs_ii_item_destroy(tmp);
            return sml_render_ctx_remember(ctx, err);
        }

        const auto set_err = secs_sml_render_context_set(ctx, name, tmp);
        secs_ii_item_destroy(tmp);
        return set_err;
    });
}

secs_error_t secs_sml_render_context_set_i1(secs_sml_render_context_t *ctx,
                                            const char *name,
                                            int8_t value) {
    return guard_error([&]() -> secs_error_t {
        secs_error_t sticky{};
        if (sml_render_ctx_short_circuit(ctx, sticky)) {
            return sticky;
        }
        if (!name) {
            return sml_render_ctx_remember(ctx,
                                           c_api_err(SECS_C_API_INVALID_ARGUMENT));
        }

        secs_ii_item_t *tmp = nullptr;
        const auto err = secs_ii_item_create_i1(&value, 1, &tmp);
        if (!secs_error_is_ok(err)) {
            secs_ii_item_destroy(tmp);
            return sml_render_ctx_remember(ctx, err);
        }

        const auto set_err = secs_sml_render_context_set(ctx, name, tmp);
        secs_ii_item_destroy(tmp);
        return set_err;
    });
}

secs_error_t secs_sml_render_context_set_i2(secs_sml_render_context_t *ctx,
                                            const char *name,
                                            int16_t value) {
    return guard_error([&]() -> secs_error_t {
        secs_error_t sticky{};
        if (sml_render_ctx_short_circuit(ctx, sticky)) {
            return sticky;
        }
        if (!name) {
            return sml_render_ctx_remember(ctx,
                                           c_api_err(SECS_C_API_INVALID_ARGUMENT));
        }

        secs_ii_item_t *tmp = nullptr;
        const auto err = secs_ii_item_create_i2(&value, 1, &tmp);
        if (!secs_error_is_ok(err)) {
            secs_ii_item_destroy(tmp);
            return sml_render_ctx_remember(ctx, err);
        }

        const auto set_err = secs_sml_render_context_set(ctx, name, tmp);
        secs_ii_item_destroy(tmp);
        return set_err;
    });
}

secs_error_t secs_sml_render_context_set_i4(secs_sml_render_context_t *ctx,
                                            const char *name,
                                            int32_t value) {
    return guard_error([&]() -> secs_error_t {
        secs_error_t sticky{};
        if (sml_render_ctx_short_circuit(ctx, sticky)) {
            return sticky;
        }
        if (!name) {
            return sml_render_ctx_remember(ctx,
                                           c_api_err(SECS_C_API_INVALID_ARGUMENT));
        }

        secs_ii_item_t *tmp = nullptr;
        const auto err = secs_ii_item_create_i4(&value, 1, &tmp);
        if (!secs_error_is_ok(err)) {
            secs_ii_item_destroy(tmp);
            return sml_render_ctx_remember(ctx, err);
        }

        const auto set_err = secs_sml_render_context_set(ctx, name, tmp);
        secs_ii_item_destroy(tmp);
        return set_err;
    });
}

secs_error_t secs_sml_render_context_set_i8(secs_sml_render_context_t *ctx,
                                            const char *name,
                                            int64_t value) {
    return guard_error([&]() -> secs_error_t {
        secs_error_t sticky{};
        if (sml_render_ctx_short_circuit(ctx, sticky)) {
            return sticky;
        }
        if (!name) {
            return sml_render_ctx_remember(ctx,
                                           c_api_err(SECS_C_API_INVALID_ARGUMENT));
        }

        secs_ii_item_t *tmp = nullptr;
        const auto err = secs_ii_item_create_i8(&value, 1, &tmp);
        if (!secs_error_is_ok(err)) {
            secs_ii_item_destroy(tmp);
            return sml_render_ctx_remember(ctx, err);
        }

        const auto set_err = secs_sml_render_context_set(ctx, name, tmp);
        secs_ii_item_destroy(tmp);
        return set_err;
    });
}

secs_error_t secs_sml_render_context_set_u1(secs_sml_render_context_t *ctx,
                                            const char *name,
                                            uint8_t value) {
    return guard_error([&]() -> secs_error_t {
        secs_error_t sticky{};
        if (sml_render_ctx_short_circuit(ctx, sticky)) {
            return sticky;
        }
        if (!name) {
            return sml_render_ctx_remember(ctx,
                                           c_api_err(SECS_C_API_INVALID_ARGUMENT));
        }

        secs_ii_item_t *tmp = nullptr;
        const auto err = secs_ii_item_create_u1(&value, 1, &tmp);
        if (!secs_error_is_ok(err)) {
            secs_ii_item_destroy(tmp);
            return sml_render_ctx_remember(ctx, err);
        }

        const auto set_err = secs_sml_render_context_set(ctx, name, tmp);
        secs_ii_item_destroy(tmp);
        return set_err;
    });
}

secs_error_t secs_sml_render_context_set_u2(secs_sml_render_context_t *ctx,
                                            const char *name,
                                            uint16_t value) {
    return guard_error([&]() -> secs_error_t {
        secs_error_t sticky{};
        if (sml_render_ctx_short_circuit(ctx, sticky)) {
            return sticky;
        }
        if (!name) {
            return sml_render_ctx_remember(ctx,
                                           c_api_err(SECS_C_API_INVALID_ARGUMENT));
        }

        secs_ii_item_t *tmp = nullptr;
        const auto err = secs_ii_item_create_u2(&value, 1, &tmp);
        if (!secs_error_is_ok(err)) {
            secs_ii_item_destroy(tmp);
            return sml_render_ctx_remember(ctx, err);
        }

        const auto set_err = secs_sml_render_context_set(ctx, name, tmp);
        secs_ii_item_destroy(tmp);
        return set_err;
    });
}

secs_error_t secs_sml_render_context_set_u4(secs_sml_render_context_t *ctx,
                                            const char *name,
                                            uint32_t value) {
    return guard_error([&]() -> secs_error_t {
        secs_error_t sticky{};
        if (sml_render_ctx_short_circuit(ctx, sticky)) {
            return sticky;
        }
        if (!name) {
            return sml_render_ctx_remember(ctx,
                                           c_api_err(SECS_C_API_INVALID_ARGUMENT));
        }

        secs_ii_item_t *tmp = nullptr;
        const auto err = secs_ii_item_create_u4(&value, 1, &tmp);
        if (!secs_error_is_ok(err)) {
            secs_ii_item_destroy(tmp);
            return sml_render_ctx_remember(ctx, err);
        }

        const auto set_err = secs_sml_render_context_set(ctx, name, tmp);
        secs_ii_item_destroy(tmp);
        return set_err;
    });
}

secs_error_t secs_sml_render_context_set_u8(secs_sml_render_context_t *ctx,
                                            const char *name,
                                            uint64_t value) {
    return guard_error([&]() -> secs_error_t {
        secs_error_t sticky{};
        if (sml_render_ctx_short_circuit(ctx, sticky)) {
            return sticky;
        }
        if (!name) {
            return sml_render_ctx_remember(ctx,
                                           c_api_err(SECS_C_API_INVALID_ARGUMENT));
        }

        secs_ii_item_t *tmp = nullptr;
        const auto err = secs_ii_item_create_u8(&value, 1, &tmp);
        if (!secs_error_is_ok(err)) {
            secs_ii_item_destroy(tmp);
            return sml_render_ctx_remember(ctx, err);
        }

        const auto set_err = secs_sml_render_context_set(ctx, name, tmp);
        secs_ii_item_destroy(tmp);
        return set_err;
    });
}

secs_error_t secs_sml_render_context_set_f4(secs_sml_render_context_t *ctx,
                                            const char *name,
                                            float value) {
    return guard_error([&]() -> secs_error_t {
        secs_error_t sticky{};
        if (sml_render_ctx_short_circuit(ctx, sticky)) {
            return sticky;
        }
        if (!name) {
            return sml_render_ctx_remember(ctx,
                                           c_api_err(SECS_C_API_INVALID_ARGUMENT));
        }

        secs_ii_item_t *tmp = nullptr;
        const auto err = secs_ii_item_create_f4(&value, 1, &tmp);
        if (!secs_error_is_ok(err)) {
            secs_ii_item_destroy(tmp);
            return sml_render_ctx_remember(ctx, err);
        }

        const auto set_err = secs_sml_render_context_set(ctx, name, tmp);
        secs_ii_item_destroy(tmp);
        return set_err;
    });
}

secs_error_t secs_sml_render_context_set_f8(secs_sml_render_context_t *ctx,
                                            const char *name,
                                            double value) {
    return guard_error([&]() -> secs_error_t {
        secs_error_t sticky{};
        if (sml_render_ctx_short_circuit(ctx, sticky)) {
            return sticky;
        }
        if (!name) {
            return sml_render_ctx_remember(ctx,
                                           c_api_err(SECS_C_API_INVALID_ARGUMENT));
        }

        secs_ii_item_t *tmp = nullptr;
        const auto err = secs_ii_item_create_f8(&value, 1, &tmp);
        if (!secs_error_is_ok(err)) {
            secs_ii_item_destroy(tmp);
            return sml_render_ctx_remember(ctx, err);
        }

        const auto set_err = secs_sml_render_context_set(ctx, name, tmp);
        secs_ii_item_destroy(tmp);
        return set_err;
    });
}

secs_error_t secs_sml_render_context_get(const secs_sml_render_context_t *ctx,
                                         const char *name,
                                         secs_ii_item_t **out_value) {
    return guard_error([&]() -> secs_error_t {
        if (!ctx || !name || !out_value) {
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        }
        *out_value = nullptr;

        const auto *v = ctx->ctx.get(std::string_view{name});
        if (!v) {
            return c_api_err(SECS_C_API_NOT_FOUND);
        }

        auto *h = new (std::nothrow) secs_ii_item(*v);
        if (!h) {
            return c_api_err(SECS_C_API_OUT_OF_MEMORY);
        }
        *out_value = h;
        return ok();
    });
}

secs_error_t secs_sml_render_context_get_ascii_view(const secs_sml_render_context_t *ctx,
                                                    const char *name,
                                                    const char **out_ptr,
                                                    size_t *out_n) {
    return guard_error([&]() -> secs_error_t {
        if (!ctx || !name || !out_ptr || !out_n) {
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        }
        *out_ptr = nullptr;
        *out_n = 0;

        const auto *item = ctx->ctx.get(std::string_view{name});
        if (!item) {
            return c_api_err(SECS_C_API_NOT_FOUND);
        }
        const auto *v = item->get_if<secs::ii::ASCII>();
        if (!v) {
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        }
        *out_ptr = v->value.data();
        *out_n = v->value.size();
        return ok();
    });
}

secs_error_t secs_sml_render_context_get_binary_view(const secs_sml_render_context_t *ctx,
                                                     const char *name,
                                                     const uint8_t **out_ptr,
                                                     size_t *out_n) {
    return guard_error([&]() -> secs_error_t {
        if (!ctx || !name || !out_ptr || !out_n) {
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        }
        *out_ptr = nullptr;
        *out_n = 0;

        const auto *item = ctx->ctx.get(std::string_view{name});
        if (!item) {
            return c_api_err(SECS_C_API_NOT_FOUND);
        }
        const auto *v = item->get_if<secs::ii::Binary>();
        if (!v) {
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        }
        *out_ptr = reinterpret_cast<const uint8_t *>(v->value.data());
        *out_n = v->value.size();
        return ok();
    });
}

#define SECS_SML_RENDER_CONTEXT_GET_SCALAR_IMPL(c_type, cpp_type, tag, convert_expr) \
    secs_error_t secs_sml_render_context_get_##tag(                                  \
        const secs_sml_render_context_t *ctx, const char *name, c_type *out_value) { \
        return guard_error([&]() -> secs_error_t {                                    \
            if (!ctx || !name || !out_value) {                                        \
                return c_api_err(SECS_C_API_INVALID_ARGUMENT);                        \
            }                                                                         \
            *out_value = {};                                                          \
                                                                                      \
            const auto *item = ctx->ctx.get(std::string_view{name});                  \
            if (!item) {                                                              \
                return c_api_err(SECS_C_API_NOT_FOUND);                               \
            }                                                                         \
            const auto *v = item->get_if<cpp_type>();                                 \
            if (!v || v->values.size() != 1) {                                        \
                return c_api_err(SECS_C_API_INVALID_ARGUMENT);                        \
            }                                                                         \
            *out_value = (convert_expr);                                              \
            return ok();                                                              \
        });                                                                           \
    }

SECS_SML_RENDER_CONTEXT_GET_SCALAR_IMPL(uint8_t,
                                       secs::ii::Boolean,
                                       boolean,
                                       v->values[0] ? 1u : 0u)
SECS_SML_RENDER_CONTEXT_GET_SCALAR_IMPL(int8_t, secs::ii::I1, i1, v->values[0])
SECS_SML_RENDER_CONTEXT_GET_SCALAR_IMPL(int16_t, secs::ii::I2, i2, v->values[0])
SECS_SML_RENDER_CONTEXT_GET_SCALAR_IMPL(int32_t, secs::ii::I4, i4, v->values[0])
SECS_SML_RENDER_CONTEXT_GET_SCALAR_IMPL(int64_t, secs::ii::I8, i8, v->values[0])
SECS_SML_RENDER_CONTEXT_GET_SCALAR_IMPL(uint8_t, secs::ii::U1, u1, v->values[0])
SECS_SML_RENDER_CONTEXT_GET_SCALAR_IMPL(uint16_t, secs::ii::U2, u2, v->values[0])
SECS_SML_RENDER_CONTEXT_GET_SCALAR_IMPL(uint32_t, secs::ii::U4, u4, v->values[0])
SECS_SML_RENDER_CONTEXT_GET_SCALAR_IMPL(uint64_t, secs::ii::U8, u8, v->values[0])
SECS_SML_RENDER_CONTEXT_GET_SCALAR_IMPL(float, secs::ii::F4, f4, v->values[0])
SECS_SML_RENDER_CONTEXT_GET_SCALAR_IMPL(double, secs::ii::F8, f8, v->values[0])

#undef SECS_SML_RENDER_CONTEXT_GET_SCALAR_IMPL

// ----------------------------- SML Runtime（Context-Aware） -----------------------------

secs_error_t secs_sml_runtime_encode_message_body(
    const secs_sml_runtime_t *rt,
    const char *name_or_sf,
    const secs_sml_render_context_t *ctx,
    uint8_t **out_body_bytes,
    size_t *out_body_n,
    uint8_t *out_stream,
    uint8_t *out_function,
    int *out_w_bit) {
    return guard_error([&]() -> secs_error_t {
        if (!rt || !name_or_sf || !out_body_bytes || !out_body_n) {
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        }
        *out_body_bytes = nullptr;
        *out_body_n = 0;
        if (out_stream) {
            *out_stream = 0;
        }
        if (out_function) {
            *out_function = 0;
        }
        if (out_w_bit) {
            *out_w_bit = 0;
        }

        secs::sml::RenderContext empty_ctx{};
        const auto &use_ctx = (ctx ? ctx->ctx : empty_ctx);

        std::vector<byte> out;
        bool w_bit = false;
        const auto ec = rt->rt.encode_message_body(
            std::string_view{name_or_sf},
            use_ctx,
            out,
            out_stream,
            out_function,
            out_w_bit ? &w_bit : nullptr);
        if (ec) {
            return from_error_code(ec);
        }
        if (out_w_bit) {
            *out_w_bit = w_bit ? 1 : 0;
        }

        if (!out.empty()) {
            auto *buf = static_cast<uint8_t *>(secs_malloc(out.size()));
            if (!buf) {
                return c_api_err(SECS_C_API_OUT_OF_MEMORY);
            }
            std::memcpy(buf, out.data(), out.size());
            *out_body_bytes = buf;
            *out_body_n = out.size();
        }
        return ok();
    });
}

secs_error_t secs_sml_runtime_match_response_with_context(
    const secs_sml_runtime_t *rt,
    uint8_t stream,
    uint8_t function,
    const uint8_t *body_bytes,
    size_t body_n,
    const secs_sml_render_context_t *ctx,
    char **out_name) {
    return guard_error([&]() -> secs_error_t {
        if (!rt || !out_name) {
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        }
        if (!body_bytes && body_n != 0) {
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        }
        *out_name = nullptr;

        secs::ii::Item decoded{secs::ii::List{}};
        std::size_t consumed = 0;
        if (body_n != 0) {
            auto dec_ec = secs::ii::decode_one(
                bytes_view{reinterpret_cast<const byte *>(body_bytes), body_n},
                decoded,
                consumed);
            if (dec_ec) {
                return from_error_code(dec_ec);
            }
        }

        secs::sml::RenderContext empty_ctx{};
        const auto &use_ctx = (ctx ? ctx->ctx : empty_ctx);
        auto matched = rt->rt.match_response(stream, function, decoded, use_ctx);
        if (!matched.has_value()) {
            return ok();
        }

        auto *s = static_cast<char *>(secs_malloc(matched->size() + 1));
        if (!s) {
            return c_api_err(SECS_C_API_OUT_OF_MEMORY);
        }
        std::memcpy(s, matched->data(), matched->size());
        s[matched->size()] = '\0';
        *out_name = s;
        return ok();
    });
}

secs_error_t secs_sml_runtime_match_response_with_capture(
    const secs_sml_runtime_t *rt,
    uint8_t stream,
    uint8_t function,
    const uint8_t *body_bytes,
    size_t body_n,
    const secs_sml_render_context_t *ctx,
    char **out_name,
    secs_sml_render_context_t **out_captures) {
    return guard_error([&]() -> secs_error_t {
        if (!rt || !out_name) {
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        }
        if (!body_bytes && body_n != 0) {
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        }
        *out_name = nullptr;
        if (out_captures) {
            *out_captures = nullptr;
        }

        secs::ii::Item decoded{secs::ii::List{}};
        std::size_t consumed = 0;
        if (body_n != 0) {
            auto dec_ec = secs::ii::decode_one(
                bytes_view{reinterpret_cast<const byte *>(body_bytes), body_n},
                decoded,
                consumed);
            if (dec_ec) {
                return from_error_code(dec_ec);
            }
        }

        secs::sml::RenderContext empty_ctx{};
        const auto &use_ctx = (ctx ? ctx->ctx : empty_ctx);

        secs::sml::RenderContext captured{};
        auto matched = rt->rt.match_response_with_capture(
            stream, function, decoded, use_ctx, captured);
        if (!matched.has_value()) {
            return ok();
        }

        // 1) out_name
        auto *s = static_cast<char *>(secs_malloc(matched->size() + 1));
        if (!s) {
            return c_api_err(SECS_C_API_OUT_OF_MEMORY);
        }
        std::memcpy(s, matched->data(), matched->size());
        s[matched->size()] = '\0';
        *out_name = s;

        // 2) out_captures（可选）
        if (!out_captures) {
            return ok();
        }
        auto *h = new (std::nothrow) secs_sml_render_context{};
        if (!h) {
            secs_free(*out_name);
            *out_name = nullptr;
            return c_api_err(SECS_C_API_OUT_OF_MEMORY);
        }
        h->ctx = std::move(captured);
        *out_captures = h;
        return ok();
    });
}

secs_error_t secs_sml_runtime_match_response_with_trace(
    const secs_sml_runtime_t *rt,
    uint8_t stream,
    uint8_t function,
    const uint8_t *body_bytes,
    size_t body_n,
    const secs_sml_render_context_t *ctx,
    char **out_name,
    secs_sml_match_trace_t **out_traces,
    size_t *out_trace_count) {
    return guard_error([&]() -> secs_error_t {
        if (!rt || !out_name || !out_traces || !out_trace_count) {
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        }
        if (!body_bytes && body_n != 0) {
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        }
        *out_name = nullptr;
        *out_traces = nullptr;
        *out_trace_count = 0;

        secs::ii::Item decoded{secs::ii::List{}};
        std::size_t consumed = 0;
        if (body_n != 0) {
            auto dec_ec = secs::ii::decode_one(
                bytes_view{reinterpret_cast<const byte *>(body_bytes), body_n},
                decoded,
                consumed);
            if (dec_ec) {
                return from_error_code(dec_ec);
            }
        }

        secs::sml::RenderContext empty_ctx{};
        const auto &use_ctx = (ctx ? ctx->ctx : empty_ctx);
        const auto result =
            rt->rt.match_response_with_trace(stream, function, decoded, use_ctx);

        if (result.response_name.has_value()) {
            const auto &name = *result.response_name;
            auto *s = static_cast<char *>(secs_malloc(name.size() + 1));
            if (!s) {
                return c_api_err(SECS_C_API_OUT_OF_MEMORY);
            }
            std::memcpy(s, name.data(), name.size());
            s[name.size()] = '\0';
            *out_name = s;
            return ok();
        }

        if (result.traces.empty()) {
            return ok();
        }

        const std::size_t n = result.traces.size();
        auto *traces = static_cast<secs_sml_match_trace_t *>(
            secs_malloc(sizeof(secs_sml_match_trace_t) * n));
        if (!traces) {
            return c_api_err(SECS_C_API_OUT_OF_MEMORY);
        }
        std::memset(traces, 0, sizeof(secs_sml_match_trace_t) * n);

        const auto cleanup_partial = [&](std::size_t count) noexcept {
            for (std::size_t i = 0; i < count; ++i) {
                secs_free(const_cast<char *>(traces[i].condition_message_name));
                secs_free(const_cast<char *>(traces[i].detail));
                traces[i].condition_message_name = nullptr;
                traces[i].detail = nullptr;
            }
            secs_free(traces);
        };

        for (std::size_t i = 0; i < n; ++i) {
            const auto &t = result.traces[i];
            traces[i].rule_index = t.rule_index;
            traces[i].condition_message_name = dup_string(t.condition_message_name);
            if (!traces[i].condition_message_name) {
                cleanup_partial(i);
                return c_api_err(SECS_C_API_OUT_OF_MEMORY);
            }
            traces[i].has_index = t.condition_index.has_value() ? 1 : 0;
            traces[i].index = t.condition_index.value_or(0);
            traces[i].has_list_index = t.condition_list_index.has_value() ? 1 : 0;
            traces[i].list_index = t.condition_list_index.value_or(0);
            traces[i].reason = static_cast<int>(t.reason);
            traces[i].detail = dup_string(t.detail);
            if (!traces[i].detail) {
                cleanup_partial(i + 1);
                return c_api_err(SECS_C_API_OUT_OF_MEMORY);
            }
        }

        *out_traces = traces;
        *out_trace_count = n;
        return ok();
    });
}

void secs_sml_match_traces_free(secs_sml_match_trace_t *traces, size_t count) {
    guard_void([&]() {
        if (!traces) {
            return;
        }
        for (size_t i = 0; i < count; ++i) {
            secs_free(const_cast<char *>(traces[i].condition_message_name));
            secs_free(const_cast<char *>(traces[i].detail));
            traces[i].condition_message_name = nullptr;
            traces[i].detail = nullptr;
        }
        secs_free(traces);
    });
}
