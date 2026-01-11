#pragma once

#include "secs/core/common.hpp"
#include "secs/ii/codec.hpp"
#include "secs/protocol/session.hpp"
#include "secs/utils/ii_helpers.hpp"

#include <asio/awaitable.hpp>

#include <cstdint>
#include <optional>
#include <system_error>
#include <utility>

namespace secs::utils {

/**
 * @brief protocol::Session 的易用包装：把 “bytes <-> Item” 的样板代码收敛到一处。
 *
 * 说明：
 * - 该文件为 header-only，避免引入 secs_utils <-> secs_protocol 的循环依赖；
 * - 函数语义尽量保持“薄封装”：底层错误码原样透传。
 */

struct RequestDecodedResult final {
    secs::protocol::DataMessage reply{};

    // reply.body 非空且解码成功时填充；若 reply.body 为空，则保持 nullopt。
    std::optional<DecodeOneItemResult> decoded{};
};

/**
 * @brief 发送空 body 的 primary（W=0）。
 */
inline asio::awaitable<std::error_code>
async_send_empty(secs::protocol::Session &sess,
                 std::uint8_t stream,
                 std::uint8_t function) {
    co_return co_await sess.async_send(stream, function, secs::core::bytes_view{});
}

/**
 * @brief 发送一个 Item 作为 primary（W=0）。
 */
inline asio::awaitable<std::error_code>
async_send_item(secs::protocol::Session &sess,
                std::uint8_t stream,
                std::uint8_t function,
                const secs::ii::Item &item) {
    auto [enc_ec, body] = secs::utils::encode_item(item);
    if (enc_ec) {
        co_return enc_ec;
    }
    co_return co_await sess.async_send(
        stream,
        function,
        secs::core::bytes_view{body.data(), body.size()});
}

/**
 * @brief 发送 request（W=1），并尝试把回应 body 解码为一个 Item。
 *
 * 行为：
 * - 若 reply.body 为空：decoded=nullopt（视为合法场景）；
 * - 若 reply.body 非空：
 *   - 解码成功：decoded 有值；
 *   - 解码失败：返回 decode error_code，reply 原样返回便于上层 dump/排查。
 */
inline asio::awaitable<std::pair<std::error_code, RequestDecodedResult>>
async_request_decoded(secs::protocol::Session &sess,
                      std::uint8_t stream,
                      std::uint8_t function,
                      secs::core::bytes_view body,
                      std::optional<secs::core::duration> timeout = std::nullopt,
                      const secs::ii::DecodeLimits &limits = {}) {
    auto [req_ec, reply] =
        co_await sess.async_request(stream, function, body, timeout);
    if (req_ec) {
        co_return std::pair{req_ec, RequestDecodedResult{}};
    }

    RequestDecodedResult out{};
    out.reply = std::move(reply);

    const secs::core::bytes_view reply_body{out.reply.body.data(),
                                            out.reply.body.size()};
    auto [dec_ec, decoded] = secs::utils::decode_one_item_if_any(reply_body, limits);
    if (dec_ec) {
        co_return std::pair{dec_ec, std::move(out)};
    }
    out.decoded = std::move(decoded);
    co_return std::pair{std::error_code{}, std::move(out)};
}

/**
 * @brief request body 由 Item 生成的 overload。
 */
inline asio::awaitable<std::pair<std::error_code, RequestDecodedResult>>
async_request_decoded(secs::protocol::Session &sess,
                      std::uint8_t stream,
                      std::uint8_t function,
                      const secs::ii::Item &body_item,
                      std::optional<secs::core::duration> timeout = std::nullopt,
                      const secs::ii::DecodeLimits &limits = {}) {
    auto [enc_ec, body] = secs::utils::encode_item(body_item);
    if (enc_ec) {
        co_return std::pair{enc_ec, RequestDecodedResult{}};
    }
    co_return co_await async_request_decoded(
        sess,
        stream,
        function,
        secs::core::bytes_view{body.data(), body.size()},
        timeout,
        limits);
}

/**
 * @brief handler 侧：将 Item 转成 HandlerResult（encode + 搬运 bytes）。
 */
[[nodiscard]] inline secs::protocol::HandlerResult
make_handler_result(const secs::ii::Item &item) noexcept {
    auto [ec, body] = secs::utils::encode_item(item);
    return secs::protocol::HandlerResult{ec, std::move(body)};
}

} // namespace secs::utils

