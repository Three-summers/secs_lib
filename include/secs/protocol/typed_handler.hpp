#pragma once

#include "secs/core/common.hpp"
#include "secs/core/error.hpp"
#include "secs/ii/codec.hpp"
#include "secs/ii/item.hpp"
#include "secs/protocol/router.hpp"

#include <asio/awaitable.hpp>

#include <concepts>
#include <cstdint>
#include <optional>
#include <system_error>
#include <utility>
#include <vector>

namespace secs::protocol {

/**
 * @brief Concept 约束：要求消息类型具备 from_item() 和 to_item() 方法。
 *
 * 任何满足此约束的类型都可以用作 TypedHandler 的请求/响应类型。
 */
template <typename T>
concept SecsMessage = requires(const ii::Item& item, const T& msg) {
  { T::from_item(item) } -> std::same_as<std::optional<T>>;
  { msg.to_item() } -> std::same_as<ii::Item>;
};

/**
 * @brief 类型安全的 SECS 消息处理器基类。
 *
 * 说明：
 * - TRequest/TResponse 必须满足 SecsMessage concept
 * - 子类实现 handle() 虚函数，处理业务逻辑
 * - invoke() 方法自动处理解码/编码，由框架调用
 * - 错误传播：解码失败/业务错误/编码失败统一通过 std::error_code 返回
 *
 * 使用示例：
 * @code
 * struct S1F1Request {
 *   static std::optional<S1F1Request> from_item(const ii::Item& item);
 *   ii::Item to_item() const;
 * };
 *
 * struct S1F2Response {
 *   std::string mdln;
 *   std::string softrev;
 *   static std::optional<S1F2Response> from_item(const ii::Item& item);
 *   ii::Item to_item() const;
 * };
 *
 * class S1F1Handler : public TypedHandler<S1F1Request, S1F2Response> {
 * public:
 *   asio::awaitable<std::pair<std::error_code, S1F2Response>>
 *   handle(const S1F1Request& request, const DataMessage& raw) override {
 *     S1F2Response response{"MyEquipment", "1.0.0"};
 *     co_return {std::error_code{}, response};
 *   }
 * };
 * @endcode
 */
template <SecsMessage TRequest, SecsMessage TResponse>
class TypedHandler {
 public:
  virtual ~TypedHandler() = default;

  /**
   * @brief 业务逻辑处理函数（纯虚函数，由子类实现）。
   *
   * @param request 已解码的请求消息
   * @param raw 原始 DataMessage（包含 stream/function/system_bytes 等元信息）
   * @return 错误码与响应消息的 pair
   *         - 成功时：{std::error_code{}, response}
   *         - 失败时：{error_code, TResponse{}} （响应对象会被忽略）
   */
  virtual asio::awaitable<std::pair<std::error_code, TResponse>> handle(
    const TRequest& request,
    const DataMessage& raw) = 0;

  /**
   * @brief 框架调用的入口函数（自动处理编解码）。
   *
   * 执行流程：
   * 1. 解码 msg.body → ii::Item
   * 2. 调用 TRequest::from_item() 转换为强类型请求
   * 3. 调用子类的 handle() 处理业务逻辑
   * 4. 调用 response.to_item() 转换为 ii::Item
   * 5. 编码 ii::Item → bytes
   *
   * 错误处理：
   * - 空 body：返回 core::errc::invalid_argument
   * - decode_one 失败：传播 ii::errc
   * - from_item 返回 nullopt：返回 core::errc::invalid_argument
   * - handle 返回错误：传播错误码，返回空 body
   * - encode 失败：传播 ii::errc
   *
   * @param msg 原始协议层消息
   * @return HandlerResult (std::pair<std::error_code, std::vector<byte>>)
   */
  asio::awaitable<HandlerResult> invoke(const DataMessage& msg) {
    // 步骤 1：解码消息体 → Item
    if (msg.body.empty()) {
      co_return HandlerResult{core::make_error_code(core::errc::invalid_argument), {}};
    }

    ii::Item request_item{ii::List{}};
    std::size_t consumed = 0;
    const auto decode_ec = ii::decode_one(
      secs::core::bytes_view{msg.body.data(), msg.body.size()},
      request_item,
      consumed);

    if (decode_ec) {
      co_return HandlerResult{decode_ec, {}};
    }

    // 步骤 2：Item → TRequest
    auto request_opt = TRequest::from_item(request_item);
    if (!request_opt.has_value()) {
      co_return HandlerResult{core::make_error_code(core::errc::invalid_argument), {}};
    }

    // 步骤 3：调用业务逻辑
    auto [handler_ec, response] = co_await handle(request_opt.value(), msg);
    if (handler_ec) {
      // 业务逻辑错误：返回错误码，消息体置空
      co_return HandlerResult{handler_ec, {}};
    }

    // 步骤 4：TResponse → Item
    ii::Item response_item = response.to_item();

    // 步骤 5：Item → 字节序列
    std::vector<secs::core::byte> response_body;
    const auto encode_ec = ii::encode(response_item, response_body);
    if (encode_ec) {
      co_return HandlerResult{encode_ec, {}};
    }

    co_return HandlerResult{std::error_code{}, std::move(response_body)};
  }
};

/**
 * @brief 注册类型安全的 Handler 到 Router。
 *
 * @tparam THandler TypedHandler 派生类
 * @param router 目标路由器
 * @param stream SECS Stream 号
 * @param function SECS Function 号
 * @param handler Handler 实例 (shared_ptr 确保生命周期)
 */
template <typename THandler>
void register_typed_handler(
    Router& router,
    std::uint8_t stream,
    std::uint8_t function,
    std::shared_ptr<THandler> handler) {
  router.set(stream, function,
    [handler](const DataMessage& msg) -> asio::awaitable<HandlerResult> {
      co_return co_await handler->invoke(msg);
    });
}

}  // 命名空间 secs::protocol
