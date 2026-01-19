#pragma once

#include "secs/ii/struct_codec.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <tuple>

namespace secs::messages {

/**
 * @brief S1F1 请求（Are You There）
 *
 * 常见约定：消息体为空 List。
 */
struct S1F1Request final {
    static constexpr auto secs_members() { return std::make_tuple(); }

    static std::optional<S1F1Request> from_item(const secs::ii::Item &item) {
        return secs::ii::from_item<S1F1Request>(item);
    }

    [[nodiscard]] secs::ii::Item to_item() const {
        return secs::ii::to_item(*this);
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

    static constexpr auto secs_members() {
        return std::make_tuple(&S1F2Response::mdln, &S1F2Response::softrev);
    }

    static std::optional<S1F2Response> from_item(const secs::ii::Item &item) {
        return secs::ii::from_item<S1F2Response>(item);
    }

    [[nodiscard]] secs::ii::Item to_item() const {
        return secs::ii::to_item(*this);
    }
};

} // namespace secs::messages
