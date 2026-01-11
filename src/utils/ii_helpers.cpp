#include "secs/utils/ii_helpers.hpp"

namespace secs::utils {

std::pair<std::error_code, std::vector<secs::core::byte>>
encode_item(const secs::ii::Item &item) noexcept {
    std::vector<secs::core::byte> out;
    const auto ec = secs::ii::encode(item, out);
    return {ec, std::move(out)};
}

std::pair<std::error_code, DecodeOneItemResult>
decode_one_item(secs::core::bytes_view in,
                const secs::ii::DecodeLimits &limits) noexcept {
    DecodeOneItemResult result{};
    result.consumed = 0;
    result.fully_consumed = false;

    const auto ec = secs::ii::decode_one(in, result.item, result.consumed, limits);
    if (ec) {
        return {ec, DecodeOneItemResult{}};
    }

    result.fully_consumed = (result.consumed == in.size());
    return {std::error_code{}, std::move(result)};
}

std::pair<std::error_code, std::optional<DecodeOneItemResult>>
decode_one_item_if_any(secs::core::bytes_view in,
                       const secs::ii::DecodeLimits &limits) noexcept {
    if (in.empty()) {
        return {std::error_code{}, std::nullopt};
    }

    auto [ec, result] = decode_one_item(in, limits);
    if (ec) {
        return {ec, std::nullopt};
    }

    return {std::error_code{}, std::move(result)};
}

} // namespace secs::utils

