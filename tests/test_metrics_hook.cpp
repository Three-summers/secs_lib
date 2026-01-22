/**
 * @file test_metrics_hook.cpp
 * @brief metrics hook：回调注入 + 关键埋点冒烟
 */

#include "secs/core/metrics.hpp"

#include "secs/hsms/message.hpp"
#include "secs/ii/codec.hpp"
#include "secs/sml/runtime.hpp"

#include "test_main.hpp"

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

struct Capture final {
    std::mutex mu;
    std::unordered_map<std::string, std::uint64_t> counters;
    std::unordered_map<std::string, std::int64_t> gauges;
    std::unordered_map<std::string, std::vector<std::uint64_t>> histograms;
};

void on_counter(void *user_data, const char *name, std::uint64_t delta) {
    auto *cap = static_cast<Capture *>(user_data);
    if (!cap || !name) {
        return;
    }
    std::lock_guard lk(cap->mu);
    cap->counters[std::string(name)] += delta;
}

void on_gauge(void *user_data, const char *name, std::int64_t value) {
    auto *cap = static_cast<Capture *>(user_data);
    if (!cap || !name) {
        return;
    }
    std::lock_guard lk(cap->mu);
    cap->gauges[std::string(name)] = value;
}

void on_histogram(void *user_data, const char *name, std::uint64_t value) {
    auto *cap = static_cast<Capture *>(user_data);
    if (!cap || !name) {
        return;
    }
    std::lock_guard lk(cap->mu);
    cap->histograms[std::string(name)].push_back(value);
}

void test_metrics_hook_smoke() {
    Capture cap{};

    secs::core::set_metrics_hooks(secs::core::MetricsHooks{
        /*counter=*/&on_counter,
        /*gauge=*/&on_gauge,
        /*histogram=*/&on_histogram,
        /*user_data=*/&cap,
    });

    // 1) SECS-II：encode / decode_one
    {
        const auto item = secs::ii::Item::ascii("HELLO");
        std::vector<secs::core::byte> encoded;
        TEST_EXPECT_OK(secs::ii::encode(item, encoded));

        secs::ii::Item decoded = secs::ii::Item::list({});
        std::size_t consumed = 0;
        TEST_EXPECT_OK(secs::ii::decode_one(
            secs::core::bytes_view{encoded.data(), encoded.size()},
            decoded,
            consumed));
        TEST_EXPECT(consumed > 0u);
    }

    // 2) HSMS：encode_frame / decode_frame
    {
        const auto msg = secs::hsms::make_data_message(
            /*session_id=*/0x0001,
            /*stream=*/1,
            /*function=*/1,
            /*w_bit=*/false,
            /*system_bytes=*/0x11223344,
            secs::core::bytes_view{});

        std::vector<secs::core::byte> frame;
        TEST_EXPECT_OK(secs::hsms::encode_frame(msg, frame));

        secs::hsms::Message out{};
        std::size_t consumed = 0;
        TEST_EXPECT_OK(secs::hsms::decode_frame(
            secs::core::bytes_view{frame.data(), frame.size()}, out, consumed));
        TEST_EXPECT_EQ(consumed, frame.size());
    }

    // 3) SML：parse_sml
    {
        const auto result = secs::sml::parse_sml("S1F1 W <L>.");
        TEST_EXPECT_OK(result.ec);
    }

    // 检查关键计数是否被触发（不强依赖具体数值，避免过度耦合实现细节）。
    {
        std::lock_guard lk(cap.mu);
        TEST_EXPECT(cap.counters.contains("secs.ii.encode.calls"));
        TEST_EXPECT(cap.counters.contains("secs.ii.decode_one.calls"));
        TEST_EXPECT(cap.counters.contains("secs.hsms.encode_frame.calls"));
        TEST_EXPECT(cap.counters.contains("secs.hsms.decode_frame.calls"));
        TEST_EXPECT(cap.counters.contains("secs.sml.parse.calls"));
    }

    // 清理：避免影响本进程后续调用（虽然单测为独立进程，这里仍显式清掉）。
    secs::core::set_metrics_hooks(secs::core::MetricsHooks{});
}

} // namespace

int main() {
    test_metrics_hook_smoke();
    return ::secs::tests::run_and_report();
}

