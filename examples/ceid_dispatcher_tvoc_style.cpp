/**
 * @file ceid_dispatcher_tvoc_style.cpp
 * @brief CEID 示例：仿 TVOC_Secs_App 的消息体结构，演示“按 CEID 路由处理”
 *
 * 背景（来自 tvoc_code 的现有实现）：
 * - `/home/say/code_bak/tvoc_code/src/TVOC_App/TVOC_Secs_App/tvoc_secs_app.c`
 *   中的 S6F11 体结构形如：
 *   <L[3] <U2 DATAID> <U2 CEID> <...>>
 * - 其中 CEID 固定为 0x5000（tvoc_secs_app.h: #define CEID 0x5000）
 *
 * 本示例展示如何用 secs_lib 实现相同“形式”的 CEID 处理，而不引入 GEM：
 * 1) Equipment 侧：用 secs::protocol::CeidDispatcher 解码 body -> 提取 CEID -> 按 CEID 分发 handler
 * 2) Host 侧：发送带 CEID 的 request，并校验 response 中的 CEID 与 request 一致
 *
 * 说明：
 * - 这里为了演示“请求/响应都带 CEID”的场景，示例让 S6F11 使用 W=1，并返回 S6F12
 *   的 body（由 protocol::Session 自动回 secondary）。
 * - 若你的厂商协议中 secondary 不携带 CEID，可在 Host 侧关闭 verify_equal（见代码）。
 *
 * 用法：
 *   ./ceid_dispatcher_tvoc_style
 */

#include "secs/protocol/ceid_dispatcher.hpp"
#include "secs/protocol/session.hpp"
#include "secs/secs1/link.hpp"
#include "secs/secs1/state_machine.hpp"
#include "secs/utils/ceid_helpers.hpp"

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/this_coro.hpp>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace secs;
using namespace std::chrono_literals;

namespace {

constexpr std::uint16_t kDeviceId = 0x1000; // 对齐 tvoc_secs_app.h 的 DEVICE_ID
constexpr std::uint16_t kCeid = 0x5000;     // 对齐 tvoc_secs_app.h 的 CEID（U2）

[[nodiscard]] std::optional<std::uint16_t>
extract_u2_at(const ii::Item &list_item, std::size_t index) noexcept {
    auto *list = list_item.get_if<ii::List>();
    if (!list || index >= list->size()) {
        return std::nullopt;
    }
    auto *u2 = (*list)[index].get_if<ii::U2>();
    if (!u2 || u2->values.size() != 1) {
        return std::nullopt;
    }
    return u2->values[0];
}

[[nodiscard]] std::string format_2f(double v) {
    char buf[64];
    const int n = std::snprintf(buf, sizeof(buf), "%.2f", v);
    if (n <= 0) {
        return "0.00";
    }
    return std::string(buf, static_cast<std::size_t>(n));
}

[[nodiscard]] ii::Item build_tvoc_like_s6f11_body(std::uint16_t dataid,
                                                  std::uint16_t ceid_u2) {
    // 对齐 tvoc 的“第三段”为一个 list（包含描述字符串 + 数值列表（ASCII））。
    ii::Item report = ii::Item::list({
        ii::Item::ascii("valve1: PID|FM|PM|PE MAX|AVG"),
        ii::Item::list({
            ii::Item::ascii(format_2f(12.34)),
            ii::Item::ascii(format_2f(56.78)),
            ii::Item::ascii(format_2f(90.12)),
            ii::Item::ascii(format_2f(34.56)),
            ii::Item::ascii(format_2f(78.90)),
            ii::Item::ascii(format_2f(12.30)),
            ii::Item::ascii(format_2f(45.60)),
            ii::Item::ascii(format_2f(78.00)),
        }),
    });

    return ii::Item::list({
        ii::Item::u2({dataid}),  // DATAID（tvoc: Message_ID）
        ii::Item::u2({ceid_u2}), // CEID（tvoc: 固定 0x5000）
        std::move(report),
    });
}

} // namespace

