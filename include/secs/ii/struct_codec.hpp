#pragma once

#include "secs/ii/item.hpp"

#include <concepts>
#include <cstdint>
#include <optional>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace secs::ii {

template <class T>
[[nodiscard]] Item to_item(const T &value);

template <class T>
[[nodiscard]] std::optional<T> from_item(const Item &item);

namespace detail {

template <class>
inline constexpr bool always_false_v = false;

template <class T>
using remove_cvref_t = std::remove_cv_t<std::remove_reference_t<T>>;

template <class T>
struct is_std_vector : std::false_type {};
template <class T, class Alloc>
struct is_std_vector<std::vector<T, Alloc>> : std::true_type {};
template <class T>
inline constexpr bool is_std_vector_v = is_std_vector<T>::value;

template <class T>
using vector_elem_t = typename T::value_type;

template <class T>
concept HasSecsMembers = requires {
    // 约定：返回一个由“成员指针”组成的 tuple。
    { T::secs_members() };
};

template <class T>
concept HasMemberToItem = requires(const T &msg) {
    { msg.to_item() } -> std::same_as<Item>;
};

template <class T>
concept HasMemberFromItem = requires(const Item &item) {
    { T::from_item(item) } -> std::same_as<std::optional<T>>;
};

template <class T, class Tuple, std::size_t... I>
[[nodiscard]] inline Item encode_fixed_list(const T &obj,
                                            const Tuple &ptrs,
                                            std::index_sequence<I...>) {
    std::vector<Item> items;
    items.reserve(sizeof...(I));
    (items.push_back(secs::ii::to_item(obj.*std::get<I>(ptrs))), ...);
    return Item::list(std::move(items));
}

template <class T, class Tuple, std::size_t I>
[[nodiscard]] inline bool decode_fixed_assign_one(T &out,
                                                  const Tuple &ptrs,
                                                  const List &list) {
    auto ptr = std::get<I>(ptrs);
    using Field = remove_cvref_t<decltype(out.*ptr)>;
    auto v = secs::ii::from_item<Field>(list[I]);
    if (!v.has_value()) {
        return false;
    }
    out.*ptr = std::move(v.value());
    return true;
}

template <class T, class Tuple, std::size_t... I>
[[nodiscard]] inline std::optional<T> decode_fixed_list(const Item &item,
                                                        const Tuple &ptrs,
                                                        std::index_sequence<I...>) {
    auto *list = item.get_if<List>();
    if (!list || list->size() != sizeof...(I)) {
        return std::nullopt;
    }

    static_assert(std::is_default_constructible_v<T>,
                  "secs::ii::from_item<T>: struct decode requires T{}");
    T out{};
    if (!(decode_fixed_assign_one<T, Tuple, I>(out, ptrs, *list) && ...)) {
        return std::nullopt;
    }
    return out;
}

template <class T, class VecMemberPtr>
[[nodiscard]] inline Item encode_vector_only(const T &obj, VecMemberPtr ptr) {
    using Vec = remove_cvref_t<decltype(obj.*ptr)>;
    static_assert(is_std_vector_v<Vec>,
                  "secs::ii::to_item: vector-only mode expects std::vector<>");

    const auto &vec = obj.*ptr;
    std::vector<Item> items;
    items.reserve(vec.size());
    for (const auto &elem : vec) {
        items.push_back(secs::ii::to_item(elem));
    }
    return Item::list(std::move(items));
}

template <class T, class VecMemberPtr>
[[nodiscard]] inline std::optional<T> decode_vector_only(const Item &item,
                                                         VecMemberPtr ptr) {
    auto *list = item.get_if<List>();
    if (!list) {
        return std::nullopt;
    }

    static_assert(std::is_default_constructible_v<T>,
                  "secs::ii::from_item<T>: struct decode requires T{}");
    T out{};

    using Vec = remove_cvref_t<decltype(out.*ptr)>;
    static_assert(is_std_vector_v<Vec>,
                  "secs::ii::from_item: vector-only mode expects std::vector<>");
    using Elem = remove_cvref_t<vector_elem_t<Vec>>;

    auto &vec = out.*ptr;
    vec.clear();
    vec.reserve(list->size());
    for (const auto &elem_item : *list) {
        auto v = secs::ii::from_item<Elem>(elem_item);
        if (!v.has_value()) {
            return std::nullopt;
        }
        vec.push_back(std::move(v.value()));
    }
    return out;
}

} // namespace detail

