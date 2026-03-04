#include "c_api/internal.hpp"

// ----------------------------- HSMS 连接/会话 -----------------------------

void secs_hsms_data_message_free(secs_hsms_data_message_t *msg) {
    guard_void([&]() {
        if (!msg)
            return;
        if (msg->body) {
            secs_free(msg->body);
        }
        msg->body = nullptr;
        msg->body_n = 0;
    });
}

secs_error_t
secs_hsms_connection_create_memory_duplex(secs_context_t *ctx,
                                          secs_hsms_connection_t **out_client,
                                          secs_hsms_connection_t **out_server) {
    return guard_error([&]() -> secs_error_t {
        if (!ctx || !out_client || !out_server)
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        if (!context_is_alive(ctx))
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        *out_client = nullptr;
        *out_server = nullptr;

        auto c2s = std::make_shared<MemoryChannel>();
        auto s2c = std::make_shared<MemoryChannel>();

        auto client_stream =
            std::make_unique<MemoryStream>(ctx->ioc.get_executor(), s2c, c2s);
        auto server_stream =
            std::make_unique<MemoryStream>(ctx->ioc.get_executor(), c2s, s2c);

        auto *client = new (std::nothrow) secs_hsms_connection(
            secs::hsms::Connection(std::move(client_stream)));
        if (!client)
            return c_api_err(SECS_C_API_OUT_OF_MEMORY);
        auto *server = new (std::nothrow) secs_hsms_connection(
            secs::hsms::Connection(std::move(server_stream)));
        if (!server) {
            delete client;
            return c_api_err(SECS_C_API_OUT_OF_MEMORY);
        }

        *out_client = client;
        *out_server = server;
        return ok();
    });
}

void secs_hsms_connection_destroy(secs_hsms_connection_t *c) {
    guard_void([&]() { delete c; });
}

secs_error_t
secs_hsms_session_create(secs_context_t *ctx,
                         const secs_hsms_session_options_t *options,
                         secs_hsms_session_t **out_sess) {
    return guard_error([&]() -> secs_error_t {
        if (!ctx || !options || !out_sess)
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        if (!context_is_alive(ctx))
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        *out_sess = nullptr;

        auto *h = new (std::nothrow) secs_hsms_session{};
        if (!h)
            return c_api_err(SECS_C_API_OUT_OF_MEMORY);

        context_retain(ctx);
        h->ctx = ctx;
        h->ctx_ref.ptr = ctx;
        const auto opt = make_hsms_options(options);

        h->options = opt;
        try {
            h->sess = std::make_shared<secs::hsms::Session>(
                ctx->ioc.get_executor(), opt);
        } catch (...) {
            delete h;
            return c_api_err(SECS_C_API_EXCEPTION);
        }

        *out_sess = h;
        return ok();
    });
}

static secs_error_t hsms_stop_on_io_thread(secs_hsms_session_t *sess) {
    if (!sess || !sess->ctx || !sess->sess)
        return c_api_err(SECS_C_API_INVALID_ARGUMENT);

    if (!context_is_alive(sess->ctx)) {
        sess->sess->stop();
        return ok();
    }

    if (is_io_thread(sess->ctx)) {
        sess->sess->stop();
        return ok();
    }

    // 复制 shared_ptr：避免 stop() 先 post、再立刻 destroy()
    // 导致回调访问已释放对象（UAF）。
    auto s = sess->sess;
    asio::post(sess->ctx->ioc, [s]() { s->stop(); });
    return ok();
}

secs_error_t secs_hsms_session_stop(secs_hsms_session_t *sess) {
    return guard_error(
        [&]() -> secs_error_t { return hsms_stop_on_io_thread(sess); });
}

