#include "c_api/internal.hpp"

// ----------------------------- 上下文 -----------------------------

secs_error_t secs_context_create_with_options(secs_context_t **out_ctx,
                                              const secs_context_options_t *opt) {
    return guard_error([&]() -> secs_error_t {
        if (!out_ctx) {
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        }
        *out_ctx = nullptr;

        std::size_t io_threads = 1;
        if (opt && opt->io_threads != 0) {
            io_threads = opt->io_threads;
        }
        if (io_threads == 0) {
            io_threads = 1;
        }

        std::unique_ptr<secs_context> ctx(new (std::nothrow) secs_context{});
        if (!ctx) {
            return c_api_err(SECS_C_API_OUT_OF_MEMORY);
        }

        try {
            ctx->io_threads.reserve(io_threads);
            ctx->io_thread_ids.resize(io_threads);
        } catch (const std::bad_alloc &) {
            return c_api_err(SECS_C_API_OUT_OF_MEMORY);
        } catch (...) {
            return c_api_err(SECS_C_API_EXCEPTION);
        }

        std::vector<std::promise<void>> started;
        std::vector<std::future<void>> started_futures;
        try {
            started.resize(io_threads);
            started_futures.reserve(io_threads);
            for (std::size_t i = 0; i < io_threads; ++i) {
                started_futures.push_back(started[i].get_future());
            }
        } catch (const std::bad_alloc &) {
            return c_api_err(SECS_C_API_OUT_OF_MEMORY);
        } catch (...) {
            return c_api_err(SECS_C_API_EXCEPTION);
        }

        try {
            for (std::size_t i = 0; i < io_threads; ++i) {
                ctx->io_threads.emplace_back(
                    [raw = ctx.get(), i, p = std::move(started[i])]() mutable {
                        raw->io_thread_ids[i] = std::this_thread::get_id();
                        p.set_value();
                        raw->ioc.run();
                    });
            }
        } catch (...) {
            ctx->work.reset();
            ctx->ioc.stop();
            for (auto &t : ctx->io_threads) {
                if (t.joinable()) {
                    t.join();
                }
            }
            return c_api_err(SECS_C_API_EXCEPTION);
        }

        for (auto &f : started_futures) {
            f.wait();
        }

        *out_ctx = ctx.release();
        return ok();
    });
}

secs_error_t secs_context_create(secs_context_t **out_ctx) {
    return secs_context_create_with_options(out_ctx, nullptr);
}

void secs_context_destroy(secs_context_t *ctx) {
    guard_void([&]() {
        if (!ctx) {
            return;
        }

        const auto self = std::this_thread::get_id();
        bool called_from_io_thread = false;
        for (const auto tid : ctx->io_thread_ids) {
            if (tid == self) {
                called_from_io_thread = true;
                break;
            }
        }

        bool expected = false;
        if (!ctx->destroyed.compare_exchange_strong(expected,
                                                    true,
                                                    std::memory_order_acq_rel,
                                                    std::memory_order_acquire)) {
            return;
        }

        ctx->work.reset();
        ctx->ioc.stop();

        if (called_from_io_thread) {
            // 在 io 线程内销毁时，不能自 join，也不能在 run() 尚未完全退出前直接释放
            // context（否则可能 UAF）。改为后台线程统一 join+release。
            context_retain(ctx);
            try {
                std::thread([ctx]() {
                    for (auto &t : ctx->io_threads) {
                        if (t.joinable()) {
                            t.join();
                        }
                    }
                    // 先释放后台线程持有的引用，再释放 destroy() 的“所有权引用”。
                    context_release(ctx);
                    context_release(ctx);
                }).detach();
                return;
            } catch (...) {
                // 极端情况下线程创建失败：保留 destroy 标记并返回。
                // 这里释放额外 retain，避免无界泄漏；基础引用在该极端场景下保留。
                context_release(ctx);
                return;
            }
        }

        for (auto &t : ctx->io_threads) {
            if (t.joinable()) {
                t.join();
            }
        }

        context_release(ctx);
    });
}