/**
 * @brief “声明式”消息编解码：Item <-> struct 的默认映射。
 *
 * 目标：尽量减少 TypedHandler 场景下手写 to_item/from_item 的样板代码。
 *
 * 使用方式（模板元编程，无宏/无继承）：
 *
 * @code
 * struct S1F2Response final {
 *     std::string mdln;
 *     std::string softrev;
 *
 *     // 字段顺序即 List 中的子项顺序
 *     static constexpr auto secs_members() {
 *         return std::make_tuple(&S1F2Response::mdln, &S1F2Response::softrev);
 *     }
 * };
 *
 * auto item = secs::ii::to_item(S1F2Response{\"M\", \"R\"});
 * auto obj  = secs::ii::from_item<S1F2Response>(item);
 * @endcode
 *
 * 默认布局规则：
 * 1) secs_members() 返回 N 个成员指针：
 *    - to_item(obj): 生成 <L field0 field1 ... fieldN-1>
 *    - from_item<T>(item): 要求输入为 List 且 length==N，逐一 decode，否则返回 nullopt
 * 2) 若 secs_members() 只返回 1 个成员指针，且该成员类型为 std::vector<T>：
 *    - to_item(obj): 生成 <L elem...>（逐个编码）
 *    - from_item<T>(item): 要求输入为 List，逐个解码进 vector
 *
 * 支持的字段类型（默认映射）：
 * - std::string <-> ASCII
 * - bool <-> Boolean（单值）
 * - int8/16/32/64 <-> I1/I2/I4/I8（单值）
 * - uint8/16/32/64 <-> U1/U2/U4/U8（单值）
 * - float/double <-> F4/F8（单值）
 * - secs::ii::Item <-> Item（透传）
 * - 递归：字段类型若也提供 secs_members()，则支持嵌套（List 内嵌 List）
 *
 * 兼容性：
 * - 若某类型未提供 secs_members()，但已实现传统 member API（to_item/from_item），
 *   则 to_item()/from_item<T>() 会回退到该 member API。
 */
template <class T>
[[nodiscard]] Item to_item(const T &value) {
    using U = detail::remove_cvref_t<T>;

    if constexpr (std::is_same_v<U, Item>) {
        return value;
    } else if constexpr (std::is_same_v<U, std::string>) {
        return Item::ascii(value);
    } else if constexpr (std::is_same_v<U, bool>) {
        return Item::boolean(std::vector<bool>{value});
    } else if constexpr (std::is_same_v<U, std::int8_t>) {
        return Item::i1({value});
    } else if constexpr (std::is_same_v<U, std::int16_t>) {
        return Item::i2({value});
    } else if constexpr (std::is_same_v<U, std::int32_t>) {
        return Item::i4({value});
    } else if constexpr (std::is_same_v<U, std::int64_t>) {
        return Item::i8({value});
    } else if constexpr (std::is_same_v<U, std::uint8_t>) {
        return Item::u1({value});
    } else if constexpr (std::is_same_v<U, std::uint16_t>) {
        return Item::u2({value});
    } else if constexpr (std::is_same_v<U, std::uint32_t>) {
        return Item::u4({value});
    } else if constexpr (std::is_same_v<U, std::uint64_t>) {
        return Item::u8({value});
    } else if constexpr (std::is_same_v<U, float>) {
        return Item::f4({value});
    } else if constexpr (std::is_same_v<U, double>) {
        return Item::f8({value});
    } else if constexpr (detail::HasSecsMembers<U>) {
        const auto ptrs = U::secs_members();
        constexpr std::size_t n =
            std::tuple_size_v<detail::remove_cvref_t<decltype(ptrs)>>;

        if constexpr (n == 0) {
            return Item::list({});
        } else if constexpr (n == 1) {
            auto ptr = std::get<0>(ptrs);
            using Field = detail::remove_cvref_t<
                decltype(std::declval<const U &>().*ptr)>;
            if constexpr (detail::is_std_vector_v<Field>) {
                return detail::encode_vector_only(static_cast<const U &>(value),
                                                  ptr);
            } else {
                return detail::encode_fixed_list(static_cast<const U &>(value),
                                                 ptrs,
                                                 std::make_index_sequence<n>{});
            }
        } else {
            return detail::encode_fixed_list(static_cast<const U &>(value),
                                             ptrs,
                                             std::make_index_sequence<n>{});
        }
    } else if constexpr (detail::HasMemberToItem<U>) {
        return value.to_item();
    } else {
        static_assert(detail::always_false_v<U>,
                      "secs::ii::to_item: unsupported type");
    }
}

