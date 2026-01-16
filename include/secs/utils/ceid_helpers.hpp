#pragma once

#include "secs/core/error.hpp"
#include "secs/ii/item.hpp"
#include "secs/utils/protocol_helpers.hpp"

#include <asio/awaitable.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <system_error>
#include <utility>

namespace secs::utils {

/**
 * @brief 从 Item 中提取 CEID 的函数类型（由厂商文档定义）。
 *
 * 说明：
 * - 该层不引入 GEM(E30) 语义；仅把“如何从消息体里取出 CEID”外置为策略；
 * - 返回 nullopt 表示该 Item 不满足预期结构/类型。
 */
using Ceid = std::uint32_t;
using CeidExtractor = std::function<std::optional<Ceid>(const secs::ii::Item &)>;

/**
 * @brief request-响应辅助结果：在 async_request_decoded 的基础上附带 CEID。
 */
struct RequestCeidDecodedResult final {
    secs::protocol::DataMessage reply{};
    std::optional<DecodeOneItemResult> decoded{};

    // 若提取失败，则为 nullopt。
    std::optional<Ceid> request_ceid{};
    std::optional<Ceid> reply_ceid{};
};

/**
 * @brief 发送 request（W=1）并尝试解码 reply，同时提取 request/reply 中的 CEID。
 *
 * @param request_body 用于发送的 SECS-II Item（由 encode_item 编码）
 * @param request_ceid_extractor 从 request_body 提取 CEID
 * @param reply_ceid_extractor 从 reply 解码后的 Item 提取 CEID
 * @param verify_equal 若为 true，则要求 request/reply CEID 均存在且相等，否则返回 invalid_argument
 *
 * 说明：
 * - 若 reply.body 为空：decoded=nullopt，reply_ceid=nullopt
 * - 若 reply.body 非空但解码失败：返回 decode error_code，并保留 reply 便于排查
 */
inline asio::awaitable<std::pair<std::error_code, RequestCeidDecodedResult>>
async_request_decoded_with_ceid(secs::protocol::Session &sess,
                                std::uint8_t stream,
                                std::uint8_t function,
                                const secs::ii::Item &request_body,
                                CeidExtractor request_ceid_extractor,
                                CeidExtractor reply_ceid_extractor,
                                bool verify_equal = true,
                                std::optional<secs::core::duration> timeout =
                                    std::nullopt,
                                const secs::ii::DecodeLimits &limits = {}) {
    RequestCeidDecodedResult out{};

    if (request_ceid_extractor) {
        out.request_ceid = request_ceid_extractor(request_body);
    }
    if (verify_equal && !out.request_ceid.has_value()) {
        co_return std::pair{
            secs::core::make_error_code(secs::core::errc::invalid_argument),
            std::move(out)};
    }

    auto [ec, decoded] = co_await secs::utils::async_request_decoded(
        sess, stream, function, request_body, timeout, limits);
    if (ec) {
        // 透传错误码，同时尽量带回 reply（若底层已有）。
        out.reply = std::move(decoded.reply);
        out.decoded = std::move(decoded.decoded);
        co_return std::pair{ec, std::move(out)};
    }

    out.reply = std::move(decoded.reply);
    out.decoded = std::move(decoded.decoded);

    if (out.decoded.has_value() && reply_ceid_extractor) {
        out.reply_ceid = reply_ceid_extractor(out.decoded->item);
    }

    if (verify_equal) {
        if (!out.reply_ceid.has_value() ||
            out.reply_ceid.value() != out.request_ceid.value()) {
            co_return std::pair{
                secs::core::make_error_code(secs::core::errc::invalid_argument),
                std::move(out)};
        }
    }

    co_return std::pair{std::error_code{}, std::move(out)};
}

/**
 * @brief 提取一个“无符号标量”到 uint32（常用于 CEID/DATAID 等字段）。
 *
 * 支持类型：U1/U2/U4/U8（U8 仅在不溢出 uint32 时返回）。
 */
[[nodiscard]] inline std::optional<std::uint32_t>
extract_unsigned_scalar_u32(const secs::ii::Item &item) noexcept {
    if (auto *u1 = item.get_if<secs::ii::U1>()) {
        if (u1->values.size() == 1) {
            return u1->values[0];
        }
        return std::nullopt;
    }
    if (auto *u2 = item.get_if<secs::ii::U2>()) {
        if (u2->values.size() == 1) {
            return u2->values[0];
        }
        return std::nullopt;
    }
    if (auto *u4 = item.get_if<secs::ii::U4>()) {
        if (u4->values.size() == 1) {
            return u4->values[0];
        }
        return std::nullopt;
    }
    if (auto *u8 = item.get_if<secs::ii::U8>()) {
        if (u8->values.size() == 1) {
            if (u8->values[0] <=
                static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max())) {
                return static_cast<std::uint32_t>(u8->values[0]);
            }
        }
        return std::nullopt;
    }
    return std::nullopt;
}

/**
 * @brief 从 List 的指定下标提取 uint32 标量。
 */
[[nodiscard]] inline std::optional<std::uint32_t>
extract_list_unsigned_u32_at(const secs::ii::Item &list_item,
                             std::size_t index) noexcept {
    auto *list = list_item.get_if<secs::ii::List>();
    if (!list || index >= list->size()) {
        return std::nullopt;
    }
    return extract_unsigned_scalar_u32((*list)[index]);
}

/**
 * @brief 参考实现：S6F11-like 的 CEID 提取（<L <DATAID> <CEID> <...>>）。
 *
 * 该函数仅作为“厂商文档字段布局”的最小示例：
 * - CEID 位于 List 的第 2 个元素（index=1）
 * - CEID 是 U1/U2/U4/U8 的单值标量（推荐 U4）
 *
 * 若你的厂商文档布局不同，请按需改写 extractor。
 */
[[nodiscard]] inline std::optional<std::uint32_t>
extract_ceid_s6f11_like(const secs::ii::Item &body) noexcept {
    return extract_list_unsigned_u32_at(body, 1);
}

} // namespace secs::utils

