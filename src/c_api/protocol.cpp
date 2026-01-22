#include "c_api/internal.hpp"

// ----------------------------- 协议会话（protocol::Session）
// -----------------------------

void secs_data_message_free(secs_data_message_t *msg) {
    if (!msg)
        return;
    if (msg->body) {
        secs_free(msg->body);
    }
    msg->body = nullptr;
    msg->body_n = 0;
}

static void proto_dump_sink_bridge(void *user,
                                   const char *data,
                                   std::size_t size) noexcept;

static secs::protocol::SessionOptions
make_proto_options(const secs_protocol_session_options_t *options,
                   protocol_state *state) {
    secs::protocol::SessionOptions out{};
    if (!options) {
        return out;
    }
    out.t3 = ms_to_duration_or_default(options->t3_ms, out.t3);
    out.poll_interval =
        ms_to_duration_or_default(options->poll_interval_ms, out.poll_interval);
    if (options->max_pending_requests != 0) {
        out.max_pending_requests = options->max_pending_requests;
    }

    const auto flags = options->dump_flags;
    if ((flags & SECS_PROTOCOL_DUMP_ENABLE) != 0) {
        out.dump.enable = true;

        const bool want_tx = (flags & SECS_PROTOCOL_DUMP_TX) != 0;
        const bool want_rx = (flags & SECS_PROTOCOL_DUMP_RX) != 0;
        if (!want_tx && !want_rx) {
            out.dump.dump_tx = true;
            out.dump.dump_rx = true;
        } else {
            out.dump.dump_tx = want_tx;
            out.dump.dump_rx = want_rx;
        }

        if ((flags & SECS_PROTOCOL_DUMP_COLOR) != 0) {
            out.dump.hsms.enable_color = true;
            out.dump.hsms.hex.enable_color = true;
            out.dump.hsms.item.enable_color = true;
            out.dump.secs1.enable_color = true;
            out.dump.secs1.hex.enable_color = true;
            out.dump.secs1.item.enable_color = true;
        }

        if ((flags & SECS_PROTOCOL_DUMP_SECS2_DECODE) != 0) {
            out.dump.hsms.enable_secs2_decode = true;
            out.dump.secs1.enable_secs2_decode = true;
        }

        if (options->dump_sink && state) {
            state->dump_sink = options->dump_sink;
            state->dump_sink_user = options->dump_sink_user;
            out.dump.sink = proto_dump_sink_bridge;
            out.dump.sink_user = state;
        }
    }

    return out;
}

static void proto_dump_sink_bridge(void *user,
                                   const char *data,
                                   std::size_t size) noexcept {
    auto *state = static_cast<protocol_state *>(user);
    if (!state || !state->dump_sink || !data || size == 0) {
        return;
    }
    try {
        state->dump_sink(state->dump_sink_user, data, size);
    } catch (...) {
        // C callback 不应抛异常；这里吞掉，避免 noexcept 触发 terminate。
    }
}

secs_error_t secs_protocol_session_create_from_hsms(
    secs_context_t *ctx,
    secs_hsms_session_t *hsms_sess,
    uint16_t session_id,
    const secs_protocol_session_options_t *options,
    secs_protocol_session_t **out_sess) {
    return guard_error([&]() -> secs_error_t {
        if (!ctx || !hsms_sess || !hsms_sess->sess || !out_sess) {
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        }
        if (hsms_sess->ctx != ctx) {
            // 协议层与 HSMS 会话必须共享同一个
            // context，否则执行器/线程模型会被破坏。
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        }
        *out_sess = nullptr;

        auto handle = std::unique_ptr<secs_protocol_session>(
            new (std::nothrow) secs_protocol_session{});
        if (!handle) {
            return c_api_err(SECS_C_API_OUT_OF_MEMORY);
        }

        auto state = std::make_shared<protocol_state>();
        state->ctx = ctx;
        state->hsms_keepalive = hsms_sess->sess;
        state->sess = std::make_unique<secs::protocol::Session>(
            *state->hsms_keepalive,
            session_id,
            make_proto_options(options, state.get()));

        // 启动 async_run：保证请求-响应匹配与入站路由在后台持续运行。
        // 注意：协程捕获 shared_ptr，确保即使 C 侧提前 destroy，run_loop 也不会
        // UAF。
        asio::co_spawn(
            ctx->ioc,
            [state]() -> asio::awaitable<void> {
                co_await state->sess->async_run();
            },
            [state](std::exception_ptr) { state->run_done.set(); });
        state->run_spawned.store(true);

        handle->state = std::move(state);
        *out_sess = handle.release();
        return ok();
    });
}

