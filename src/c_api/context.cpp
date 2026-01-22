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

        ctx->work.reset();
        ctx->ioc.stop();

        for (auto &t : ctx->io_threads) {
            if (t.joinable()) {
                t.join();
            }
        }
        delete ctx;
    });
}