asio::awaitable<int> run() {
    auto ex = co_await asio::this_coro::executor;

    // 1) 创建一对“内存互联”的 SECS-I Link（跨平台可运行）
    auto [host_link, eq_link] = secs::secs1::MemoryLink::create(ex);

    // 2) 两端分别创建 SECS-I 状态机
    secs::secs1::StateMachine host_sm(host_link, kDeviceId);
    secs::secs1::StateMachine eq_sm(eq_link, kDeviceId);

    // 3) 两端创建统一协议层 Session（注意 R-bit 方向配置）
    protocol::SessionOptions host_opt{};
    host_opt.t3 = 3s;
    host_opt.poll_interval = 10ms;
    host_opt.secs1_reverse_bit = false; // Host -> Equipment（R=0）

    protocol::SessionOptions eq_opt = host_opt;
    eq_opt.secs1_reverse_bit = true; // Equipment -> Host（R=1）

    protocol::Session host_sess(host_sm, kDeviceId, host_opt);
    protocol::Session eq_sess(eq_sm, kDeviceId, eq_opt);

    // 4) Equipment 侧：按 CEID 分发（CEID 在 list 的 index=1）
    auto extractor =
        [](const protocol::DataMessage &,
           const ii::Item &body) -> std::optional<protocol::CeidDispatcher::Ceid> {
        const auto ceid = secs::utils::extract_ceid_s6f11_like(body);
        if (!ceid.has_value()) {
            return std::nullopt;
        }
        return static_cast<protocol::CeidDispatcher::Ceid>(ceid.value());
    };

    auto disp = std::make_shared<protocol::CeidDispatcher>(extractor);

    // CEID=0x5000：返回一个“携带同 CEID 的响应体”，用于演示 request/reply CEID 校验
    disp->set_item(
        kCeid,
        [](protocol::CeidDispatcher::Ceid ceid,
           const ii::Item &req_body,
           const protocol::DataMessage &raw)
            -> asio::awaitable<protocol::CeidDispatcher::ItemHandlerResult> {
            (void)raw;

            // 这里仅示意：回包体也按 <L <U2 DATAID> <U2 CEID> <A "ACK">>
            const auto dataid = extract_u2_at(req_body, 0).value_or(0);
            ii::Item rsp = ii::Item::list({
                ii::Item::u2({dataid}),
                ii::Item::u2({static_cast<std::uint16_t>(ceid)}),
                ii::Item::ascii("ACK"),
            });
            co_return std::pair{std::error_code{}, std::move(rsp)};
        });

    // 未注册 CEID：走 default（仍回显 CEID，便于 Host 侧校验）
    disp->set_default_item(
        [](protocol::CeidDispatcher::Ceid ceid,
           const ii::Item &req_body,
           const protocol::DataMessage &) -> asio::awaitable<
               protocol::CeidDispatcher::ItemHandlerResult> {
            const auto dataid = extract_u2_at(req_body, 0).value_or(0);
            ii::Item rsp = ii::Item::list({
                ii::Item::u2({dataid}),
                ii::Item::u2({static_cast<std::uint16_t>(ceid)}),
                ii::Item::ascii("DEFAULT"),
            });
            co_return std::pair{std::error_code{}, std::move(rsp)};
        });

    protocol::register_ceid_dispatcher(eq_sess.router(), 6, 11, disp);

    // 设备端：启动接收循环（负责收包、路由 handler、回包）
    asio::co_spawn(ex, eq_sess.async_run(), asio::detached);

    // 5) Host 侧：构造 tvoc-like 的 S6F11 body 并发起 W=1 request
    std::uint16_t dataid = 1;
    const ii::Item request_body = build_tvoc_like_s6f11_body(dataid, kCeid);

    std::cout << "[Host] request S6F11(W=1), DATAID=" << dataid
              << " CEID=0x" << std::hex << kCeid << std::dec << "\n";

    // 关键点：verify_equal=true 要求 response 中也能提取出 CEID，且与 request 一致。
    // 如果你的厂商协议 secondary 不带 CEID：把 verify_equal 改成 false。
    auto [ec, out] = co_await secs::utils::async_request_decoded_with_ceid(
        host_sess,
        6,
        11,
        request_body,
        secs::utils::extract_ceid_s6f11_like,
        secs::utils::extract_ceid_s6f11_like,
        true,
        3s);

    if (ec) {
        std::cerr << "[Host] request failed: " << ec.message() << "\n";
        host_sess.stop();
        eq_sess.stop();
        co_return 1;
    }

    std::cout << "[Host] got reply: S" << static_cast<int>(out.reply.stream)
              << "F" << static_cast<int>(out.reply.function)
              << " body_n=" << out.reply.body.size() << "\n";

    if (!out.decoded.has_value()) {
        std::cerr << "[Host] reply body empty or decode failed\n";
        host_sess.stop();
        eq_sess.stop();
        co_return 2;
    }

    auto *list = out.decoded->item.get_if<ii::List>();
    if (!list || list->size() < 3) {
        std::cerr << "[Host] reply item shape mismatch\n";
        host_sess.stop();
        eq_sess.stop();
        co_return 3;
    }

    auto *ascii = (*list)[2].get_if<ii::ASCII>();
    if (!ascii) {
        std::cerr << "[Host] reply ack field is not ASCII\n";
        host_sess.stop();
        eq_sess.stop();
        co_return 4;
    }

    std::cout << "[Host] reply ACK: \"" << ascii->value << "\"\n";
    std::cout << "PASS\n";

    host_sess.stop();
    eq_sess.stop();
    co_return 0;
}

int main() {
    std::cout << "=== CEID Dispatcher Example (TVOC style) ===\n\n";

    asio::io_context ioc;
    int rc = 1;

    asio::co_spawn(
        ioc,
        [&]() -> asio::awaitable<void> {
            rc = co_await run();
            ioc.stop();
        },
        asio::detached);

    ioc.run();
    return rc;
}