secs_error_t secs_hsms_session_open_active_ip(secs_hsms_session_t *sess,
                                              const char *ip,
                                              uint16_t port) {
    return guard_error([&]() -> secs_error_t {
        if (!sess || !sess->ctx || !sess->sess || !ip)
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);

        std::error_code parse_ec{};
        auto addr = asio::ip::make_address(ip, parse_ec);
        if (parse_ec)
            return from_error_code(parse_ec);

        asio::ip::tcp::endpoint ep{addr, port};
        return run_blocking_ec(
            sess->ctx,
            [s = sess->sess, ep]() -> asio::awaitable<std::error_code> {
                co_return co_await s->async_open_active(ep);
            });
    });
}

secs_error_t secs_hsms_session_open_passive_ip(secs_hsms_session_t *sess,
                                               const char *ip,
                                               uint16_t port) {
    return guard_error([&]() -> secs_error_t {
        if (!sess || !sess->ctx || !sess->sess || !ip)
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);

        std::error_code parse_ec{};
        auto addr = asio::ip::make_address(ip, parse_ec);
        if (parse_ec)
            return from_error_code(parse_ec);

        asio::ip::tcp::endpoint ep{addr, port};
        return run_blocking_ec(
            sess->ctx,
            [s = sess->sess, ep]() -> asio::awaitable<std::error_code> {
                asio::ip::tcp::acceptor acceptor{s->executor()};
                std::error_code ec{};

                acceptor.open(ep.protocol(), ec);
                if (ec) {
                    co_return ec;
                }

                acceptor.set_option(asio::ip::tcp::acceptor::reuse_address(true),
                                    ec);
                if (ec) {
                    co_return ec;
                }

                acceptor.bind(ep, ec);
                if (ec) {
                    co_return ec;
                }

                acceptor.listen(asio::socket_base::max_listen_connections, ec);
                if (ec) {
                    co_return ec;
                }

                auto [acc_ec, socket] = co_await acceptor.async_accept(
                    asio::as_tuple(asio::use_awaitable));
                if (acc_ec) {
                    co_return acc_ec;
                }

                co_return co_await s->async_open_passive(std::move(socket));
            });
    });
}

secs_error_t secs_hsms_session_run_active_ip(secs_hsms_session_t *sess,
                                             const char *ip,
                                             uint16_t port) {
    return guard_error([&]() -> secs_error_t {
        if (!sess || !sess->ctx || !sess->sess || !ip) {
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        }

        std::error_code parse_ec{};
        auto addr = asio::ip::make_address(ip, parse_ec);
        if (parse_ec) {
            return from_error_code(parse_ec);
        }
        asio::ip::tcp::endpoint ep{addr, port};

        return run_blocking_ec(
            sess->ctx,
            [s = sess->sess, ep]() -> asio::awaitable<std::error_code> {
                co_return co_await s->async_run_active(ep);
            });
    });
}

secs_error_t secs_hsms_session_run_passive_ip(secs_hsms_session_t *sess,
                                              const char *ip,
                                              uint16_t port) {
    return guard_error([&]() -> secs_error_t {
        if (!sess || !sess->ctx || !sess->sess || !ip) {
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        }

        for (;;) {
            const auto open_ec = secs_hsms_session_open_passive_ip(sess, ip, port);
            if (!secs_error_is_ok(open_ec)) {
                return open_ec;
            }

            // 等待断线：reader_loop_ 退出后才返回，便于继续下一轮 accept。
            const auto wait_ec = run_blocking_ec(
                sess->ctx,
                [s = sess->sess]() -> asio::awaitable<std::error_code> {
                    co_return co_await s->async_wait_reader_stopped(std::nullopt);
                });
            if (!secs_error_is_ok(wait_ec)) {
                return wait_ec;
            }

            if (!sess->options.auto_reconnect) {
                return ok();
            }

            // 退避：使用 HSMS T5（与 C++ async_run_active 的策略保持一致）。
            if (sess->options.t5 != secs::core::duration{}) {
                std::this_thread::sleep_for(sess->options.t5);
            }
        }
    });
}

