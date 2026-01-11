#pragma once

#include "secs/ii/item.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace secs::messages {

/**
 * @brief S2F13 请求（Equipment Constant Request）
 *
 * 常见约定：<L <U4 ECID> ...>.
 */
struct S2F13Request final {
    std::vector<std::uint32_t> ecids;

    static std::optional<S2F13Request> from_item(const secs::ii::Item &item) {
        auto *list = item.get_if<secs::ii::List>();
        if (!list) {
            return std::nullopt;
        }

        S2F13Request req;
        req.ecids.reserve(list->size());
        for (const auto &elem : *list) {
            auto *u4 = elem.get_if<secs::ii::U4>();
            if (!u4 || u4->values.size() != 1) {
                return std::nullopt;
            }
            req.ecids.push_back(u4->values[0]);
        }
        return req;
    }

    [[nodiscard]] secs::ii::Item to_item() const {
        std::vector<secs::ii::Item> items;
        items.reserve(ecids.size());
        for (const auto id : ecids) {
            items.push_back(secs::ii::Item::u4({id}));
        }
        return secs::ii::Item::list(std::move(items));
    }
};

/**
 * @brief S2F14 响应（Equipment Constant Data）
 *
 * 常见约定：<L <A ECV> ...>.
 */
struct S2F14Response final {
    std::vector<std::string> ecvs;

    static std::optional<S2F14Response> from_item(const secs::ii::Item &item) {
        auto *list = item.get_if<secs::ii::List>();
        if (!list) {
            return std::nullopt;
        }

        S2F14Response resp;
        resp.ecvs.reserve(list->size());
        for (const auto &elem : *list) {
            auto *ascii = elem.get_if<secs::ii::ASCII>();
            if (!ascii) {
                return std::nullopt;
            }
            resp.ecvs.push_back(ascii->value);
        }
        return resp;
    }

    [[nodiscard]] secs::ii::Item to_item() const {
        std::vector<secs::ii::Item> items;
        items.reserve(ecvs.size());
        for (const auto &val : ecvs) {
            items.push_back(secs::ii::Item::ascii(val));
        }
        return secs::ii::Item::list(std::move(items));
    }
};

} // namespace secs::messages

