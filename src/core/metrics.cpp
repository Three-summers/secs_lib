#include "secs/core/metrics.hpp"

#include <atomic>
#include <mutex>
#include <type_traits>

namespace secs::core {
namespace {

static_assert(std::is_trivially_copyable_v<MetricsHooks>,
              "MetricsHooks must be trivially copyable for atomic snapshot");

MetricsHooks g_metrics_hooks_slots[2]{MetricsHooks{}, MetricsHooks{}};
std::atomic<unsigned> g_metrics_active_slot{0};
std::mutex g_metrics_set_mu;

} // namespace

void set_metrics_hooks(MetricsHooks hooks) noexcept {
    try {
        std::lock_guard lk(g_metrics_set_mu);
        const auto cur = g_metrics_active_slot.load(std::memory_order_relaxed);
        const auto next = (cur == 0u) ? 1u : 0u;
        g_metrics_hooks_slots[next] = hooks;
        g_metrics_active_slot.store(next, std::memory_order_release);
    } catch (...) {
    }
}

MetricsHooks metrics_hooks() noexcept {
    const auto idx = g_metrics_active_slot.load(std::memory_order_acquire);
    return g_metrics_hooks_slots[(idx == 0u) ? 0u : 1u];
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