secs_error_t secs_protocol_session_create_from_secs1_serial(
    secs_context_t *ctx,
    const char *serial_path,
    int baud,
    uint16_t device_id,
    int reverse_bit,
    const secs_protocol_session_options_t *options,
    secs_protocol_session_t **out_sess) {
    return guard_error([&]() -> secs_error_t {
        if (!ctx || !serial_path || !out_sess) {
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        }
        *out_sess = nullptr;

#if !defined(ASIO_HAS_SERIAL_PORT)
        (void)baud;
        (void)device_id;
        (void)reverse_bit;
        (void)options;
        return c_api_err(SECS_C_API_INVALID_ARGUMENT);
#else
        auto handle = std::unique_ptr<secs_protocol_session>(
            new (std::nothrow) secs_protocol_session{});
        if (!handle) {
            return c_api_err(SECS_C_API_OUT_OF_MEMORY);
        }

        auto state = std::make_shared<protocol_state>();
        state->ctx = ctx;

        auto [ec, link] = secs::secs1::SerialPortLink::open(
            ctx->ioc.get_executor(), std::string(serial_path), baud);
        if (ec) {
            return from_error_code(ec);
        }

        state->secs1_link =
            std::make_unique<secs::secs1::SerialPortLink>(std::move(link));
        state->secs1_sm = std::make_unique<secs::secs1::StateMachine>(
            *state->secs1_link, device_id);

        auto proto_opt = make_proto_options(options, state.get());
        proto_opt.secs1_reverse_bit = (reverse_bit != 0);
        state->sess = std::make_unique<secs::protocol::Session>(
            *state->secs1_sm, device_id, proto_opt);

        handle->state = std::move(state);
        *out_sess = handle.release();
        return ok();
#endif
    });
}

secs_error_t secs_protocol_session_create_from_secs1_memory_duplex(
    secs_context_t *ctx,
    uint16_t device_id,
    const secs_protocol_session_options_t *options,
    secs_protocol_session_t **out_host,
    secs_protocol_session_t **out_equipment) {
    return guard_error([&]() -> secs_error_t {
        if (!ctx || !out_host || !out_equipment) {
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        }
        *out_host = nullptr;
        *out_equipment = nullptr;

        auto [host_ep, eq_ep] = secs::secs1::MemoryLink::create(
            ctx->ioc.get_executor());

        auto host = std::unique_ptr<secs_protocol_session>(
            new (std::nothrow) secs_protocol_session{});
        if (!host) {
            return c_api_err(SECS_C_API_OUT_OF_MEMORY);
        }
        auto equip = std::unique_ptr<secs_protocol_session>(
            new (std::nothrow) secs_protocol_session{});
        if (!equip) {
            return c_api_err(SECS_C_API_OUT_OF_MEMORY);
        }

        auto host_state = std::make_shared<protocol_state>();
        host_state->ctx = ctx;
        host_state->secs1_link =
            std::make_unique<secs::secs1::MemoryLink::Endpoint>(
                std::move(host_ep));
        host_state->secs1_sm = std::make_unique<secs::secs1::StateMachine>(
            *host_state->secs1_link, device_id);
        auto host_opt = make_proto_options(options, host_state.get());
        host_opt.secs1_reverse_bit = false;
        host_state->sess = std::make_unique<secs::protocol::Session>(
            *host_state->secs1_sm, device_id, host_opt);
        host->state = std::move(host_state);

        auto eq_state = std::make_shared<protocol_state>();
        eq_state->ctx = ctx;
        eq_state->secs1_link =
            std::make_unique<secs::secs1::MemoryLink::Endpoint>(
                std::move(eq_ep));
        eq_state->secs1_sm = std::make_unique<secs::secs1::StateMachine>(
            *eq_state->secs1_link, device_id);
        auto eq_opt = make_proto_options(options, eq_state.get());
        eq_opt.secs1_reverse_bit = true;
        eq_state->sess = std::make_unique<secs::protocol::Session>(
            *eq_state->secs1_sm, device_id, eq_opt);
        equip->state = std::move(eq_state);

        *out_host = host.release();
        *out_equipment = equip.release();
        return ok();
    });
}

static secs_error_t proto_stop_on_io_thread(secs_protocol_session_t *sess) {
    if (!sess || !sess->state || !sess->state->ctx || !sess->state->sess) {
        return c_api_err(SECS_C_API_INVALID_ARGUMENT);
    }

    auto state = sess->state;
    if (is_io_thread(state->ctx)) {
        state->sess->stop();
        return ok();
    }
    asio::post(state->ctx->ioc, [state]() { state->sess->stop(); });
    return ok();
}

secs_error_t secs_protocol_session_stop(secs_protocol_session_t *sess) {
    return guard_error(
        [&]() -> secs_error_t { return proto_stop_on_io_thread(sess); });
}

