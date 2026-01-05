# 性能基准（Benchmarks）

> 文档更新：2026-01-05（Codex）

本目录提供一些“可重复跑”的微基准，用于观察核心模块的吞吐/耗时趋势（不是严格的统计学基准框架）。

## 构建

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DSECS_BUILD_BENCHMARKS=ON
cmake --build build --target benchmarks -j
```

## 运行

```bash
./build/benchmarks/bench_core_buffer
./build/benchmarks/bench_secs2_codec
./build/benchmarks/bench_hsms_message
./build/benchmarks/bench_secs1_block
./build/benchmarks/bench_sml_runtime
```

## 说明与建议

- 请尽量使用 `Release` 构建；Debug 会显著扭曲结果。
- 若需要更稳定的数值：固定 CPU 频率/关闭省电、避免后台负载、重复多次取趋势。
