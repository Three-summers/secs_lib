#pragma once

#include "secs/ii/item.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace secs::messages {

/**
 * @brief S1F1 请求（Are You There）
 *
 * 常见约定：消息体为空 List。
 */
struct S1F1Request final {
    static std::optional<S1F1Request> from_item(const secs::ii::Item &item) {
        if (!item.get_if<secs::ii::List>()) {
            return std::nullopt;
        }
        return S1F1Request{};
    }

    [[nodiscard]] secs::ii::Item to_item() const {
        return secs::ii::Item::list({});
    }
};

/**
 * @brief S1F2 响应（On Line Data）
 *
 * 常见约定：<L <A MDLN> <A SOFTREV>>.
 */
struct S1F2Response final {
    std::string mdln;
    std::string softrev;

    static std::optional<S1F2Response> from_item(const secs::ii::Item &item) {
        auto *list = item.get_if<secs::ii::List>();
        if (!list || list->size() != 2) {
            return std::nullopt;
        }

        auto *mdln = (*list)[0].get_if<secs::ii::ASCII>();
        auto *softrev = (*list)[1].get_if<secs::ii::ASCII>();
        if (!mdln || !softrev) {
            return std::nullopt;
        }

        return S1F2Response{mdln->value, softrev->value};
    }

    [[nodiscard]] secs::ii::Item to_item() const {
        return secs::ii::Item::list(
            {secs::ii::Item::ascii(mdln), secs::ii::Item::ascii(softrev)});
    }
};

} // namespace secs::messages