void secs_protocol_session_destroy(secs_protocol_session_t *sess) {
    guard_void([&]() {
        if (!sess)
            return;

        // 即便 state 为空，也允许释放 handle 本身。
        const auto state = sess->state;
        if (!state || !state->ctx || !state->sess) {
            delete sess;
            return;
        }

        // io 线程内不能阻塞等待；这里仅 stop 并释放 handle，让 run_loop
        // 自行结束并释放 state。
        if (is_io_thread(state->ctx)) {
            state->sess->stop();
            delete sess;
            return;
        }

        const bool need_wait = state->run_spawned.load();

        (void)proto_stop_on_io_thread(sess);
        if (need_wait) {
            (void)run_blocking_ec(
                state->ctx, [state]() -> asio::awaitable<std::error_code> {
                    co_return co_await state->run_done.async_wait(std::nullopt);
                });
        }

        delete sess;
    });
}

secs_error_t secs_protocol_session_poll_once(secs_protocol_session_t *sess,
                                             uint32_t timeout_ms,
                                             int *out_handled) {
    return guard_error([&]() -> secs_error_t {
        if (!sess || !sess->state || !sess->state->ctx || !sess->state->sess ||
            !out_handled) {
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        }

        *out_handled = 0;

        std::error_code ec{};
        const auto state = sess->state;
        const auto bridge = run_blocking<std::error_code>(
            state->ctx,
            [state,
             timeout = ms_to_optional_duration(timeout_ms)]()
                -> asio::awaitable<std::error_code> {
                co_return co_await state->sess->async_poll_once(timeout);
            },
            ec);

        if (!secs_error_is_ok(bridge)) {
            return bridge;
        }

        if (ec == make_error_code(errc::timeout)) {
            *out_handled = 0;
            return ok();
        }
        if (ec) {
            return from_error_code(ec);
        }

        *out_handled = 1;
        return ok();
    });
}