static secs_error_t hsms_open_with_connection(secs_hsms_session_t *sess,
                                              secs_hsms_connection_t **io_conn,
                                              bool passive) {
    if (!sess || !sess->ctx || !sess->sess)
        return c_api_err(SECS_C_API_INVALID_ARGUMENT);
    if (!io_conn || !*io_conn)
        return c_api_err(SECS_C_API_INVALID_ARGUMENT);

    // 取走连接对象（调用后 io_conn 置空，避免误用）
    secs::hsms::Connection conn = std::move((*io_conn)->conn);
    secs_hsms_connection_destroy(*io_conn);
    *io_conn = nullptr;

    if (passive) {
        return run_blocking_ec(
            sess->ctx,
            [s = sess->sess, conn = std::move(conn)]() mutable
            -> asio::awaitable<std::error_code> {
                co_return co_await s->async_open_passive(std::move(conn));
            });
    }

    return run_blocking_ec(
        sess->ctx,
        [s = sess->sess,
         conn = std::move(conn)]() mutable -> asio::awaitable<std::error_code> {
            co_return co_await s->async_open_active(std::move(conn));
        });
}

secs_error_t
secs_hsms_session_open_active_connection(secs_hsms_session_t *sess,
                                         secs_hsms_connection_t **io_conn) {
    return guard_error([&]() -> secs_error_t {
        return hsms_open_with_connection(sess, io_conn, false);
    });
}

secs_error_t
secs_hsms_session_open_passive_connection(secs_hsms_session_t *sess,
                                          secs_hsms_connection_t **io_conn) {
    return guard_error([&]() -> secs_error_t {
        return hsms_open_with_connection(sess, io_conn, true);
    });
}

secs_error_t secs_hsms_session_is_selected(const secs_hsms_session_t *sess,
                                           int *out_selected) {
    return guard_error([&]() -> secs_error_t {
        if (!sess || !sess->ctx || !sess->sess || !out_selected)
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        *out_selected = 0;

        bool selected = false;
        auto bridge = run_blocking<bool>(
            sess->ctx,
            [s = sess->sess]() -> asio::awaitable<bool> {
                co_return s->is_selected();
            },
            selected);
        if (!secs_error_is_ok(bridge))
            return bridge;

        *out_selected = selected ? 1 : 0;
        return ok();
    });
}

void secs_hsms_session_destroy(secs_hsms_session_t *sess) {
    guard_void([&]() {
        if (!sess)
            return;
        if (!sess->ctx || !sess->sess) {
            delete sess;
            return;
        }

        if (!context_is_alive(sess->ctx)) {
            sess->sess->stop();
            delete sess;
            return;
        }

        // 若在 io 线程调用 destroy，为避免死锁/悬挂协程，改为“异步销毁”。
        if (is_io_thread(sess->ctx)) {
            asio::co_spawn(
                sess->ctx->ioc,
                [sess]() -> asio::awaitable<void> {
                    sess->sess->stop();
                    (void)co_await sess->sess->async_wait_reader_stopped(
                        std::chrono::seconds(5));
                    delete sess;
                },
                asio::detached);
            return;
        }

        // 同步销毁：先 stop，再等待 reader_loop_ 退出，最后释放对象。
        (void)run_blocking_ec(
            sess->ctx, [s = sess->sess]() -> asio::awaitable<std::error_code> {
                s->stop();
                co_return std::error_code{};
            });
        (void)run_blocking_ec(
            sess->ctx, [s = sess->sess]() -> asio::awaitable<std::error_code> {
                co_return co_await s->async_wait_reader_stopped(
                    std::chrono::seconds(5));
            });
        delete sess;
    });
}

secs_error_t secs_hsms_session_linktest(secs_hsms_session_t *sess) {
    return guard_error([&]() -> secs_error_t {
        if (!sess || !sess->ctx || !sess->sess)
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        return run_blocking_ec(
            sess->ctx, [s = sess->sess]() -> asio::awaitable<std::error_code> {
                co_return co_await s->async_linktest();
            });
    });
}

