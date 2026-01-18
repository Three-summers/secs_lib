/**
 * @file sml_ceid_complete.cpp
 * @brief SML + CEID 完整示例：展示如何结合 SML 模板、CEID dispatcher 和变量注入
 *
 * 功能：
 * 1. Equipment 侧：
 *    - 加载 SML 模板文件（定义多个响应模板，带占位符）
 *    - 使用 protocol::Router 的条件响应机制（if 规则）
 *    - 根据收到的 CEID 自动选择响应模板
 *    - 运行时注入动态数据（设备状态、温度、报警等）
 *
 * 2. Host 侧：
 *    - 发送不同 CEID 的 S6F11 请求
 *    - 接收并解析 S6F12 响应
 *    - 验证响应内容
 *
 * 场景：
 *   CEID 0x1001: 设备状态查询
 *   CEID 0x1002: 温度数据查询
 *   CEID 0x1003: 报警信息查询
 *   CEID 0x1004: 生产数据查询
 *
 * 用法：
 *   ./sml_ceid_complete
 */

#include "secs/ii/codec.hpp"
#include "secs/ii/item.hpp"
#include "secs/protocol/session.hpp"
#include "secs/secs1/link.hpp"
#include "secs/secs1/state_machine.hpp"
#include "secs/sml/render.hpp"
#include "secs/sml/runtime.hpp"
#include "secs/utils/ii_helpers.hpp"
#include "secs/utils/protocol_helpers.hpp"

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/this_coro.hpp>

#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using namespace secs;
using namespace std::chrono_literals;

namespace {

constexpr std::uint16_t kDeviceId = 1;

/* ========== 辅助函数：从 Item 中提取 U2 值 ========== */

[[nodiscard]] std::optional<std::uint16_t>
extract_u2_at(const ii::Item &list_item, std::size_t index) noexcept {
    auto *list = list_item.get_if<ii::List>();
    if (!list || index >= list->size()) {
        return std::nullopt;
    }
    auto *u2 = (*list)[index].get_if<ii::U2>();
    if (!u2 || u2->values.empty()) {
        return std::nullopt;
    }
    return u2->values[0];
}

/* ========== 辅助函数：从 S6F11 body 中提取 CEID ========== */

[[nodiscard]] std::optional<std::uint16_t>
extract_ceid_from_s6f11(const ii::Item &body) noexcept {
    // S6F11 结构: <L <U2 DATAID> <U2 CEID> <L ...>>
    // CEID 在 index=1
    return extract_u2_at(body, 1);
}

/* ========== 辅助函数：构造 S6F11 请求 body ========== */

[[nodiscard]] ii::Item
build_s6f11_request(std::uint16_t dataid, std::uint16_t ceid) {
    return ii::Item::list({
        ii::Item::u2({dataid}),
        ii::Item::u2({ceid}),
        ii::Item::list({}), // 参数列表（本示例为空）
    });
}

/* ========== 辅助函数：打印 Item（简化版） ========== */

void print_item(const ii::Item &item, int indent = 0) {
    std::string prefix(indent * 2, ' ');

    if (auto *list = item.get_if<ii::List>()) {
        std::cout << prefix << "<L> (" << list->size() << " items)\n";
        for (const auto &child : *list) {
            print_item(child, indent + 1);
        }
        return;
    }

    if (auto *a = item.get_if<ii::ASCII>()) {
        std::cout << prefix << "<A \"" << a->value << "\">\n";
        return;
    }

    if (auto *u1 = item.get_if<ii::U1>()) {
        std::cout << prefix << "<U1";
        for (auto v : u1->values)
            std::cout << " " << static_cast<unsigned>(v);
        std::cout << ">\n";
        return;
    }

    if (auto *u2 = item.get_if<ii::U2>()) {
        std::cout << prefix << "<U2";
        for (auto v : u2->values)
            std::cout << " " << v;
        std::cout << ">\n";
        return;
    }

    if (auto *u4 = item.get_if<ii::U4>()) {
        std::cout << prefix << "<U4";
        for (auto v : u4->values)
            std::cout << " " << v;
        std::cout << ">\n";
        return;
    }

    if (auto *f4 = item.get_if<ii::F4>()) {
        std::cout << prefix << "<F4";
        for (auto v : f4->values)
            std::cout << " " << v;
        std::cout << ">\n";
        return;
    }

    std::cout << prefix << "<...>\n";
}

/* ========== Equipment 侧：模拟设备数据源 ========== */

struct DeviceData {
    std::string device_name = "EQUIPMENT-001";
    std::uint8_t status_code = 1; // 1=running
    std::uint32_t uptime_seconds = 12345;

