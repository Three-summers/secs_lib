#pragma once

#include <cstdint>

namespace secs::core {

/**
 * @brief 运行时指标 hook（可观测性）。
 *
 * 设计目标：
 * - 不引入第三方依赖（Prometheus/OpenTelemetry 由业务侧适配）；
 * - 默认无成本：未设置 hook 时为 no-op；
 * - 线程安全：hooks 以“原子快照”方式读取，避免半更新；
 * - 低开销：一次原子 load + 空指针分支。
 *
 * 约定：
 * - 回调应做到“不抛异常”；库内部会 best-effort 吞掉异常，避免影响业务路径。
 * - name 建议为静态字符串；若回调需要持久化，请自行拷贝。
 */
using MetricsCounterFn =
    void (*)(void *user_data, const char *name, std::uint64_t delta);
using MetricsGaugeFn =
    void (*)(void *user_data, const char *name, std::int64_t value);
using MetricsHistogramFn =
    void (*)(void *user_data, const char *name, std::uint64_t value);

struct MetricsHooks final {
    MetricsCounterFn counter{nullptr};
    MetricsGaugeFn gauge{nullptr};
    MetricsHistogramFn histogram{nullptr};
    void *user_data{nullptr};
};

void set_metrics_hooks(MetricsHooks hooks) noexcept;
[[nodiscard]] MetricsHooks metrics_hooks() noexcept;

// 轻量 emit API：在库内关键路径直接调用。
void metrics_counter(const char *name, std::uint64_t delta) noexcept;
void metrics_gauge(const char *name, std::int64_t value) noexcept;
void metrics_histogram(const char *name, std::uint64_t value) noexcept;

} // namespace secs::core