namespace {

[[nodiscard]] secs::protocol::Handler
make_protocol_raw_handler(secs_protocol_handler_fn cb, void *user_data) {
    return [cb, user_data](const secs::protocol::DataMessage &msg)
               -> asio::awaitable<secs::protocol::HandlerResult> {
        uint8_t *out_body = nullptr;
        size_t out_n = 0;
        try {
            secs_data_message_view_t view{};
            view.stream = msg.stream;
            view.function = msg.function;
            view.w_bit = msg.w_bit ? 1 : 0;
            view.system_bytes = msg.system_bytes;
            view.body = reinterpret_cast<const uint8_t *>(msg.body.data());
            view.body_n = msg.body.size();

            secs_error_t cec = cb(user_data, &view, &out_body, &out_n);

            if (!secs_error_is_ok(cec)) {
                if (out_body) {
                    secs_free(out_body);
                }
                co_return secs::protocol::HandlerResult{
                    make_error_code(errc::invalid_argument), {}};
            }

            if (!out_body && out_n != 0) {
                co_return secs::protocol::HandlerResult{
                    make_error_code(errc::invalid_argument), {}};
            }

            std::vector<byte> rsp;
            rsp.resize(out_n);
            if (out_n != 0) {
                std::memcpy(rsp.data(), out_body, out_n);
            }
            if (out_body) {
                secs_free(out_body);
            }
            co_return secs::protocol::HandlerResult{std::error_code{},
                                                    std::move(rsp)};
        } catch (...) {
            if (out_body) {
                secs_free(out_body);
            }
            co_return secs::protocol::HandlerResult{
                make_error_code(errc::invalid_argument), {}};
        }
    };
}

[[nodiscard]] secs::protocol::Handler make_protocol_decoded_handler(
    secs_protocol_decoded_handler_fn cb,
    void *user_data,
    secs::ii::DecodeLimits limits,
    bool strict_consumed) {
    return [cb,
            user_data,
            limits = std::move(limits),
            strict_consumed](const secs::protocol::DataMessage &msg)
               -> asio::awaitable<secs::protocol::HandlerResult> {
        secs_ii_item_t *out_item_body = nullptr;
        try {
            secs_data_message_view_t view{};
            view.stream = msg.stream;
            view.function = msg.function;
            view.w_bit = msg.w_bit ? 1 : 0;
            view.system_bytes = msg.system_bytes;
            view.body = reinterpret_cast<const uint8_t *>(msg.body.data());
            view.body_n = msg.body.size();

            secs::ii::Item decoded{secs::ii::List{}};
            std::size_t consumed = 0;
            const auto dec_ec = secs::ii::decode_one(
                bytes_view{msg.body.data(), msg.body.size()},
                decoded,
                consumed,
                limits);
            if (dec_ec) {
                co_return secs::protocol::HandlerResult{dec_ec, {}};
            }
            if (strict_consumed && consumed != msg.body.size()) {
                co_return secs::protocol::HandlerResult{
                    make_error_code(errc::invalid_argument), {}};
            }

            auto *decoded_handle =
                new (std::nothrow) secs_ii_item(std::move(decoded));
            if (!decoded_handle) {
                co_return secs::protocol::HandlerResult{
                    make_error_code(errc::out_of_memory), {}};
            }
            std::unique_ptr<secs_ii_item_t, void (*)(secs_ii_item_t *)>
                decoded_guard(decoded_handle, secs_ii_item_destroy);

            secs_error_t cec =
                cb(user_data, &view, decoded_handle, &out_item_body);

            std::unique_ptr<secs_ii_item_t, void (*)(secs_ii_item_t *)> rsp_item{
                out_item_body, secs_ii_item_destroy};
            // rsp_item 取得所有权后，避免异常路径/兜底清理重复释放。
            out_item_body = nullptr;

            if (!secs_error_is_ok(cec)) {
                // 回调拒绝处理：不回包；同时确保清理 out_item_body（若有）。
                co_return secs::protocol::HandlerResult{
                    make_error_code(errc::invalid_argument), {}};
            }

            if (!rsp_item) {
                co_return secs::protocol::HandlerResult{std::error_code{}, {}};
            }

            std::vector<byte> rsp;
            const auto enc_ec = secs::ii::encode(rsp_item->item, rsp);
            if (enc_ec) {
                co_return secs::protocol::HandlerResult{enc_ec, {}};
            }

            co_return secs::protocol::HandlerResult{std::error_code{},
                                                    std::move(rsp)};
        } catch (const std::bad_alloc &) {
            if (out_item_body) {
                secs_ii_item_destroy(out_item_body);
            }
            co_return secs::protocol::HandlerResult{
                make_error_code(errc::out_of_memory), {}};
        } catch (...) {
            if (out_item_body) {
                secs_ii_item_destroy(out_item_body);
            }
            co_return secs::protocol::HandlerResult{
                make_error_code(errc::invalid_argument), {}};
        }
    };
}

[[nodiscard]] secs::protocol::Handler make_protocol_sml_auto_reply_handler(
    std::shared_ptr<secs::sml::Runtime> runtime) {
    return [runtime = std::move(runtime)](const secs::protocol::DataMessage &msg)
               -> asio::awaitable<secs::protocol::HandlerResult> {
        try {
            // protocol::Session 的 auto-reply 仅在 W=1 时发送 secondary，因此这里
            // 对 W=0 直接短路，避免不必要的解码开销。
            if (!msg.w_bit) {
                co_return secs::protocol::HandlerResult{std::error_code{}, {}};
            }
            if (msg.function == 0xFFu) {
                co_return secs::protocol::HandlerResult{
                    make_error_code(errc::invalid_argument), {}};
            }

            secs::ii::Item decoded{secs::ii::List{}};
            std::size_t consumed = 0;
            if (!msg.body.empty()) {
                const auto dec_ec = secs::ii::decode_one(
                    bytes_view{msg.body.data(), msg.body.size()}, decoded, consumed);
                if (dec_ec) {
                    co_return secs::protocol::HandlerResult{dec_ec, {}};
                }
            }

            // Data Capture：允许在条件里用 `<pattern>` 抓取 `$NAME`，并把捕获结果
            // 作为渲染上下文注入到响应模板（实现“配置即解析”）。
            secs::sml::RenderContext captured{};
            auto matched = runtime->match_response_with_capture(
                msg.stream, msg.function, decoded, captured);
            if (!matched.has_value()) {
                co_return secs::protocol::HandlerResult{
                    make_error_code(errc::invalid_argument), {}};
            }

            const auto *rsp = runtime->get_message(*matched);
            if (!rsp) {
                co_return secs::protocol::HandlerResult{
                    make_error_code(errc::invalid_argument), {}};
            }

            const auto expected_function =
                static_cast<std::uint8_t>(msg.function + 1u);
            if (rsp->stream != msg.stream || rsp->function != expected_function ||
                rsp->w_bit) {
                co_return secs::protocol::HandlerResult{
                    make_error_code(errc::invalid_argument), {}};
            }

            secs::ii::Item rendered{secs::ii::List{}};
            const auto render_ec =
                secs::sml::render_item(rsp->item, captured, rendered);
            if (render_ec) {
                co_return secs::protocol::HandlerResult{render_ec, {}};
            }

            std::vector<byte> out;
            const auto enc_ec = secs::ii::encode(rendered, out);
            if (enc_ec) {
                co_return secs::protocol::HandlerResult{enc_ec, {}};
            }

            co_return secs::protocol::HandlerResult{std::error_code{},
                                                    std::move(out)};
        } catch (const std::bad_alloc &) {
            co_return secs::protocol::HandlerResult{
                make_error_code(errc::out_of_memory), {}};
        } catch (...) {
            co_return secs::protocol::HandlerResult{
                make_error_code(errc::invalid_argument), {}};
        }
    };
}

} // namespace

