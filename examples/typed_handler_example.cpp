/**
 * @file typed_handler_example.cpp
 * @brief 演示 TypedHandler 的使用方式
 *
 * 本示例展示如何：
 * 1. 定义厂商特定的消息结构 (vendor_messages.hpp)
 * 2. 继承 TypedHandler 实现业务逻辑
 * 3. 使用 register_typed_handler() 注册到 Router
 * 4. 模拟消息处理流程
 */

#include "vendor_messages.hpp"

#include "secs/ii/codec.hpp"
#include "secs/protocol/router.hpp"
#include "secs/protocol/typed_handler.hpp"

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>

#include <iostream>
#include <memory>

using namespace secs::examples;
using namespace secs::protocol;
using namespace secs::ii;
using namespace secs::core;

// ============================================================================
// S1F1 处理器实现
// ============================================================================
class S1F1Handler : public TypedHandler<S1F1Request, S1F2Response> {
 public:
  S1F1Handler(std::string mdln, std::string softrev)
    : mdln_(std::move(mdln)), softrev_(std::move(softrev)) {}

  asio::awaitable<std::pair<std::error_code, S1F2Response>> handle(
      [[maybe_unused]] const S1F1Request& request,
      const DataMessage& raw) override {
    std::cout << "[S1F1Handler] Received S" << static_cast<int>(raw.stream)
              << "F" << static_cast<int>(raw.function)
              << " (system_bytes=" << raw.system_bytes << ")\n";

    S1F2Response response{mdln_, softrev_};
    std::cout << "[S1F1Handler] Responding with MDLN=" << mdln_
              << ", SOFTREV=" << softrev_ << "\n";

    co_return std::pair{std::error_code{}, response};
  }

 private:
  std::string mdln_;
  std::string softrev_;
};

// ============================================================================
// S2F13 处理器实现
// ============================================================================
class S2F13Handler : public TypedHandler<S2F13Request, S2F14Response> {
 public:
  asio::awaitable<std::pair<std::error_code, S2F14Response>> handle(
      const S2F13Request& request,
      const DataMessage& raw) override {
    std::cout << "[S2F13Handler] Received S" << static_cast<int>(raw.stream)
              << "F" << static_cast<int>(raw.function)
              << " requesting " << request.ecids.size() << " ECIDs\n";

    // 模拟设备常量查询
    S2F14Response response;
    for (auto ecid : request.ecids) {
      std::cout << "  - ECID " << ecid;
      std::string value = "VALUE_" + std::to_string(ecid);
      response.ecvs.push_back(value);
      std::cout << " -> " << value << "\n";
    }

    co_return std::pair{std::error_code{}, response};
  }
};

// ============================================================================
// 模拟消息处理
// ============================================================================
void simulate_message_handling(Router& router) {
  asio::io_context ioc;

  // 模拟 S1F1 请求
  asio::co_spawn(ioc, [&]() -> asio::awaitable<void> {
    std::cout << "\n=== Simulating S1F1 (Are You There) ===\n";

    // 编码请求
    auto request_item = S1F1Request{}.to_item();
    std::vector<byte> request_body;
    encode(request_item, request_body);

    // 构造 DataMessage
    DataMessage msg;
    msg.stream = 1;
    msg.function = 1;
    msg.w_bit = true;
    msg.system_bytes = 12345;
    msg.body = request_body;

    // 查找并调用处理器
    auto handler_opt = router.find(1, 1);
    if (handler_opt) {
      auto [ec, response_body] = co_await (*handler_opt)(msg);
      if (!ec) {
        // 解码响应
        Item response_item{List{}};
        std::size_t consumed = 0;
        decode_one(bytes_view{response_body.data(), response_body.size()},
                   response_item, consumed);

        auto response = S1F2Response::from_item(response_item);
        if (response) {
          std::cout << "[Result] MDLN=" << response->mdln
                    << ", SOFTREV=" << response->softrev << "\n";
        }
      } else {
        std::cout << "[Error] " << ec.message() << "\n";
      }
    }

    co_return;
  }, asio::detached);

  // 模拟 S2F13 请求
  asio::co_spawn(ioc, [&]() -> asio::awaitable<void> {
    std::cout << "\n=== Simulating S2F13 (Equipment Constant Request) ===\n";

    // 编码请求
    S2F13Request request;
    request.ecids = {100, 200, 300};
    auto request_item = request.to_item();
    std::vector<byte> request_body;
    encode(request_item, request_body);

    // 构造 DataMessage
    DataMessage msg;
    msg.stream = 2;
    msg.function = 13;
    msg.w_bit = true;
    msg.system_bytes = 67890;
    msg.body = request_body;

    // 查找并调用处理器
    auto handler_opt = router.find(2, 13);
    if (handler_opt) {
      auto [ec, response_body] = co_await (*handler_opt)(msg);
      if (!ec) {
        // 解码响应
        Item response_item{List{}};
        std::size_t consumed = 0;
        decode_one(bytes_view{response_body.data(), response_body.size()},
                   response_item, consumed);

        auto response = S2F14Response::from_item(response_item);
        if (response) {
          std::cout << "[Result] ECVs: ";
          for (const auto& v : response->ecvs) {
            std::cout << v << " ";
          }
          std::cout << "\n";
        }
      } else {
        std::cout << "[Error] " << ec.message() << "\n";
      }
    }

    co_return;
  }, asio::detached);

  ioc.run();
}

int main() {
  std::cout << "TypedHandler Example\n";
  std::cout << "====================\n";

  // 创建 Router（路由器）
  Router router;

  // 注册 S1F1 处理器
  auto s1f1_handler = std::make_shared<S1F1Handler>("ACME-3000", "v2.1.0");
  register_typed_handler(router, 1, 1, s1f1_handler);
  std::cout << "Registered S1F1 handler\n";

  // 注册 S2F13 处理器
  auto s2f13_handler = std::make_shared<S2F13Handler>();
  register_typed_handler(router, 2, 13, s2f13_handler);
  std::cout << "Registered S2F13 handler\n";

  // 模拟消息处理
  simulate_message_handling(router);

  std::cout << "\n=== Example Complete ===\n";
  return 0;
}
