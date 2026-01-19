#pragma once

#include "secs/ii/struct_codec.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

namespace secs::messages {

/**
 * @brief S2F13 请求（Equipment Constant Request）
 *
 * 常见约定：<L <U4 ECID> ...>.
 */
struct S2F13Request final {
    std::vector<std::uint32_t> ecids;

    static constexpr auto secs_members() {
        return std::make_tuple(&S2F13Request::ecids);
    }

    static std::optional<S2F13Request> from_item(const secs::ii::Item &item) {
        return secs::ii::from_item<S2F13Request>(item);
    }

    [[nodiscard]] secs::ii::Item to_item() const {
        return secs::ii::to_item(*this);
    }
};

/**
 * @brief S2F14 响应（Equipment Constant Data）
 *
 * 常见约定：<L <A ECV> ...>.
 */
struct S2F14Response final {
    std::vector<std::string> ecvs;

    static constexpr auto secs_members() {
        return std::make_tuple(&S2F14Response::ecvs);
    }

    static std::optional<S2F14Response> from_item(const secs::ii::Item &item) {
        return secs::ii::from_item<S2F14Response>(item);
    }

    [[nodiscard]] secs::ii::Item to_item() const {
        return secs::ii::to_item(*this);
    }
};

} // namespace secs::messages