secs_error_t secs_protocol_session_set_handler(secs_protocol_session_t *sess,
                                               uint8_t stream,
                                               uint8_t function,
                                               secs_protocol_handler_fn cb,
                                               void *user_data) {
    return guard_error([&]() -> secs_error_t {
        if (!sess || !sess->state || !sess->state->sess || !cb)
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        sess->state->sess->router().set(
            stream, function, make_protocol_raw_handler(cb, user_data));
        return ok();
    });
}

secs_error_t
secs_protocol_session_set_stream_default_handler(secs_protocol_session_t *sess,
                                                 uint8_t stream,
                                                 secs_protocol_handler_fn cb,
                                                 void *user_data) {
    return guard_error([&]() -> secs_error_t {
        if (!sess || !sess->state || !sess->state->sess || !cb) {
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        }
        sess->state->sess->router().set_stream_default(
            stream, make_protocol_raw_handler(cb, user_data));
        return ok();
    });
}

secs_error_t
secs_protocol_session_clear_stream_default_handler(secs_protocol_session_t *sess,
                                                   uint8_t stream) {
    return guard_error([&]() -> secs_error_t {
        if (!sess || !sess->state || !sess->state->sess) {
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        }
        sess->state->sess->router().clear_stream_default(stream);
        return ok();
    });
}

secs_error_t secs_protocol_session_set_decoded_handler(
    secs_protocol_session_t *sess,
    uint8_t stream,
    uint8_t function,
    const secs_ii_decode_limits_t *decode_limits,
    int strict_consumed,
    secs_protocol_decoded_handler_fn cb,
    void *user_data) {
    return guard_error([&]() -> secs_error_t {
        if (!sess || !sess->state || !sess->state->sess || !cb) {
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        }
        sess->state->sess->router().set(
            stream,
            function,
            make_protocol_decoded_handler(cb,
                                          user_data,
                                          make_decode_limits(decode_limits),
                                          strict_consumed != 0));
        return ok();
    });
}

secs_error_t secs_protocol_session_set_decoded_stream_default_handler(
    secs_protocol_session_t *sess,
    uint8_t stream,
    const secs_ii_decode_limits_t *decode_limits,
    int strict_consumed,
    secs_protocol_decoded_handler_fn cb,
    void *user_data) {
    return guard_error([&]() -> secs_error_t {
        if (!sess || !sess->state || !sess->state->sess || !cb) {
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        }
        sess->state->sess->router().set_stream_default(
            stream,
            make_protocol_decoded_handler(cb,
                                          user_data,
                                          make_decode_limits(decode_limits),
                                          strict_consumed != 0));
        return ok();
    });
}

secs_error_t secs_protocol_session_set_decoded_default_handler(
    secs_protocol_session_t *sess,
    const secs_ii_decode_limits_t *decode_limits,
    int strict_consumed,
    secs_protocol_decoded_handler_fn cb,
    void *user_data) {
    return guard_error([&]() -> secs_error_t {
        if (!sess || !sess->state || !sess->state->sess || !cb) {
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        }
        sess->state->sess->router().set_default(
            make_protocol_decoded_handler(cb,
                                          user_data,
                                          make_decode_limits(decode_limits),
                                          strict_consumed != 0));
        return ok();
    });
}

secs_error_t secs_ceid_dispatcher_create_list_path(
    const size_t *indices,
    size_t indices_n,
    const secs_ii_decode_limits_t *decode_limits,
    int strict_consumed,
    secs_ceid_dispatcher_t **out_disp) {
    return guard_error([&]() -> secs_error_t {
        if (!out_disp) {
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        }
        if (!indices && indices_n != 0) {
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        }

        std::vector<std::size_t> path;
        path.reserve(indices_n);
        for (std::size_t i = 0; i < indices_n; ++i) {
            path.push_back(indices[i]);
        }

        secs::protocol::CeidDispatcher::DecodeOptions opt{};
        opt.limits = make_decode_limits(decode_limits);
        opt.strict_consumed = (strict_consumed != 0);

        auto extractor =
            [path = std::move(path)](
                const secs::protocol::DataMessage &,
                const secs::ii::Item &body)
                -> std::optional<secs::protocol::CeidDispatcher::Ceid> {
            const auto ceid = extract_u32_from_list_path(body, path);
            if (!ceid.has_value()) {
                return std::nullopt;
            }
            return static_cast<secs::protocol::CeidDispatcher::Ceid>(
                ceid.value());
        };

        auto dispatcher = std::make_shared<secs::protocol::CeidDispatcher>(
            std::move(extractor), opt);

        auto handle = std::make_unique<secs_ceid_dispatcher>();
        handle->dispatcher = std::move(dispatcher);
        *out_disp = handle.release();
        return ok();
    });
}

