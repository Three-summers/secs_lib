#include "secs/core/metrics.hpp"

#include <mutex>

namespace secs::core {
namespace {

// 用互斥锁保护 hooks 的读写快照。此前的“双槽 + 原子索引”方案在连续两次
// set_metrics_hooks 时会翻回读者正在拷贝的槽，构成数据竞争（撕裂快照，
// 例如旧 counter 函数指针配上新 user_data）。metrics 发射频率是每消息级
// 而非每字节级，无竞争互斥锁的开销可以接受。
MetricsHooks g_metrics_hooks{};
std::mutex g_metrics_mu;

} // namespace

void set_metrics_hooks(MetricsHooks hooks) noexcept {
    try {
        std::lock_guard lk(g_metrics_mu);
        g_metrics_hooks = hooks;
    } catch (...) {
    }
}

MetricsHooks metrics_hooks() noexcept {
    try {
        std::lock_guard lk(g_metrics_mu);
        return g_metrics_hooks;
    } catch (...) {
        return MetricsHooks{};
    }
}

void metrics_counter(const char *name, std::uint64_t delta) noexcept {
    const auto hooks = metrics_hooks();
    if (!hooks.counter) {
        return;
    }
    try {
        hooks.counter(hooks.user_data, name, delta);
    } catch (...) {
    }
}

void metrics_gauge(const char *name, std::int64_t value) noexcept {
    const auto hooks = metrics_hooks();
    if (!hooks.gauge) {
        return;
    }
    try {
        hooks.gauge(hooks.user_data, name, value);
    } catch (...) {
    }
}

void metrics_histogram(const char *name, std::uint64_t value) noexcept {
    const auto hooks = metrics_hooks();
    if (!hooks.histogram) {
        return;
    }
    try {
        hooks.histogram(hooks.user_data, name, value);
    } catch (...) {
    }
}

} // namespace secs::core