secs_error_t
secs_hsms_session_send_data_auto_system_bytes(secs_hsms_session_t *sess,
                                              uint8_t stream,
                                              uint8_t function,
                                              int w_bit,
                                              const uint8_t *body_bytes,
                                              size_t body_n,
                                              uint32_t *out_system_bytes) {
    return guard_error([&]() -> secs_error_t {
        if (!sess || !sess->ctx || !sess->sess)
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        if (!body_bytes && body_n != 0)
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);

        using Result = std::pair<std::error_code, std::uint32_t>;
        Result result{};
        auto bridge = run_blocking<Result>(
            sess->ctx,
            [s = sess->sess,
             sid = sess->options.session_id,
             stream,
             function,
             w = (w_bit != 0),
             body = bytes_view{reinterpret_cast<const byte *>(body_bytes),
                               body_n}]() -> asio::awaitable<Result> {
                const auto sb = s->allocate_system_bytes();
                const auto msg = secs::hsms::make_data_message(
                    sid, stream, function, w, sb, body);
                auto ec = co_await s->async_send(msg);
                co_return Result{ec, sb};
            },
            result);
        if (!secs_error_is_ok(bridge))
            return bridge;

        if (out_system_bytes) {
            *out_system_bytes = result.second;
        }
        return from_error_code(result.first);
    });
}

secs_error_t
secs_hsms_session_send_data_with_system_bytes(secs_hsms_session_t *sess,
                                              uint8_t stream,
                                              uint8_t function,
                                              int w_bit,
                                              uint32_t system_bytes,
                                              const uint8_t *body_bytes,
                                              size_t body_n) {
    return guard_error([&]() -> secs_error_t {
        if (!sess || !sess->ctx || !sess->sess)
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        if (!body_bytes && body_n != 0)
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);

        return run_blocking_ec(
            sess->ctx,
            [s = sess->sess,
             sid = sess->options.session_id,
             stream,
             function,
             w = (w_bit != 0),
             system_bytes,
             body = bytes_view{reinterpret_cast<const byte *>(body_bytes),
                               body_n}]() -> asio::awaitable<std::error_code> {
                const auto msg = secs::hsms::make_data_message(
                    sid, stream, function, w, system_bytes, body);
                co_return co_await s->async_send(msg);
            });
    });
}

secs_error_t secs_hsms_session_receive_data(secs_hsms_session_t *sess,
                                            uint32_t timeout_ms,
                                            secs_hsms_data_message_t *out_msg) {
    return guard_error([&]() -> secs_error_t {
        if (!sess || !sess->ctx || !sess->sess || !out_msg)
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);

        using Result = std::pair<std::error_code, secs::hsms::Message>;
        Result result{};
        auto bridge = run_blocking<Result>(
            sess->ctx,
            [s = sess->sess, timeout = ms_to_optional_duration(timeout_ms)]()
                -> asio::awaitable<Result> {
                co_return co_await s->async_receive_data(timeout);
            },
            result);
        if (!secs_error_is_ok(bridge))
            return bridge;
        if (result.first)
            return from_error_code(result.first);
        return fill_hsms_out_message(result.second, out_msg);
    });
}

secs_error_t
secs_hsms_session_request_data(secs_hsms_session_t *sess,
                               uint8_t stream,
                               uint8_t function,
                               const uint8_t *body_bytes,
                               size_t body_n,
                               uint32_t timeout_ms,
                               secs_hsms_data_message_t *out_reply) {
    return guard_error([&]() -> secs_error_t {
        if (!sess || !sess->ctx || !sess->sess || !out_reply)
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        if (!body_bytes && body_n != 0)
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);

        using Result = std::pair<std::error_code, secs::hsms::Message>;
        Result result{};

        auto bridge = run_blocking<Result>(
            sess->ctx,
            [s = sess->sess,
             stream,
             function,
             body =
                 bytes_view{reinterpret_cast<const byte *>(body_bytes), body_n},
             timeout = ms_to_optional_duration(
                 timeout_ms)]() -> asio::awaitable<Result> {
                co_return co_await s->async_request_data(
                    stream, function, body, timeout);
            },
            result);
        if (!secs_error_is_ok(bridge))
            return bridge;
        if (result.first)
            return from_error_code(result.first);
        return fill_hsms_out_message(result.second, out_reply);
    });
}