void secs_ceid_dispatcher_destroy(secs_ceid_dispatcher_t *disp) {
    guard_void([&]() { delete disp; });
}

secs_error_t secs_ceid_dispatcher_set_handler(secs_ceid_dispatcher_t *disp,
                                              uint32_t ceid,
                                              secs_ceid_handler_fn cb,
                                              void *user_data) {
    return guard_error([&]() -> secs_error_t {
        if (!disp || !disp->dispatcher || !cb) {
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        }

        auto handler =
            [cb, user_data](secs::protocol::CeidDispatcher::Ceid c,
                            const secs::ii::Item &,
                            const secs::protocol::DataMessage &msg)
            -> asio::awaitable<secs::protocol::HandlerResult> {
            uint8_t *out_body = nullptr;
            size_t out_n = 0;
            try {
                secs_data_message_view_t view{};
                view.stream = msg.stream;
                view.function = msg.function;
                view.w_bit = msg.w_bit ? 1 : 0;
                view.system_bytes = msg.system_bytes;
                view.body =
                    reinterpret_cast<const uint8_t *>(msg.body.data());
                view.body_n = msg.body.size();

                secs_error_t cec =
                    cb(user_data, static_cast<uint32_t>(c), &view, &out_body, &out_n);
                if (!secs_error_is_ok(cec)) {
                    if (out_body) {
                        secs_free(out_body);
                    }
                    co_return secs::protocol::HandlerResult{
                        make_error_code(errc::invalid_argument), {}};
                }
                if (!out_body && out_n != 0) {
                    co_return secs::protocol::HandlerResult{
                        make_error_code(errc::invalid_argument), {}};
                }

                std::vector<byte> rsp;
                rsp.resize(out_n);
                if (out_n != 0) {
                    std::memcpy(rsp.data(), out_body, out_n);
                }
                if (out_body) {
                    secs_free(out_body);
                }
                co_return secs::protocol::HandlerResult{std::error_code{},
                                                        std::move(rsp)};
            } catch (...) {
                if (out_body) {
                    secs_free(out_body);
                }
                co_return secs::protocol::HandlerResult{
                    make_error_code(errc::invalid_argument), {}};
            }
        };

        disp->dispatcher->set(
            static_cast<secs::protocol::CeidDispatcher::Ceid>(ceid),
            std::move(handler));
        return ok();
    });
}

secs_error_t secs_ceid_dispatcher_set_default_handler(secs_ceid_dispatcher_t *disp,
                                                      secs_ceid_handler_fn cb,
                                                      void *user_data) {
    return guard_error([&]() -> secs_error_t {
        if (!disp || !disp->dispatcher || !cb) {
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        }

        auto handler =
            [cb, user_data](secs::protocol::CeidDispatcher::Ceid c,
                            const secs::ii::Item &,
                            const secs::protocol::DataMessage &msg)
            -> asio::awaitable<secs::protocol::HandlerResult> {
            uint8_t *out_body = nullptr;
            size_t out_n = 0;
            try {
                secs_data_message_view_t view{};
                view.stream = msg.stream;
                view.function = msg.function;
                view.w_bit = msg.w_bit ? 1 : 0;
                view.system_bytes = msg.system_bytes;
                view.body =
                    reinterpret_cast<const uint8_t *>(msg.body.data());
                view.body_n = msg.body.size();

                secs_error_t cec =
                    cb(user_data, static_cast<uint32_t>(c), &view, &out_body, &out_n);
                if (!secs_error_is_ok(cec)) {
                    if (out_body) {
                        secs_free(out_body);
                    }
                    co_return secs::protocol::HandlerResult{
                        make_error_code(errc::invalid_argument), {}};
                }
                if (!out_body && out_n != 0) {
                    co_return secs::protocol::HandlerResult{
                        make_error_code(errc::invalid_argument), {}};
                }

                std::vector<byte> rsp;
                rsp.resize(out_n);
                if (out_n != 0) {
                    std::memcpy(rsp.data(), out_body, out_n);
                }
                if (out_body) {
                    secs_free(out_body);
                }
                co_return secs::protocol::HandlerResult{std::error_code{},
                                                        std::move(rsp)};
            } catch (...) {
                if (out_body) {
                    secs_free(out_body);
                }
                co_return secs::protocol::HandlerResult{
                    make_error_code(errc::invalid_argument), {}};
            }
        };

        disp->dispatcher->set_default(std::move(handler));
        return ok();
    });
}

secs_error_t secs_ceid_dispatcher_clear_default_handler(
    secs_ceid_dispatcher_t *disp) {
    return guard_error([&]() -> secs_error_t {
        if (!disp || !disp->dispatcher) {
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        }
        disp->dispatcher->clear_default();
        return ok();
    });
}

