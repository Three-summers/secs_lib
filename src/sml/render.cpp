#include "secs/sml/render.hpp"

#include "secs/core/error.hpp"

#include <new>
#include <system_error>
#include <type_traits>

namespace secs::sml {

namespace {

class RenderErrorCategory final : public std::error_category {
public:
    [[nodiscard]] const char *name() const noexcept override {
        return "sml.render";
    }

    [[nodiscard]] std::string message(int ev) const override {
        switch (static_cast<render_errc>(ev)) {
        case render_errc::ok:
            return "success";
        case render_errc::missing_variable:
            return "missing variable";
        case render_errc::type_mismatch:
            return "type mismatch";
        }
        return "unknown render error";
    }
};

const RenderErrorCategory kRenderErrorCategory{};

[[nodiscard]] std::error_code render_ascii(const TplASCII &a,
                                           const RenderContext &ctx,
                                           secs::ii::Item &out) {
    if (const auto *s = std::get_if<std::string>(&a.value)) {
        out = secs::ii::Item::ascii(*s);
        return {};
    }

    const auto *ref = std::get_if<VarRef>(&a.value);
    if (!ref) {
        return make_error_code(render_errc::type_mismatch);
    }

    const auto *v = ctx.get(ref->name);
    if (!v) {
        return make_error_code(render_errc::missing_variable);
    }
    const auto *ascii = v->get_if<secs::ii::ASCII>();
    if (!ascii) {
        return make_error_code(render_errc::type_mismatch);
    }
    out = secs::ii::Item::ascii(ascii->value);
    return {};
}

[[nodiscard]] std::error_code render_binary(const TplBinary &b,
                                            const RenderContext &ctx,
                                            secs::ii::Item &out) {
    std::vector<secs::ii::byte> bytes;
    for (const auto &expr : b.values) {
        if (const auto *lit = std::get_if<secs::ii::byte>(&expr)) {
            bytes.push_back(*lit);
            continue;
        }

        const auto *ref = std::get_if<VarRef>(&expr);
        if (!ref) {
            return make_error_code(render_errc::type_mismatch);
        }
        const auto *v = ctx.get(ref->name);
        if (!v) {
            return make_error_code(render_errc::missing_variable);
        }
        const auto *bin = v->get_if<secs::ii::Binary>();
        if (!bin) {
            return make_error_code(render_errc::type_mismatch);
        }
        bytes.insert(bytes.end(), bin->value.begin(), bin->value.end());
    }

    out = secs::ii::Item::binary(std::move(bytes));
    return {};
}

[[nodiscard]] std::error_code render_boolean(const TplBoolean &b,
                                             const RenderContext &ctx,
                                             secs::ii::Item &out) {
    std::vector<bool> values;
    for (const auto &expr : b.values) {
        if (const auto *lit = std::get_if<bool>(&expr)) {
            values.push_back(*lit);
            continue;
        }

        const auto *ref = std::get_if<VarRef>(&expr);
        if (!ref) {
            return make_error_code(render_errc::type_mismatch);
        }
        const auto *v = ctx.get(ref->name);
        if (!v) {
            return make_error_code(render_errc::missing_variable);
        }
        const auto *bv = v->get_if<secs::ii::Boolean>();
        if (!bv) {
            return make_error_code(render_errc::type_mismatch);
        }
        values.insert(values.end(), bv->values.begin(), bv->values.end());
    }

    out = secs::ii::Item::boolean(std::move(values));
    return {};
}

template <class T, class IiT, class MakeFn>
[[nodiscard]] std::error_code render_numeric(const std::vector<ValueExpr<T>> &exprs,
                                             const RenderContext &ctx,
                                             secs::ii::Item &out,
                                             MakeFn &&make_item) {
    std::vector<T> values;
    for (const auto &expr : exprs) {
        if (const auto *lit = std::get_if<T>(&expr)) {
            values.push_back(*lit);
            continue;
        }

        const auto *ref = std::get_if<VarRef>(&expr);
        if (!ref) {
            return make_error_code(render_errc::type_mismatch);
        }
        const auto *v = ctx.get(ref->name);
        if (!v) {
            return make_error_code(render_errc::missing_variable);
        }
        const auto *tv = v->template get_if<IiT>();
        if (!tv) {
            return make_error_code(render_errc::type_mismatch);
        }
        values.insert(values.end(), tv->values.begin(), tv->values.end());
    }

    out = make_item(std::move(values));
    return {};
}

} // namespace

const std::error_category &render_error_category() noexcept {
    return kRenderErrorCategory;
}

std::error_code make_error_code(render_errc e) noexcept {
    return {static_cast<int>(e), kRenderErrorCategory};
}

std::error_code render_item(const TemplateItem &tpl,
                            const RenderContext &ctx,
                            secs::ii::Item &out) noexcept {
    try {
        return std::visit(
            [&](const auto &alt) -> std::error_code {
                using T = std::decay_t<decltype(alt)>;

                if constexpr (std::is_same_v<T, TplList>) {
                    std::vector<secs::ii::Item> items;
                    items.reserve(alt.size());
                    for (const auto &child : alt) {
                        secs::ii::Item rendered{secs::ii::List{}};
                        const auto ec = render_item(child, ctx, rendered);
                        if (ec) {
                            return ec;
                        }
                        items.push_back(std::move(rendered));
                    }
                    out = secs::ii::Item::list(std::move(items));
                    return {};
                } else if constexpr (std::is_same_v<T, TplASCII>) {
                    return render_ascii(alt, ctx, out);
                } else if constexpr (std::is_same_v<T, TplBinary>) {
                    return render_binary(alt, ctx, out);
                } else if constexpr (std::is_same_v<T, TplBoolean>) {
                    return render_boolean(alt, ctx, out);
                } else if constexpr (std::is_same_v<T, TplI1>) {
                    return render_numeric<std::int8_t, secs::ii::I1>(
                        alt.values, ctx, out, secs::ii::Item::i1);
                } else if constexpr (std::is_same_v<T, TplI2>) {
                    return render_numeric<std::int16_t, secs::ii::I2>(
                        alt.values, ctx, out, secs::ii::Item::i2);
                } else if constexpr (std::is_same_v<T, TplI4>) {
                    return render_numeric<std::int32_t, secs::ii::I4>(
                        alt.values, ctx, out, secs::ii::Item::i4);
                } else if constexpr (std::is_same_v<T, TplI8>) {
                    return render_numeric<std::int64_t, secs::ii::I8>(
                        alt.values, ctx, out, secs::ii::Item::i8);
                } else if constexpr (std::is_same_v<T, TplU1>) {
                    return render_numeric<std::uint8_t, secs::ii::U1>(
                        alt.values, ctx, out, secs::ii::Item::u1);
                } else if constexpr (std::is_same_v<T, TplU2>) {
                    return render_numeric<std::uint16_t, secs::ii::U2>(
                        alt.values, ctx, out, secs::ii::Item::u2);
                } else if constexpr (std::is_same_v<T, TplU4>) {
                    return render_numeric<std::uint32_t, secs::ii::U4>(
                        alt.values, ctx, out, secs::ii::Item::u4);
                } else if constexpr (std::is_same_v<T, TplU8>) {
                    return render_numeric<std::uint64_t, secs::ii::U8>(
                        alt.values, ctx, out, secs::ii::Item::u8);
                } else if constexpr (std::is_same_v<T, TplF4>) {
                    return render_numeric<float, secs::ii::F4>(
                        alt.values, ctx, out, secs::ii::Item::f4);
                } else if constexpr (std::is_same_v<T, TplF8>) {
                    return render_numeric<double, secs::ii::F8>(
                        alt.values, ctx, out, secs::ii::Item::f8);
                } else {
                    return secs::core::make_error_code(
                        secs::core::errc::invalid_argument);
                }
            },
            tpl.storage());
    } catch (const std::bad_alloc &) {
        return secs::core::make_error_code(secs::core::errc::out_of_memory);
    } catch (...) {
        return secs::core::make_error_code(secs::core::errc::invalid_argument);
    }
}

} // namespace secs::sml
