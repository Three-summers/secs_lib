#pragma once

#include "secs/ii/item.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace secs::examples {

using ii::ASCII;
using ii::Item;
using ii::List;
using ii::U4;

// ============================================================================
// S1F1 请求（Are You There）- 通常为空 List
// ============================================================================
struct S1F1Request {
    static std::optional<S1F1Request> from_item(const ii::Item &item) {
        // S1F1 通常是空 List 或无消息体
        auto *list = item.get_if<List>();
        if (!list) {
            return std::nullopt;
        }
        return S1F1Request{};
    }

    ii::Item to_item() const { return Item::list({}); }
};

// ============================================================================
// S1F2 响应（On Line Data）
// ============================================================================
struct S1F2Response {
    std::string mdln;    // 设备型号（MDLN）
    std::string softrev; // 软件版本（SOFTREV）

    static std::optional<S1F2Response> from_item(const ii::Item &item) {
        auto *list = item.get_if<List>();
        if (!list || list->size() != 2) {
            return std::nullopt;
        }

        auto *mdln = (*list)[0].get_if<ASCII>();
        auto *softrev = (*list)[1].get_if<ASCII>();
        if (!mdln || !softrev) {
            return std::nullopt;
        }

        return S1F2Response{mdln->value, softrev->value};
    }

    ii::Item to_item() const {
        return Item::list({Item::ascii(mdln), Item::ascii(softrev)});
    }
};

// ============================================================================
// S2F13 请求（Equipment Constant Request）
// ============================================================================
struct S2F13Request {
    std::vector<std::uint32_t> ecids; // 设备常量 ID 列表（ECID）

    static std::optional<S2F13Request> from_item(const ii::Item &item) {
        auto *list = item.get_if<List>();
        if (!list) {
            return std::nullopt;
        }

        S2F13Request req;
        for (const auto &elem : *list) {
            auto *u4 = elem.get_if<U4>();
            if (!u4 || u4->values.empty()) {
                return std::nullopt;
            }
            req.ecids.push_back(u4->values[0]);
        }
        return req;
    }

    ii::Item to_item() const {
        std::vector<Item> items;
        items.reserve(ecids.size());
        for (auto id : ecids) {
            items.push_back(Item::u4({id}));
        }
        return Item::list(std::move(items));
    }
};

// ============================================================================
// S2F14 响应（Equipment Constant Data）
// ============================================================================
struct S2F14Response {
    std::vector<std::string> ecvs; // 设备常量值列表（ECV）

    static std::optional<S2F14Response> from_item(const ii::Item &item) {
        auto *list = item.get_if<List>();
        if (!list) {
            return std::nullopt;
        }

        S2F14Response resp;
        for (const auto &elem : *list) {
            auto *ascii = elem.get_if<ASCII>();
            if (!ascii) {
                return std::nullopt;
            }
            resp.ecvs.push_back(ascii->value);
        }
        return resp;
    }

    ii::Item to_item() const {
        std::vector<Item> items;
        items.reserve(ecvs.size());
        for (const auto &val : ecvs) {
            items.push_back(Item::ascii(val));
        }
        return Item::list(std::move(items));
    }
};

} // namespace secs::examples