secs_error_t secs_ceid_dispatcher_erase_handler(secs_ceid_dispatcher_t *disp,
                                                uint32_t ceid) {
    return guard_error([&]() -> secs_error_t {
        if (!disp || !disp->dispatcher) {
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        }
        disp->dispatcher->erase(
            static_cast<secs::protocol::CeidDispatcher::Ceid>(ceid));
        return ok();
    });
}

secs_error_t secs_protocol_session_set_ceid_dispatcher(
    secs_protocol_session_t *sess,
    uint8_t stream,
    uint8_t function,
    secs_ceid_dispatcher_t *disp) {
    return guard_error([&]() -> secs_error_t {
        if (!sess || !sess->state || !sess->state->sess || !disp ||
            !disp->dispatcher) {
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        }

        secs::protocol::register_ceid_dispatcher(
            sess->state->sess->router(),
            stream,
            function,
            disp->dispatcher);
        return ok();
    });
}

secs_error_t
secs_protocol_session_set_default_handler(secs_protocol_session_t *sess,
                                          secs_protocol_handler_fn cb,
                                          void *user_data) {
    return guard_error([&]() -> secs_error_t {
        if (!sess || !sess->state || !sess->state->sess || !cb)
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        sess->state->sess->router().set_default(
            make_protocol_raw_handler(cb, user_data));
        return ok();
    });
}

secs_error_t
secs_protocol_session_clear_default_handler(secs_protocol_session_t *sess) {
    return guard_error([&]() -> secs_error_t {
        if (!sess || !sess->state || !sess->state->sess)
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        sess->state->sess->router().clear_default();
        return ok();
    });
}

secs_error_t
secs_protocol_session_set_sml_default_handler(secs_protocol_session_t *sess,
                                              const secs_sml_runtime_t *rt) {
    return guard_error([&]() -> secs_error_t {
        if (!sess || !sess->state || !sess->state->sess || !rt) {
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        }
        if (!rt->rt.loaded()) {
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        }

        // 拷贝 runtime：避免 C 侧销毁 rt 导致 handler UAF。
        auto runtime =
            std::make_shared<secs::sml::Runtime>(rt->rt); // 可能分配/失败
        sess->state->sess->router().set_default(
            make_protocol_sml_auto_reply_handler(runtime));
        return ok();
    });
}

secs_error_t secs_protocol_session_set_sml_stream_default_handler(
    secs_protocol_session_t *sess,
    uint8_t stream,
    const secs_sml_runtime_t *rt) {
    return guard_error([&]() -> secs_error_t {
        if (!sess || !sess->state || !sess->state->sess || !rt) {
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        }
        if (!rt->rt.loaded()) {
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        }

        // 拷贝 runtime：避免 C 侧销毁 rt 导致 handler UAF。
        auto runtime =
            std::make_shared<secs::sml::Runtime>(rt->rt); // 可能分配/失败

        sess->state->sess->router().set_stream_default(
            stream, make_protocol_sml_auto_reply_handler(runtime));
        return ok();
    });
}

secs_error_t secs_protocol_session_erase_handler(secs_protocol_session_t *sess,
                                                 uint8_t stream,
                                                 uint8_t function) {
    return guard_error([&]() -> secs_error_t {
        if (!sess || !sess->state || !sess->state->sess)
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        sess->state->sess->router().erase(stream, function);
        return ok();
    });
}

secs_error_t secs_protocol_session_send(secs_protocol_session_t *sess,
                                        uint8_t stream,
                                        uint8_t function,
                                        const uint8_t *body_bytes,
                                        size_t body_n) {
    return guard_error([&]() -> secs_error_t {
        if (!sess || !sess->state || !sess->state->ctx || !sess->state->sess)
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        if (!body_bytes && body_n != 0)
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);

        auto state = sess->state;
        return run_blocking_ec(
            state->ctx,
            [state,
             stream,
             function,
             body = bytes_view{reinterpret_cast<const byte *>(body_bytes),
                               body_n}]() -> asio::awaitable<std::error_code> {
                co_return co_await state->sess->async_send(
                    stream, function, body);
            });
    });
}

secs_error_t secs_protocol_session_request(secs_protocol_session_t *sess,
                                           uint8_t stream,
                                           uint8_t function,
                                           const uint8_t *body_bytes,
                                           size_t body_n,
                                           uint32_t timeout_ms,
                                           secs_data_message_t *out_reply) {
    return guard_error([&]() -> secs_error_t {
        if (!sess || !sess->state || !sess->state->ctx || !sess->state->sess ||
            !out_reply)
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        if (!body_bytes && body_n != 0)
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);

        using Result = std::pair<std::error_code, secs::protocol::DataMessage>;
        Result result{};

        auto state = sess->state;
        auto bridge = run_blocking<Result>(
            state->ctx,
            [state,
             stream,
             function,
             body =
                 bytes_view{reinterpret_cast<const byte *>(body_bytes), body_n},
             timeout = ms_to_optional_duration(
                 timeout_ms)]() -> asio::awaitable<Result> {
                co_return co_await state->sess->async_request(
                    stream, function, body, timeout);
            },
            result);
        if (!secs_error_is_ok(bridge))
            return bridge;
        if (result.first)
            return from_error_code(result.first);
        return fill_protocol_out_message(result.second, out_reply);
    });
}