template <class T>
[[nodiscard]] std::optional<T> from_item(const Item &item) {
    using U = detail::remove_cvref_t<T>;
    static_assert(std::is_same_v<U, T>,
                  "secs::ii::from_item<T>: T must not be cv/ref");

    if constexpr (std::is_same_v<U, Item>) {
        return item;
    } else if constexpr (std::is_same_v<U, std::string>) {
        auto *ascii = item.get_if<ASCII>();
        if (!ascii) {
            return std::nullopt;
        }
        return ascii->value;
    } else if constexpr (std::is_same_v<U, bool>) {
        auto *b = item.get_if<Boolean>();
        if (!b || b->values.size() != 1) {
            return std::nullopt;
        }
        return b->values[0];
    } else if constexpr (std::is_same_v<U, std::int8_t>) {
        auto *p = item.get_if<I1>();
        if (!p || p->values.size() != 1) {
            return std::nullopt;
        }
        return p->values[0];
    } else if constexpr (std::is_same_v<U, std::int16_t>) {
        auto *p = item.get_if<I2>();
        if (!p || p->values.size() != 1) {
            return std::nullopt;
        }
        return p->values[0];
    } else if constexpr (std::is_same_v<U, std::int32_t>) {
        auto *p = item.get_if<I4>();
        if (!p || p->values.size() != 1) {
            return std::nullopt;
        }
        return p->values[0];
    } else if constexpr (std::is_same_v<U, std::int64_t>) {
        auto *p = item.get_if<I8>();
        if (!p || p->values.size() != 1) {
            return std::nullopt;
        }
        return p->values[0];
    } else if constexpr (std::is_same_v<U, std::uint8_t>) {
        auto *p = item.get_if<U1>();
        if (!p || p->values.size() != 1) {
            return std::nullopt;
        }
        return p->values[0];
    } else if constexpr (std::is_same_v<U, std::uint16_t>) {
        auto *p = item.get_if<U2>();
        if (!p || p->values.size() != 1) {
            return std::nullopt;
        }
        return p->values[0];
    } else if constexpr (std::is_same_v<U, std::uint32_t>) {
        auto *p = item.get_if<U4>();
        if (!p || p->values.size() != 1) {
            return std::nullopt;
        }
        return p->values[0];
    } else if constexpr (std::is_same_v<U, std::uint64_t>) {
        auto *p = item.get_if<U8>();
        if (!p || p->values.size() != 1) {
            return std::nullopt;
        }
        return p->values[0];
    } else if constexpr (std::is_same_v<U, float>) {
        auto *p = item.get_if<F4>();
        if (!p || p->values.size() != 1) {
            return std::nullopt;
        }
        return p->values[0];
    } else if constexpr (std::is_same_v<U, double>) {
        auto *p = item.get_if<F8>();
        if (!p || p->values.size() != 1) {
            return std::nullopt;
        }
        return p->values[0];
    } else if constexpr (detail::HasSecsMembers<U>) {
        const auto ptrs = U::secs_members();
        constexpr std::size_t n =
            std::tuple_size_v<detail::remove_cvref_t<decltype(ptrs)>>;

        if constexpr (n == 0) {
            // 空消息体：只要求是 List（不强制 length==0，避免过度严格）。
            if (!item.get_if<List>()) {
                return std::nullopt;
            }
            static_assert(std::is_default_constructible_v<U>,
                          "secs::ii::from_item<T>: empty struct requires T{}");
            return U{};
        } else if constexpr (n == 1) {
            auto ptr = std::get<0>(ptrs);
            using Field =
                detail::remove_cvref_t<decltype(std::declval<U &>().*ptr)>;
            if constexpr (detail::is_std_vector_v<Field>) {
                return detail::decode_vector_only<U>(item, ptr);
            } else {
                return detail::decode_fixed_list<U>(item,
                                                    ptrs,
                                                    std::make_index_sequence<n>{});
            }
        } else {
            return detail::decode_fixed_list<U>(item,
                                                ptrs,
                                                std::make_index_sequence<n>{});
        }
    } else if constexpr (detail::HasMemberFromItem<U>) {
        return U::from_item(item);
    } else {
        static_assert(detail::always_false_v<U>,
                      "secs::ii::from_item<T>: unsupported type");
    }
}

} // namespace secs::ii
