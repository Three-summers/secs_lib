#include "bench_main.hpp"

#include "secs/c_api.h"

#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

static volatile std::uint64_t g_sink = 0;

static std::vector<std::string> make_var_names(std::size_t n) {
    std::vector<std::string> out;
    out.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        out.push_back("V" + std::to_string(i));
    }
    return out;
}

static void bench_set_u2_single_key() {
    constexpr int loops = 200000;

    secs_sml_render_context_t *ctx = NULL;
    secs_error_t err = secs_sml_render_context_create(&ctx);
    if (!secs_error_is_ok(err)) {
        std::cerr << "secs_sml_render_context_create failed\n";
        return;
    }

    /* 预热/校验 */
    err = secs_sml_render_context_set_u2(ctx, "X", 1);
    if (!secs_error_is_ok(err)) {
        std::cerr << "set_u2 preflight failed\n";
        secs_sml_render_context_destroy(ctx);
        return;
    }

    BENCH_RUN("C API: RenderContext set_u2 (single key)",
              static_cast<std::size_t>(loops) * sizeof(std::uint16_t),
              5,
              {
                  secs_sml_render_context_clear(ctx);
                  std::uint64_t checksum = 0;
                  for (int i = 0; i < loops; ++i) {
                      const secs_error_t e = secs_sml_render_context_set_u2(
                          ctx, "X", static_cast<std::uint16_t>(i));
                      checksum += static_cast<std::uint64_t>(e.value);
                  }
                  g_sink ^= checksum;
              });

    BENCH_RUN("C API: RenderContext set_u2 (sticky enabled, OK)",
              static_cast<std::size_t>(loops) * sizeof(std::uint16_t),
              5,
              {
                  secs_sml_render_context_clear(ctx);
                  (void)secs_sml_render_context_begin(ctx);
                  std::uint64_t checksum = 0;
                  for (int i = 0; i < loops; ++i) {
                      const secs_error_t e = secs_sml_render_context_set_u2(
                          ctx, "X", static_cast<std::uint16_t>(i));
                      checksum += static_cast<std::uint64_t>(e.value);
                  }
                  const secs_error_t end_err = secs_sml_render_context_end(ctx);
                  checksum += static_cast<std::uint64_t>(end_err.value);
                  g_sink ^= checksum;
              });

    /* short-circuit 预热：确保错误被记忆且后续返回同一错误 */
    {
        secs_sml_render_context_clear(ctx);
        (void)secs_sml_render_context_begin(ctx);
        const secs_error_t first = secs_sml_render_context_set_boolean(ctx, "BAD", 2);
        const secs_error_t second = secs_sml_render_context_set_u2(ctx, "X", 1);
        if (secs_error_is_ok(first) || secs_error_is_ok(second) ||
            first.value != second.value ||
            (first.category == NULL) || (second.category == NULL) ||
            std::strcmp(first.category, second.category) != 0) {
            std::cerr << "sticky short-circuit preflight mismatch\n";
        }
        (void)secs_sml_render_context_end(ctx);
    }

    BENCH_RUN("C API: RenderContext short-circuit (sticky)",
              static_cast<std::size_t>(loops) * sizeof(std::uint16_t),
              5,
              {
                  secs_sml_render_context_clear(ctx);
                  (void)secs_sml_render_context_begin(ctx);
                  (void)secs_sml_render_context_set_boolean(ctx, "BAD", 2);
                  std::uint64_t checksum = 0;
                  for (int i = 0; i < loops; ++i) {
                      const secs_error_t e = secs_sml_render_context_set_u2(
                          ctx, "X", static_cast<std::uint16_t>(i));
                      checksum += static_cast<std::uint64_t>(e.value);
                  }
                  const secs_error_t end_err = secs_sml_render_context_end(ctx);
                  checksum += static_cast<std::uint64_t>(end_err.value);
                  g_sink ^= checksum;
              });

    secs_sml_render_context_destroy(ctx);
}

static void bench_set_get_many_vars(std::size_t vars) {
    const auto names = make_var_names(vars);

    secs_sml_render_context_t *ctx = NULL;
    secs_error_t err = secs_sml_render_context_create(&ctx);
    if (!secs_error_is_ok(err)) {
        std::cerr << "secs_sml_render_context_create failed\n";
        return;
    }

    const int set_loops = (vars <= 64) ? 2000 : 400;
    const int get_loops = (vars <= 64) ? 5000 : 1000;

    /* 预热填充：保证 get_* 用例不会走 NOT_FOUND */
    secs_sml_render_context_clear(ctx);
    for (std::size_t i = 0; i < vars; ++i) {
        (void)secs_sml_render_context_set_u2(ctx,
                                            names[i].c_str(),
                                            static_cast<std::uint16_t>(i));
    }

    BENCH_RUN(std::string("C API: RenderContext set_u2 (vars=") +
                  std::to_string(vars) + ")",
              static_cast<std::size_t>(set_loops) * vars * sizeof(std::uint16_t),
              5,
              {
                  secs_sml_render_context_clear(ctx);
                  std::uint64_t checksum = 0;
                  for (int i = 0; i < set_loops; ++i) {
                      for (std::size_t j = 0; j < vars; ++j) {
                          const secs_error_t e =
                              secs_sml_render_context_set_u2(
                                  ctx,
                                  names[j].c_str(),
                                  static_cast<std::uint16_t>(i + (int)j));
                          checksum += static_cast<std::uint64_t>(e.value);
                      }
                  }
                  g_sink ^= checksum;
              });

    BENCH_RUN(std::string("C API: RenderContext get_u2 (vars=") +
                  std::to_string(vars) + ")",
              static_cast<std::size_t>(get_loops) * vars * sizeof(std::uint16_t),
              5,
              {
                  std::uint64_t checksum = 0;
                  for (int i = 0; i < get_loops; ++i) {
                      for (std::size_t j = 0; j < vars; ++j) {
                          std::uint16_t out = 0;
                          const secs_error_t e =
                              secs_sml_render_context_get_u2(
                                  ctx, names[j].c_str(), &out);
                          checksum += static_cast<std::uint64_t>(e.value);
                          checksum += out;
                      }
                  }
                  g_sink ^= checksum;
              });

    secs_sml_render_context_destroy(ctx);
}

static void bench_set_ascii_small() {
    constexpr int loops = 100000;
    const char *kValue = "MODEL-1234";

    secs_sml_render_context_t *ctx = NULL;
    secs_error_t err = secs_sml_render_context_create(&ctx);
    if (!secs_error_is_ok(err)) {
        std::cerr << "secs_sml_render_context_create failed\n";
        return;
    }

    BENCH_RUN("C API: RenderContext set_ascii (len=10)",
              static_cast<std::size_t>(loops) * 10u,
              5,
              {
                  secs_sml_render_context_clear(ctx);
                  std::uint64_t checksum = 0;
                  for (int i = 0; i < loops; ++i) {
                      const secs_error_t e =
                          secs_sml_render_context_set_ascii(ctx, "MDLN", kValue);
                      checksum += static_cast<std::uint64_t>(e.value);
                  }
                  g_sink ^= checksum;
              });

    secs_sml_render_context_destroy(ctx);
}

} // namespace

int main() {
    bench_set_u2_single_key();
    bench_set_get_many_vars(64);
    bench_set_get_many_vars(256);
    bench_set_ascii_small();

    secs::benchmarks::print_results();
    return g_sink == 0 ? 0 : 0;
}