secs_error_t secs_protocol_session_request_with_ceid_list_path(
    secs_protocol_session_t *sess,
    uint8_t stream,
    uint8_t function,
    const uint8_t *body_bytes,
    size_t body_n,
    uint32_t timeout_ms,
    const size_t *ceid_indices,
    size_t ceid_indices_n,
    const secs_ii_decode_limits_t *decode_limits,
    int verify_equal,
    secs_data_message_t *out_reply,
    int *out_has_request_ceid,
    uint32_t *out_request_ceid,
    int *out_has_reply_ceid,
    uint32_t *out_reply_ceid) {
    return guard_error([&]() -> secs_error_t {
        if (!sess || !sess->state || !sess->state->ctx || !sess->state->sess ||
            !out_reply) {
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        }
        if (!body_bytes && body_n != 0) {
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        }
        if (!ceid_indices && ceid_indices_n != 0) {
            return c_api_err(SECS_C_API_INVALID_ARGUMENT);
        }

        // 统一清空输出，避免调用方复用结构体时残留旧数据。
        secs_data_message_free(out_reply);

        if (out_has_request_ceid) {
            *out_has_request_ceid = 0;
        }
        if (out_request_ceid) {
            *out_request_ceid = 0;
        }
        if (out_has_reply_ceid) {
            *out_has_reply_ceid = 0;
        }
        if (out_reply_ceid) {
            *out_reply_ceid = 0;
        }

        std::vector<std::size_t> path;
        path.reserve(ceid_indices_n);
        for (std::size_t i = 0; i < ceid_indices_n; ++i) {
            path.push_back(ceid_indices[i]);
        }

        const auto limits = make_decode_limits(decode_limits);

        std::optional<std::uint32_t> request_ceid;
        if (body_n != 0) {
            secs::ii::Item item{secs::ii::List{}};
            std::size_t consumed = 0;
            const auto dec_ec =
                secs::ii::decode_one(bytes_view{reinterpret_cast<const byte *>(body_bytes),
                                                body_n},
                                     item,
                                     consumed,
                                     limits);
            if (dec_ec) {
                return from_error_code(dec_ec);
            }
            request_ceid = extract_u32_from_list_path(item, path);
        }

        if (request_ceid.has_value()) {
            if (out_has_request_ceid) {
                *out_has_request_ceid = 1;
            }
            if (out_request_ceid) {
                *out_request_ceid = request_ceid.value();
            }
        } else {
            if (verify_equal != 0) {
                return from_error_code(make_error_code(errc::invalid_argument));
            }
        }

        using Result = std::pair<std::error_code, secs::protocol::DataMessage>;
        Result result{};

        auto state = sess->state;
        auto bridge = run_blocking<Result>(
            state->ctx,
            [state,
             stream,
             function,
             body = bytes_view{reinterpret_cast<const byte *>(body_bytes), body_n},
             timeout = ms_to_optional_duration(timeout_ms)]() -> asio::awaitable<Result> {
                co_return co_await state->sess->async_request(
                    stream, function, body, timeout);
            },
            result);
        if (!secs_error_is_ok(bridge)) {
            return bridge;
        }
        if (result.first) {
            return from_error_code(result.first);
        }

        // 即便后续 CEID 校验失败，也把 reply 原样带回（便于排查）。
        const auto fill_ec = fill_protocol_out_message(result.second, out_reply);
        if (!secs_error_is_ok(fill_ec)) {
            return fill_ec;
        }

        std::optional<std::uint32_t> reply_ceid;
        if (!result.second.body.empty()) {
            secs::ii::Item item{secs::ii::List{}};
            std::size_t consumed = 0;
            const auto dec_ec = secs::ii::decode_one(
                bytes_view{result.second.body.data(), result.second.body.size()},
                item,
                consumed,
                limits);
            if (dec_ec) {
                return from_error_code(dec_ec);
            }
            reply_ceid = extract_u32_from_list_path(item, path);
        }

        if (reply_ceid.has_value()) {
            if (out_has_reply_ceid) {
                *out_has_reply_ceid = 1;
            }
            if (out_reply_ceid) {
                *out_reply_ceid = reply_ceid.value();
            }
        }

        if (verify_equal != 0) {
            if (!request_ceid.has_value() || !reply_ceid.has_value() ||
                reply_ceid.value() != request_ceid.value()) {
                return from_error_code(make_error_code(errc::invalid_argument));
            }
        }

        return ok();
    });
}