    float temp_sensor_1 = 25.5f;
    float temp_sensor_2 = 26.3f;
    float temp_sensor_3 = 24.8f;

    std::uint16_t alarm_count = 2;
    std::string alarm_msg_1 = "High temperature warning";
    std::string alarm_msg_2 = "Low pressure alert";

    std::uint32_t total_count = 10000;
    std::uint32_t good_count = 9850;
    std::uint32_t bad_count = 150;
};

/* ========== Equipment 侧：根据 CEID 填充 RenderContext ========== */

void fill_context_for_ceid(std::uint16_t ceid,
                           std::uint16_t dataid,
                           const DeviceData &data,
                           sml::RenderContext &ctx) {
    // 所有响应都需要 DATAID
    ctx.set("DATAID", ii::Item::u2({dataid}));

    switch (ceid) {
    case 0x1001: // 设备状态查询
        ctx.set("DEVICE_NAME", ii::Item::ascii(data.device_name));
        ctx.set("STATUS_CODE", ii::Item::u1({data.status_code}));
        ctx.set("UPTIME_SECONDS", ii::Item::u4({data.uptime_seconds}));
        break;

    case 0x1002: // 温度数据查询
        ctx.set("TEMP_SENSOR_1", ii::Item::f4({data.temp_sensor_1}));
        ctx.set("TEMP_SENSOR_2", ii::Item::f4({data.temp_sensor_2}));
        ctx.set("TEMP_SENSOR_3", ii::Item::f4({data.temp_sensor_3}));
        {
            float avg = (data.temp_sensor_1 + data.temp_sensor_2 +
                         data.temp_sensor_3) /
                        3.0f;
            ctx.set("TEMP_AVG", ii::Item::f4({avg}));
        }
        break;

    case 0x1003: // 报警信息查询
        ctx.set("ALARM_COUNT", ii::Item::u2({data.alarm_count}));
        ctx.set("ALARM_MSG_1", ii::Item::ascii(data.alarm_msg_1));
        ctx.set("ALARM_MSG_2", ii::Item::ascii(data.alarm_msg_2));
        break;

    case 0x1004: // 生产数据查询
        ctx.set("TOTAL_COUNT", ii::Item::u4({data.total_count}));
        ctx.set("GOOD_COUNT", ii::Item::u4({data.good_count}));
        ctx.set("BAD_COUNT", ii::Item::u4({data.bad_count}));
        {
            float yield = static_cast<float>(data.good_count) /
                          static_cast<float>(data.total_count) * 100.0f;
            ctx.set("YIELD_RATE", ii::Item::f4({yield}));
        }
        break;

    default:
        break;
    }
}

/* ========== 主协程 ========== */

asio::awaitable<int> run() {
    auto ex = co_await asio::this_coro::executor;

    // 1) 创建内存互联的 SECS-I Link（跨平台可运行）
    auto [host_link, eq_link] = secs::secs1::MemoryLink::create(ex);

    secs::secs1::StateMachine host_sm(host_link, kDeviceId);
    secs::secs1::StateMachine eq_sm(eq_link, kDeviceId);

    protocol::SessionOptions host_opt{};
    host_opt.t3 = 3s;
    host_opt.poll_interval = 10ms;
    host_opt.secs1_reverse_bit = false; // Host -> Equipment

    protocol::SessionOptions eq_opt = host_opt;
    eq_opt.secs1_reverse_bit = true; // Equipment -> Host

    protocol::Session host_sess(host_sm, kDeviceId, host_opt);
    protocol::Session eq_sess(eq_sm, kDeviceId, eq_opt);

    // 2) Equipment 侧：加载 SML 模板
    std::cout << "=== Loading SML template ===\n";

    // 读取 SML 文件（假设与可执行文件在同一目录）
    std::ifstream sml_file("sml_ceid_complete.sml");
    if (!sml_file) {
        std::cerr << "ERROR: Cannot open sml_ceid_complete.sml\n";
        std::cerr << "Please ensure the .sml file is in the same directory as "
                     "the executable.\n";
        co_return 1;
    }

    std::stringstream buffer;
    buffer << sml_file.rdbuf();
    std::string sml_source = buffer.str();

    sml::Runtime rt;
    auto load_ec = rt.load(sml_source);
    if (load_ec) {
        std::cerr << "ERROR: Failed to load SML: " << load_ec.message() << "\n";
        co_return 2;
    }

    std::cout << "SML loaded successfully.\n";
    std::cout << "  Messages: " << rt.messages().size() << "\n";
    std::cout << "  Conditions: " << rt.conditions().size() << "\n\n";

    // 3) Equipment 侧：注册 S6F11 handler（使用 SML 条件响应）
    DeviceData device_data;

    eq_sess.router().set(
        6,
        11,
        [&rt, &device_data](const protocol::DataMessage &req)
            -> asio::awaitable<protocol::HandlerResult> {
            std::cout << "[Equipment] Received S6F11\n";

            // 解码请求 body
            if (req.body.empty()) {
                std::cout << "[Equipment] Empty body, rejecting\n";
                co_return protocol::HandlerResult{
                    core::make_error_code(core::errc::invalid_argument), {}};
            }

            auto [dec_ec, decoded] = utils::decode_one_item(
                core::bytes_view{req.body.data(), req.body.size()});
            if (dec_ec) {
                std::cout << "[Equipment] Decode failed: " << dec_ec.message()
                          << "\n";
                co_return protocol::HandlerResult{dec_ec, {}};
            }

            // 提取 DATAID 和 CEID
            auto dataid_opt = extract_u2_at(decoded.item, 0);
            auto ceid_opt = extract_ceid_from_s6f11(decoded.item);

            if (!dataid_opt || !ceid_opt) {
                std::cout << "[Equipment] Invalid S6F11 structure\n";
                co_return protocol::HandlerResult{
                    core::make_error_code(core::errc::invalid_argument), {}};
            }

            std::uint16_t dataid = *dataid_opt;
            std::uint16_t ceid = *ceid_opt;

            std::cout << "[Equipment] DATAID=" << dataid << ", CEID=0x"
                      << std::hex << ceid << std::dec << "\n";

            // 使用 SML Runtime 的条件响应匹配
            auto response_name =
                rt.match_response(req.stream, req.function, decoded.item);

            if (!response_name) {
                std::cout << "[Equipment] No matching response for CEID=0x"
                          << std::hex << ceid << std::dec << "\n";
                co_return protocol::HandlerResult{
                    core::make_error_code(core::errc::invalid_argument), {}};
            }

            std::cout << "[Equipment] Matched response: " << *response_name
                      << "\n";

            // 填充 RenderContext（注入动态数据）
            sml::RenderContext ctx;
            fill_context_for_ceid(ceid, dataid, device_data, ctx);

            // 渲染响应模板
            std::vector<core::byte> response_body;
            auto enc_ec = rt.encode_message_body(*response_name, ctx, response_body);

            if (enc_ec) {
                std::cout << "[Equipment] Render failed: " << enc_ec.message()
                          << "\n";
                co_return protocol::HandlerResult{enc_ec, {}};
            }

            std::cout << "[Equipment] Response rendered, size="
                      << response_body.size() << " bytes\n\n";

            co_return protocol::HandlerResult{std::error_code{},
                                              std::move(response_body)};
        });

    // 4) Equipment 侧：启动接收循环
    asio::co_spawn(ex, eq_sess.async_run(), asio::detached);

    // 5) Host 侧：发送多个不同 CEID 的请求
    std::vector<std::uint16_t> test_ceids = {0x1001, 0x1002, 0x1003, 0x1004};

    for (std::size_t i = 0; i < test_ceids.size(); ++i) {
        std::uint16_t dataid = static_cast<std::uint16_t>(i + 1);
        std::uint16_t ceid = test_ceids[i];

        std::cout << "=== Test " << (i + 1) << ": CEID=0x" << std::hex << ceid
                  << std::dec << " ===\n";

        // 构造请求
        ii::Item request_item = build_s6f11_request(dataid, ceid);
        auto [enc_ec, request_body] = utils::encode_item(request_item);
        if (enc_ec) {
            std::cerr << "[Host] Encode request failed: " << enc_ec.message()
                      << "\n";
            continue;
        }

        std::cout << "[Host] Sending S6F11 (W=1), DATAID=" << dataid
                  << ", CEID=0x" << std::hex << ceid << std::dec << "\n";

        // 发送请求
        auto [req_ec, reply] = co_await utils::async_request_decoded(
            host_sess,
            6,
            11,
            core::bytes_view{request_body.data(), request_body.size()},
            3s);

        if (req_ec) {
            std::cerr << "[Host] Request failed: " << req_ec.message() << "\n";
            continue;
        }

        std::cout << "[Host] Received S" << static_cast<int>(reply.reply.stream)
                  << "F" << static_cast<int>(reply.reply.function) << "\n";

        // 打印响应内容
        if (reply.decoded) {
            std::cout << "[Host] Response body:\n";
            print_item(reply.decoded->item, 1);
        }

        std::cout << "\n";
    }

    // 6) 清理
    host_sess.stop();
    eq_sess.stop();

    std::cout << "=== All tests completed ===\n";
    co_return 0;
}

} // namespace

int main() {
    std::cout << "=== SML + CEID Complete Example ===\n\n";

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
