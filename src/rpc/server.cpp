#include "secs/rpc/server.hpp"

#include "secs/core/error.hpp"
#include "secs/hsms/session.hpp"
#include "secs/ii/codec.hpp"
#include "secs/ii/item.hpp"
#include "secs/protocol/session.hpp"
#include "secs/rpc/contracts.hpp"
#include "rpc/internal.hpp"
#include "secs/secs1/serial_port_link.hpp"
#include "secs/secs1/state_machine.hpp"
#include "secs/secs1/timer.hpp"

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/executor_work_guard.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/address.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/post.hpp>

#include <brpc/closure_guard.h>
#include <brpc/server.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace secs::rpc {
namespace {

#ifndef SECS_RPC_PROJECT_VERSION
#define SECS_RPC_PROJECT_VERSION "0.1.0"
#endif

constexpr std::string_view kProjectVersion = SECS_RPC_PROJECT_VERSION;

using Byte = secs::core::byte;
using Duration = secs::core::duration;

std::string normalize_enabled_protocols(std::string_view raw) {
    std::istringstream iss(std::string{raw});
    std::string protocol;
    std::string normalized;
    while (iss >> protocol) {
        // brpc 当前对 http/h2 白名单的处理存在特殊分支，
        // 显式写入会在启动阶段被误判为 unknown protocol。
        if (protocol == "http" || protocol == "h2") {
            continue;
        }
        if (!normalized.empty()) {
            normalized.push_back(' ');
        }
        normalized += protocol;
    }
    return normalized;
}

std::error_code make_brpc_error(int rc) {
    if (rc == 0) {
        return {};
    }
    if (errno != 0) {
        return {errno, std::generic_category()};
    }
    return {rc > 0 ? rc : -rc, std::generic_category()};
}

void fill_ok_status(v1::RpcStatus *status) {
    if (!status) {
        return;
    }
    status->set_ok(true);
    status->clear_error();
}

void fill_rpc_error(v1::RpcError *error,
                    const std::error_code &ec,
                    std::string_view override_message = {}) {
    if (!error) {
        return;
    }
    error->set_category(ec.category().name());
    error->set_value(ec.value());
    if (!override_message.empty()) {
        error->set_message(std::string{override_message});
    } else {
        error->set_message(ec.message());
    }
}

void fill_error_status(v1::RpcStatus *status,
                       const std::error_code &ec,
                       std::string_view override_message = {}) {
    if (!status) {
        return;
    }
    status->set_ok(false);
    fill_rpc_error(status->mutable_error(), ec, override_message);
}

template <typename Response>
void set_error(Response *response,
               const std::error_code &ec,
               std::string_view override_message = {}) {
    fill_error_status(response->mutable_status(), ec, override_message);
}

template <typename RepeatedField, typename Value>
void append_all(RepeatedField *field, const std::vector<Value> &values) {
    for (const auto &value : values) {
        field->Add(value);
    }
}

template <typename Target, typename SourceRange>
std::error_code copy_narrow_signed(const SourceRange &source,
                                   std::vector<Target> &target) {
    target.clear();
    target.reserve(static_cast<std::size_t>(source.size()));
    for (const auto value : source) {
        if (value < static_cast<long long>(std::numeric_limits<Target>::min()) ||
            value > static_cast<long long>(std::numeric_limits<Target>::max())) {
            return std::make_error_code(std::errc::result_out_of_range);
        }
        target.push_back(static_cast<Target>(value));
    }
    return {};
}

template <typename Target, typename SourceRange>
std::error_code copy_narrow_unsigned(const SourceRange &source,
                                     std::vector<Target> &target) {
    target.clear();
    target.reserve(static_cast<std::size_t>(source.size()));
    for (const auto value : source) {
        if (value > static_cast<unsigned long long>(
                        std::numeric_limits<Target>::max())) {
            return std::make_error_code(std::errc::result_out_of_range);
        }
        target.push_back(static_cast<Target>(value));
    }
    return {};
}

std::error_code item_from_proto(const v1::ItemNode &node, secs::ii::Item &out);

void item_to_proto(const secs::ii::Item &item, v1::ItemNode *out) {
    if (!out) {
        return;
    }
    out->Clear();

    std::visit(
        [&](const auto &value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, secs::ii::List>) {
                out->set_type(v1::ITEM_TYPE_LIST);
                for (const auto &child : value) {
                    item_to_proto(child, out->add_items());
                }
            } else if constexpr (std::is_same_v<T, secs::ii::ASCII>) {
                out->set_type(v1::ITEM_TYPE_ASCII);
                out->set_ascii_value(value.value);
            } else if constexpr (std::is_same_v<T, secs::ii::Binary>) {
                out->set_type(v1::ITEM_TYPE_BINARY);
                out->set_binary_value(
                    std::string(reinterpret_cast<const char *>(value.value.data()),
                                value.value.size()));
            } else if constexpr (std::is_same_v<T, secs::ii::Boolean>) {
                out->set_type(v1::ITEM_TYPE_BOOLEAN);
                for (const bool v : value.values) {
                    out->add_bool_values(v);
                }
            } else if constexpr (std::is_same_v<T, secs::ii::I1>) {
                out->set_type(v1::ITEM_TYPE_I1);
                for (const auto v : value.values) {
                    out->add_i1_values(v);
                }
            } else if constexpr (std::is_same_v<T, secs::ii::I2>) {
                out->set_type(v1::ITEM_TYPE_I2);
                for (const auto v : value.values) {
                    out->add_i2_values(v);
                }
            } else if constexpr (std::is_same_v<T, secs::ii::I4>) {
                out->set_type(v1::ITEM_TYPE_I4);
                for (const auto v : value.values) {
                    out->add_i4_values(v);
                }
            } else if constexpr (std::is_same_v<T, secs::ii::I8>) {
                out->set_type(v1::ITEM_TYPE_I8);
                for (const auto v : value.values) {
                    out->add_i8_values(v);
                }
            } else if constexpr (std::is_same_v<T, secs::ii::U1>) {
                out->set_type(v1::ITEM_TYPE_U1);
                for (const auto v : value.values) {
                    out->add_u1_values(v);
                }
            } else if constexpr (std::is_same_v<T, secs::ii::U2>) {
                out->set_type(v1::ITEM_TYPE_U2);
                for (const auto v : value.values) {
                    out->add_u2_values(v);
                }
            } else if constexpr (std::is_same_v<T, secs::ii::U4>) {
                out->set_type(v1::ITEM_TYPE_U4);
                for (const auto v : value.values) {
                    out->add_u4_values(v);
                }
            } else if constexpr (std::is_same_v<T, secs::ii::U8>) {
                out->set_type(v1::ITEM_TYPE_U8);
                for (const auto v : value.values) {
                    out->add_u8_values(v);
                }
            } else if constexpr (std::is_same_v<T, secs::ii::F4>) {
                out->set_type(v1::ITEM_TYPE_F4);
                append_all(out->mutable_f4_values(), value.values);
            } else if constexpr (std::is_same_v<T, secs::ii::F8>) {
                out->set_type(v1::ITEM_TYPE_F8);
                append_all(out->mutable_f8_values(), value.values);
            }
        },
        item.storage());
}

std::error_code item_from_proto(const v1::ItemNode &node, secs::ii::Item &out) {
    switch (node.type()) {
    case v1::ITEM_TYPE_LIST: {
        secs::ii::List items;
        items.reserve(static_cast<std::size_t>(node.items_size()));
        for (const auto &child : node.items()) {
            secs::ii::Item converted = secs::ii::Item::list({});
            auto ec = item_from_proto(child, converted);
            if (ec) {
                return ec;
            }
            items.push_back(std::move(converted));
        }
        out = secs::ii::Item::list(std::move(items));
        return {};
    }
    case v1::ITEM_TYPE_ASCII:
        out = secs::ii::Item::ascii(node.ascii_value());
        return {};
    case v1::ITEM_TYPE_BINARY: {
        std::vector<Byte> value(node.binary_value().begin(), node.binary_value().end());
        out = secs::ii::Item::binary(std::move(value));
        return {};
    }
    case v1::ITEM_TYPE_BOOLEAN: {
        std::vector<bool> values;
        values.reserve(static_cast<std::size_t>(node.bool_values_size()));
        for (const bool v : node.bool_values()) {
            values.push_back(v);
        }
        out = secs::ii::Item::boolean(std::move(values));
        return {};
    }
    case v1::ITEM_TYPE_I1: {
        std::vector<std::int8_t> values;
        auto ec = copy_narrow_signed<std::int8_t>(node.i1_values(), values);
        if (ec) {
            return ec;
        }
        out = secs::ii::Item::i1(std::move(values));
        return {};
    }
    case v1::ITEM_TYPE_I2: {
        std::vector<std::int16_t> values;
        auto ec = copy_narrow_signed<std::int16_t>(node.i2_values(), values);
        if (ec) {
            return ec;
        }
        out = secs::ii::Item::i2(std::move(values));
        return {};
    }
    case v1::ITEM_TYPE_I4: {
        std::vector<std::int32_t> values;
        values.reserve(static_cast<std::size_t>(node.i4_values_size()));
        for (const auto v : node.i4_values()) {
            values.push_back(v);
        }
        out = secs::ii::Item::i4(std::move(values));
        return {};
    }
    case v1::ITEM_TYPE_I8: {
        std::vector<std::int64_t> values;
        values.reserve(static_cast<std::size_t>(node.i8_values_size()));
        for (const auto v : node.i8_values()) {
            values.push_back(v);
        }
        out = secs::ii::Item::i8(std::move(values));
        return {};
    }
    case v1::ITEM_TYPE_U1: {
        std::vector<std::uint8_t> values;
        auto ec = copy_narrow_unsigned<std::uint8_t>(node.u1_values(), values);
        if (ec) {
            return ec;
        }
        out = secs::ii::Item::u1(std::move(values));
        return {};
    }
    case v1::ITEM_TYPE_U2: {
        std::vector<std::uint16_t> values;
        auto ec = copy_narrow_unsigned<std::uint16_t>(node.u2_values(), values);
        if (ec) {
            return ec;
        }
        out = secs::ii::Item::u2(std::move(values));
        return {};
    }
    case v1::ITEM_TYPE_U4: {
        std::vector<std::uint32_t> values;
        values.reserve(static_cast<std::size_t>(node.u4_values_size()));
        for (const auto v : node.u4_values()) {
            values.push_back(v);
        }
        out = secs::ii::Item::u4(std::move(values));
        return {};
    }
    case v1::ITEM_TYPE_U8: {
        std::vector<std::uint64_t> values;
        values.reserve(static_cast<std::size_t>(node.u8_values_size()));
        for (const auto v : node.u8_values()) {
            values.push_back(v);
        }
        out = secs::ii::Item::u8(std::move(values));
        return {};
    }
    case v1::ITEM_TYPE_F4: {
        std::vector<float> values(node.f4_values().begin(), node.f4_values().end());
        out = secs::ii::Item::f4(std::move(values));
        return {};
    }
    case v1::ITEM_TYPE_F8: {
        std::vector<double> values(node.f8_values().begin(), node.f8_values().end());
        out = secs::ii::Item::f8(std::move(values));
        return {};
    }
    case v1::ITEM_TYPE_UNSPECIFIED:
    default:
        return std::make_error_code(std::errc::invalid_argument);
    }
}

std::error_code encode_message_body(const v1::MessageEnvelope &message,
                                    std::vector<Byte> &body_out) {
    body_out.clear();
    if (!message.has_decoded_item()) {
        body_out.assign(message.body().begin(), message.body().end());
        return {};
    }

    secs::ii::Item item = secs::ii::Item::list({});
    auto ec = item_from_proto(message.decoded_item(), item);
    if (ec) {
        return ec;
    }
    return secs::ii::encode(item, body_out);
}

std::error_code fill_envelope_from_data_message(
    const secs::protocol::DataMessage &message,
    v1::MessageEnvelope *out) {
    if (!out) {
        return std::make_error_code(std::errc::invalid_argument);
    }
    out->Clear();
    out->set_stream(message.stream);
    out->set_function(message.function);
    out->set_w_bit(message.w_bit);
    out->set_system_bytes(message.system_bytes);
    out->set_body(std::string(reinterpret_cast<const char *>(message.body.data()),
                              message.body.size()));

    if (!message.body.empty()) {
        secs::ii::Item decoded = secs::ii::Item::list({});
        std::size_t consumed = 0;
        const auto ec = secs::ii::decode_one(
            secs::core::bytes_view{message.body.data(), message.body.size()},
            decoded,
            consumed);
        if (!ec && consumed == message.body.size()) {
            item_to_proto(decoded, out->mutable_decoded_item());
        }
    }
    return {};
}

std::error_code validate_primary_message(const v1::MessageEnvelope &message,
                                         bool allow_wait_bit,
                                         std::string &detail) {
    if (!message.has_stream() || message.stream() > 0x7FU) {
        detail.assign("stream must be in range [0, 127]");
        return std::make_error_code(std::errc::invalid_argument);
    }
    if (!message.has_function() || message.function() == 0U ||
        message.function() > 0xFFU || (message.function() & 0x01U) == 0U) {
        detail.assign("function must be a non-zero primary function");
        return std::make_error_code(std::errc::invalid_argument);
    }
    if (!allow_wait_bit && message.has_w_bit() && message.w_bit()) {
        detail.assign("send does not accept W-bit=true");
        return std::make_error_code(std::errc::invalid_argument);
    }
    return {};
}

std::string next_session_id(std::uint64_t sequence) {
    return "rpc-session-" + std::to_string(sequence);
}

} // namespace

namespace detail {

SessionRecord::SessionRecord(std::string session_id_value,
                             std::string session_name,
                             const v1::TransportConfig &transport_config,
                             const v1::SessionRuntimeConfig &runtime_config)
    : id(std::move(session_id_value)),
      name(std::move(session_name)),
      transport(transport_config),
      runtime(runtime_config) {}

void SessionRecord::clear_last_error_locked() {
    has_last_error = false;
    last_error.Clear();
}

void SessionRecord::set_last_error_locked(const std::error_code &ec,
                                          std::string_view override_message) {
    has_last_error = true;
    fill_rpc_error(&last_error, ec, override_message);
}

v1::SessionInfo SessionRecord::snapshot_locked() {
    v1::SessionInfo info;
    info.set_session_id(id);
    info.set_name(name);
    info.set_state(state);
    info.set_running(state == v1::SESSION_STATE_RUNNING);
    if (hsms) {
        selected_generation_cache = hsms->selected_generation();
    }
    info.set_selected_generation(selected_generation_cache);
    *info.mutable_transport() = transport;
    *info.mutable_runtime() = runtime;
    if (has_last_error) {
        *info.mutable_last_error() = last_error;
    }
    return info;
}

v1::SessionInfo SessionRecord::snapshot() {
    std::lock_guard lk(state_mu);
    return snapshot_locked();
}

std::shared_ptr<SessionRecord>
SessionRegistry::create(const v1::CreateSessionRequest &request,
                        const std::string &session_id,
                        const std::string &session_name) {
    auto record = std::make_shared<SessionRecord>(
        session_id, session_name, request.transport(), request.runtime());
    std::lock_guard lk(mu_);
    sessions_.emplace(session_id, record);
    return record;
}

std::shared_ptr<SessionRecord>
SessionRegistry::find(std::string_view session_id) const {
    std::lock_guard lk(mu_);
    const auto it = sessions_.find(std::string(session_id));
    if (it == sessions_.end()) {
        return nullptr;
    }
    return it->second;
}

std::vector<std::shared_ptr<SessionRecord>> SessionRegistry::list() const {
    std::vector<std::shared_ptr<SessionRecord>> records;
    std::lock_guard lk(mu_);
    records.reserve(sessions_.size());
    for (const auto &[_, record] : sessions_) {
        records.push_back(record);
    }
    std::sort(records.begin(),
              records.end(),
              [](const auto &lhs, const auto &rhs) {
                  return lhs->id < rhs->id;
              });
    return records;
}

bool SessionRegistry::erase(std::string_view session_id) {
    std::lock_guard lk(mu_);
    return sessions_.erase(std::string(session_id)) != 0U;
}

Duration milliseconds_to_duration(std::uint32_t value) {
    return std::chrono::milliseconds{value};
}

secs::protocol::SessionOptions
make_protocol_options(const SessionRecord &record) {
    secs::protocol::SessionOptions options{};
    if (record.runtime.has_poll_interval_ms()) {
        options.poll_interval = milliseconds_to_duration(
            record.runtime.poll_interval_ms());
    }
    if (record.runtime.has_max_pending_requests()) {
        options.max_pending_requests =
            static_cast<std::size_t>(record.runtime.max_pending_requests());
    }
    if (record.runtime.has_request_timeout_ms() &&
        record.runtime.request_timeout_ms() > 0U) {
        options.t3 = milliseconds_to_duration(record.runtime.request_timeout_ms());
    } else if (record.transport.kind() == v1::TRANSPORT_KIND_HSMS &&
               record.transport.has_hsms() &&
               record.transport.hsms().has_t3_ms() &&
               record.transport.hsms().t3_ms() > 0U) {
        options.t3 = milliseconds_to_duration(record.transport.hsms().t3_ms());
    }
    if (record.runtime.has_enable_dump()) {
        options.dump.enable = record.runtime.enable_dump();
    }
    if (record.runtime.has_dump_tx()) {
        options.dump.dump_tx = record.runtime.dump_tx();
    }
    if (record.runtime.has_dump_rx()) {
        options.dump.dump_rx = record.runtime.dump_rx();
    }
    if (record.runtime.has_enable_secs2_decode_in_dump()) {
        options.dump.hsms.enable_secs2_decode =
            record.runtime.enable_secs2_decode_in_dump();
        options.dump.secs1.enable_secs2_decode =
            record.runtime.enable_secs2_decode_in_dump();
    }

    if (record.transport.kind() == v1::TRANSPORT_KIND_SECS1 &&
        record.transport.has_secs1()) {
        const auto &config = record.transport.secs1();
        if (config.has_reverse_bit()) {
            options.secs1_reverse_bit = config.reverse_bit();
        } else if (config.has_equipment_role()) {
            options.secs1_reverse_bit = config.equipment_role();
        }
    }

    return options;
}

secs::hsms::SessionOptions make_hsms_options(const SessionRecord &record) {
    secs::hsms::SessionOptions options{};
    const auto &config = record.transport.hsms();
    if (config.has_session_id()) {
        options.session_id = static_cast<std::uint16_t>(config.session_id());
    }
    if (config.has_t3_ms()) {
        options.t3 = milliseconds_to_duration(config.t3_ms());
    }
    if (config.has_t5_ms()) {
        options.t5 = milliseconds_to_duration(config.t5_ms());
    }
    if (config.has_t6_ms()) {
        options.t6 = milliseconds_to_duration(config.t6_ms());
    }
    if (config.has_t7_ms()) {
        options.t7 = milliseconds_to_duration(config.t7_ms());
    }
    if (config.has_t8_ms()) {
        options.t8 = milliseconds_to_duration(config.t8_ms());
    }
    if (config.has_auto_reconnect()) {
        options.auto_reconnect = config.auto_reconnect();
    }
    if (record.runtime.has_max_pending_requests()) {
        options.max_pending_requests =
            static_cast<std::size_t>(record.runtime.max_pending_requests());
    }
    return options;
}

secs::secs1::Timeouts make_secs1_timeouts(const SessionRecord &record) {
    secs::secs1::Timeouts timeouts{};
    if (record.runtime.has_request_timeout_ms() &&
        record.runtime.request_timeout_ms() > 0U) {
        timeouts.t3_reply =
            milliseconds_to_duration(record.runtime.request_timeout_ms());
    }
    return timeouts;
}

std::optional<Duration> effective_request_timeout(const SessionRecord &record,
                                                  const v1::RequestRequest &request) {
    if (request.has_timeout_ms() && request.timeout_ms() > 0U) {
        return milliseconds_to_duration(request.timeout_ms());
    }
    if (record.runtime.has_request_timeout_ms() &&
        record.runtime.request_timeout_ms() > 0U) {
        return milliseconds_to_duration(record.runtime.request_timeout_ms());
    }
    return std::nullopt;
}

std::error_code validate_transport_config(const v1::TransportConfig &transport,
                                          std::string &detail) {
    switch (transport.kind()) {
    case v1::TRANSPORT_KIND_HSMS: {
        if (!transport.has_hsms()) {
            detail.assign("hsms config is required");
            return std::make_error_code(std::errc::invalid_argument);
        }
        const auto &config = transport.hsms();
        if (!config.has_ip() || config.ip().empty()) {
            detail.assign("hsms.ip is required");
            return std::make_error_code(std::errc::invalid_argument);
        }
        if (!config.has_port() || config.port() == 0U ||
            config.port() > 65535U) {
            detail.assign("hsms.port must be in range [1, 65535]");
            return std::make_error_code(std::errc::invalid_argument);
        }
        std::error_code address_ec{};
        (void)asio::ip::make_address(config.ip(), address_ec);
        if (address_ec) {
            detail.assign("hsms.ip must be a valid IP address");
            return address_ec;
        }
        if (config.has_session_id() && config.session_id() > 0x7FFFU) {
            detail.assign("hsms.session_id must be in range [0, 32767]");
            return std::make_error_code(std::errc::invalid_argument);
        }
        return {};
    }
    case v1::TRANSPORT_KIND_SECS1: {
        if (!transport.has_secs1()) {
            detail.assign("secs1 config is required");
            return std::make_error_code(std::errc::invalid_argument);
        }
        const auto &config = transport.secs1();
        if (!config.has_serial_path() || config.serial_path().empty()) {
            detail.assign("secs1.serial_path is required");
            return std::make_error_code(std::errc::invalid_argument);
        }
        if (config.has_baud() && config.baud() <= 0) {
            detail.assign("secs1.baud must be positive");
            return std::make_error_code(std::errc::invalid_argument);
        }
        return {};
    }
    case v1::TRANSPORT_KIND_UNSPECIFIED:
    default:
        detail.assign("transport.kind is required");
        return std::make_error_code(std::errc::invalid_argument);
    }
}

void clear_runtime_objects(SessionRecord &record) {
    record.protocol.reset();
    record.hsms.reset();
    record.acceptor.reset();
    record.secs1_state_machine.reset();
    record.secs1_link.reset();
}

void cleanup_finished_worker(SessionRecord &record) {
    if (record.io && record.io->stopped()) {
        if (record.worker.joinable()) {
            record.worker.join();
        }
        record.worker = std::thread{};
        record.work_guard.reset();
        record.io.reset();
        clear_runtime_objects(record);
    }
}

std::error_code ensure_worker(SessionRecord &record) {
    cleanup_finished_worker(record);
    if (record.io) {
        return {};
    }

    try {
        record.io = std::make_unique<asio::io_context>();
        record.work_guard.emplace(asio::make_work_guard(*record.io));
        record.worker = std::thread([io = record.io.get()] { io->run(); });
    } catch (...) {
        if (record.worker.joinable()) {
            record.worker.join();
        }
        record.worker = std::thread{};
        record.work_guard.reset();
        record.io.reset();
        clear_runtime_objects(record);
        return std::make_error_code(std::errc::not_enough_memory);
    }
    return {};
}

void stop_runtime(SessionRecord &record) {
    if (record.protocol) {
        record.protocol->stop();
    }
    if (record.hsms) {
        record.hsms->stop();
    }

    record.work_guard.reset();
    if (record.io) {
        record.io->stop();
    }
    if (record.worker.joinable()) {
        record.worker.join();
    }
    record.worker = std::thread{};
    record.io.reset();
    clear_runtime_objects(record);
}

void wait_for_no_active_calls(SessionRecord &record) {
    std::unique_lock lk(record.state_mu);
    record.rpc_calls_cv.wait(lk, [&record] {
        return record.active_rpc_calls == 0U;
    });
}

std::error_code begin_message_call(SessionRecord &record,
                                   secs::protocol::Session *&protocol_out,
                                   asio::io_context *&io_out,
                                   std::string &detail) {
    std::lock_guard lk(record.state_mu);
    if (record.deleted) {
        detail.assign("session not found");
        return std::make_error_code(std::errc::no_such_file_or_directory);
    }
    if (record.state != v1::SESSION_STATE_RUNNING || !record.protocol ||
        !record.io) {
        detail.assign("session is not running");
        return std::make_error_code(std::errc::operation_not_permitted);
    }
    if (!record.accepting_rpc_calls) {
        detail.assign("session is stopping");
        return std::make_error_code(std::errc::operation_canceled);
    }

    ++record.active_rpc_calls;
    protocol_out = record.protocol.get();
    io_out = record.io.get();
    return {};
}

void end_message_call(SessionRecord &record) noexcept {
    std::lock_guard lk(record.state_mu);
    if (record.active_rpc_calls == 0U) {
        return;
    }
    --record.active_rpc_calls;
    if (record.active_rpc_calls == 0U) {
        record.rpc_calls_cv.notify_all();
    }
}

bool requires_serial_message_calls(const SessionRecord &record) noexcept {
    return record.transport.kind() == v1::TRANSPORT_KIND_SECS1;
}

void shutdown_record(SessionRecord &record) {
    std::lock_guard invoke_lk(record.invoke_mu);
    {
        std::lock_guard state_lk(record.state_mu);
        record.accepting_rpc_calls = false;
        if (record.hsms) {
            record.selected_generation_cache = record.hsms->selected_generation();
        }
        if (record.state == v1::SESSION_STATE_RUNNING) {
            record.state = v1::SESSION_STATE_STOPPED;
        }
    }
    wait_for_no_active_calls(record);
    stop_runtime(record);
}

void shutdown_registry(SessionRegistry &registry) {
    for (const auto &record : registry.list()) {
        shutdown_record(*record);
    }
}

void mark_background_exit(const std::shared_ptr<SessionRecord> &record,
                          const std::error_code &ec) {
    {
        std::lock_guard lk(record->state_mu);
        if (record->hsms) {
            record->selected_generation_cache = record->hsms->selected_generation();
        }
        record->state = v1::SESSION_STATE_STOPPED;
        record->accepting_rpc_calls = false;
        if (ec && ec != std::make_error_code(std::errc::operation_canceled) &&
            ec != secs::core::make_error_code(secs::core::errc::cancelled)) {
            record->set_last_error_locked(ec);
        }
    }
    record->work_guard.reset();
    if (record->io) {
        record->io->stop();
    }
}

template <typename Factory>
std::error_code launch_background_runner(const std::shared_ptr<SessionRecord> &record,
                                         Factory &&factory) {
    try {
        asio::co_spawn(
            record->io->get_executor(),
            [record, factory = std::forward<Factory>(factory)]() mutable
                -> asio::awaitable<void> {
                std::error_code ec{};
                try {
                    ec = co_await factory();
                } catch (...) {
                    ec = std::make_error_code(std::errc::io_error);
                }
                mark_background_exit(record, ec);
                co_return;
            },
            asio::detached);
    } catch (...) {
        return std::make_error_code(std::errc::not_enough_memory);
    }
    return {};
}

template <typename T, typename Factory>
T run_on_executor(asio::io_context &io, Factory &&factory) {
    auto promise = std::make_shared<std::promise<T>>();
    auto future = promise->get_future();

    asio::co_spawn(
        io.get_executor(),
        [promise, factory = std::forward<Factory>(factory)]() mutable
            -> asio::awaitable<void> {
            try {
                promise->set_value(co_await factory());
            } catch (...) {
                promise->set_exception(std::current_exception());
            }
            co_return;
        },
        asio::detached);

    return future.get();
}

std::error_code start_hsms_session(const std::shared_ptr<SessionRecord> &record,
                                   std::string &detail) {
    cleanup_finished_worker(*record);
    auto ec = ensure_worker(*record);
    if (ec) {
        detail.assign("failed to start session worker");
        return ec;
    }

    const auto &config = record->transport.hsms();
    std::error_code address_ec{};
    const auto address = asio::ip::make_address(config.ip(), address_ec);
    if (address_ec) {
        detail.assign("failed to parse hsms.ip");
        return address_ec;
    }

    record->hsms = std::make_unique<secs::hsms::Session>(
        record->io->get_executor(), make_hsms_options(*record));
    const auto protocol_options = make_protocol_options(*record);
    record->protocol = std::make_unique<secs::protocol::Session>(
        *record->hsms,
        static_cast<std::uint16_t>(config.session_id()),
        protocol_options);

    if (config.has_passive() && config.passive()) {
        record->acceptor =
            std::make_unique<asio::ip::tcp::acceptor>(record->io->get_executor());
        auto &acceptor = *record->acceptor;
        acceptor.open(address.is_v4() ? asio::ip::tcp::v4()
                                      : asio::ip::tcp::v6(),
                      ec);
        if (ec) {
            detail.assign("failed to open hsms passive acceptor");
            return ec;
        }
        acceptor.set_option(asio::ip::tcp::acceptor::reuse_address(true), ec);
        if (ec) {
            detail.assign("failed to configure hsms passive acceptor");
            return ec;
        }
        acceptor.bind(asio::ip::tcp::endpoint(address,
                                              static_cast<std::uint16_t>(
                                                  config.port())),
                      ec);
        if (ec) {
            detail.assign("failed to bind hsms passive acceptor");
            return ec;
        }
        acceptor.listen(asio::socket_base::max_listen_connections, ec);
        if (ec) {
            detail.assign("failed to listen on hsms passive acceptor");
            return ec;
        }

        return launch_background_runner(
            record,
            [record]() -> asio::awaitable<std::error_code> {
                co_return co_await record->hsms->async_run_passive(*record->acceptor);
            });
    }

    const asio::ip::tcp::endpoint endpoint(
        address, static_cast<std::uint16_t>(config.port()));
    return launch_background_runner(
        record,
        [record, endpoint]() -> asio::awaitable<std::error_code> {
            co_return co_await record->hsms->async_run_active(endpoint);
        });
}

std::error_code start_secs1_session(const std::shared_ptr<SessionRecord> &record,
                                    std::string &detail) {
    cleanup_finished_worker(*record);
    auto ec = ensure_worker(*record);
    if (ec) {
        detail.assign("failed to start session worker");
        return ec;
    }

    const auto &config = record->transport.secs1();
    const int baud = config.has_baud() ? config.baud() : 9600;
    auto [open_ec, link] =
        secs::secs1::SerialPortLink::open(record->io->get_executor(),
                                          config.serial_path(),
                                          baud);
    if (open_ec) {
        detail.assign("failed to open secs1 serial path");
        return open_ec;
    }

    auto link_ptr =
        std::make_unique<secs::secs1::SerialPortLink>(std::move(link));
    auto *link_raw = link_ptr.get();

    std::optional<std::uint16_t> device_id = std::nullopt;
    if (config.has_device_id()) {
        device_id = static_cast<std::uint16_t>(config.device_id());
    }

    record->secs1_link = std::move(link_ptr);
    record->secs1_state_machine = std::make_unique<secs::secs1::StateMachine>(
        *link_raw, device_id, make_secs1_timeouts(*record));
    record->protocol = std::make_unique<secs::protocol::Session>(
        *record->secs1_state_machine,
        device_id.value_or(0U),
        make_protocol_options(*record));
    return {};
}

std::error_code start_session_runtime(const std::shared_ptr<SessionRecord> &record,
                                      std::string &detail) {
    switch (record->transport.kind()) {
    case v1::TRANSPORT_KIND_HSMS:
        return start_hsms_session(record, detail);
    case v1::TRANSPORT_KIND_SECS1:
        return start_secs1_session(record, detail);
    case v1::TRANSPORT_KIND_UNSPECIFIED:
    default:
        detail.assign("unsupported transport kind");
        return std::make_error_code(std::errc::invalid_argument);
    }
}

} // namespace detail

namespace {

using detail::SessionRecord;
using detail::SessionRegistry;
using detail::begin_message_call;
using detail::cleanup_finished_worker;
using detail::end_message_call;
using detail::requires_serial_message_calls;
using detail::shutdown_registry;
using detail::stop_runtime;
using detail::validate_transport_config;
using detail::wait_for_no_active_calls;

class ActiveRpcCallGuard final {
public:
    explicit ActiveRpcCallGuard(SessionRecord &record) noexcept
        : record_(&record) {}

    ActiveRpcCallGuard(const ActiveRpcCallGuard &) = delete;
    ActiveRpcCallGuard &operator=(const ActiveRpcCallGuard &) = delete;

    ActiveRpcCallGuard(ActiveRpcCallGuard &&other) noexcept
        : record_(std::exchange(other.record_, nullptr)) {}

    ActiveRpcCallGuard &operator=(ActiveRpcCallGuard &&other) noexcept {
        if (this == &other) {
            return *this;
        }
        release();
        record_ = std::exchange(other.record_, nullptr);
        return *this;
    }

    ~ActiveRpcCallGuard() { release(); }

    void release() noexcept {
        if (!record_) {
            return;
        }
        end_message_call(*record_);
        record_ = nullptr;
    }

private:
    SessionRecord *record_{nullptr};
};

class LibraryServiceImpl final : public v1::LibraryService {
public:
    void GetLibraryInfo(::google::protobuf::RpcController *controller,
                        const v1::GetLibraryInfoRequest *request,
                        v1::GetLibraryInfoResponse *response,
                        ::google::protobuf::Closure *done) override {
        brpc::ClosureGuard done_guard(done);
        (void)controller;
        (void)request;

        fill_ok_status(response->mutable_status());
        response->set_version(std::string{kProjectVersion});
        response->add_supported_transports("HSMS");
        response->add_supported_transports("SECS-I");
        response->add_supported_features("protocol-session");
        response->add_supported_features("session-service-v1");
        response->add_supported_features("messaging-service-v1");
        response->add_supported_features("grpc-compatible-protocol");
        response->add_supported_features("itemnode");
    }
};

class SessionServiceImpl final : public v1::SessionService {
public:
    explicit SessionServiceImpl(SessionRegistry &registry)
        : registry_(registry) {}

    void CreateSession(::google::protobuf::RpcController *controller,
                       const v1::CreateSessionRequest *request,
                       v1::CreateSessionResponse *response,
                       ::google::protobuf::Closure *done) override {
        brpc::ClosureGuard done_guard(done);
        (void)controller;

        std::string detail;
        if (!request->has_transport()) {
            set_error(response,
                      std::make_error_code(std::errc::invalid_argument),
                      "transport is required");
            return;
        }

        const auto ec = validate_transport_config(request->transport(), detail);
        if (ec) {
            set_error(response, ec, detail);
            return;
        }

        const auto sequence = next_sequence_.fetch_add(1U, std::memory_order_relaxed) + 1U;
        const auto session_id = next_session_id(sequence);
        const auto session_name =
            request->has_name() && !request->name().empty() ? request->name()
                                                            : session_id;

        auto record = registry_.create(*request, session_id, session_name);
        fill_ok_status(response->mutable_status());
        *response->mutable_session() = record->snapshot();
    }

    void GetSession(::google::protobuf::RpcController *controller,
                    const v1::GetSessionRequest *request,
                    v1::GetSessionResponse *response,
                    ::google::protobuf::Closure *done) override {
        brpc::ClosureGuard done_guard(done);
        (void)controller;

        auto record = registry_.find(request->session_id());
        if (!record) {
            set_error(response,
                      std::make_error_code(std::errc::no_such_file_or_directory),
                      "session not found");
            return;
        }

        {
            std::lock_guard state_lk(record->state_mu);
            if (record->deleted) {
                set_error(response,
                          std::make_error_code(std::errc::no_such_file_or_directory),
                          "session not found");
                return;
            }
            fill_ok_status(response->mutable_status());
            *response->mutable_session() = record->snapshot_locked();
        }
    }

    void ListSessions(::google::protobuf::RpcController *controller,
                      const v1::ListSessionsRequest *request,
                      v1::ListSessionsResponse *response,
                      ::google::protobuf::Closure *done) override {
        brpc::ClosureGuard done_guard(done);
        (void)controller;
        (void)request;

        fill_ok_status(response->mutable_status());
        for (const auto &record : registry_.list()) {
            std::lock_guard state_lk(record->state_mu);
            if (record->deleted) {
                continue;
            }
            *response->add_sessions() = record->snapshot_locked();
        }
    }

    void StartSession(::google::protobuf::RpcController *controller,
                      const v1::StartSessionRequest *request,
                      v1::StartSessionResponse *response,
                      ::google::protobuf::Closure *done) override {
        brpc::ClosureGuard done_guard(done);
        (void)controller;

        auto record = registry_.find(request->session_id());
        if (!record) {
            set_error(response,
                      std::make_error_code(std::errc::no_such_file_or_directory),
                      "session not found");
            return;
        }

        std::lock_guard invoke_lk(record->invoke_mu);
        {
            std::lock_guard state_lk(record->state_mu);
            if (record->deleted) {
                set_error(response,
                          std::make_error_code(std::errc::no_such_file_or_directory),
                          "session not found");
                return;
            }
            if (record->state == v1::SESSION_STATE_RUNNING) {
                fill_ok_status(response->mutable_status());
                *response->mutable_session() = record->snapshot_locked();
                return;
            }
            record->clear_last_error_locked();
            record->accepting_rpc_calls = false;
        }

        wait_for_no_active_calls(*record);
        cleanup_finished_worker(*record);

        std::string detail;
        const auto ec = detail::start_session_runtime(record, detail);
        if (ec) {
            stop_runtime(*record);
            {
                std::lock_guard state_lk(record->state_mu);
                record->state = v1::SESSION_STATE_STOPPED;
                record->accepting_rpc_calls = false;
                record->set_last_error_locked(ec, detail);
                fill_error_status(response->mutable_status(), ec, detail);
                *response->mutable_session() = record->snapshot_locked();
            }
            return;
        }

        {
            std::lock_guard state_lk(record->state_mu);
            record->state = v1::SESSION_STATE_RUNNING;
            record->accepting_rpc_calls = true;
            fill_ok_status(response->mutable_status());
            *response->mutable_session() = record->snapshot_locked();
        }
    }

    void StopSession(::google::protobuf::RpcController *controller,
                     const v1::StopSessionRequest *request,
                     v1::StopSessionResponse *response,
                     ::google::protobuf::Closure *done) override {
        brpc::ClosureGuard done_guard(done);
        (void)controller;

        auto record = registry_.find(request->session_id());
        if (!record) {
            set_error(response,
                      std::make_error_code(std::errc::no_such_file_or_directory),
                      "session not found");
            return;
        }

        std::lock_guard invoke_lk(record->invoke_mu);
        bool was_running = false;
        {
            std::lock_guard state_lk(record->state_mu);
            if (record->deleted) {
                set_error(response,
                          std::make_error_code(std::errc::no_such_file_or_directory),
                          "session not found");
                return;
            }
            if (record->hsms) {
                record->selected_generation_cache = record->hsms->selected_generation();
            }
            was_running = record->state == v1::SESSION_STATE_RUNNING;
            record->accepting_rpc_calls = false;
            if (was_running) {
                record->state = v1::SESSION_STATE_STOPPED;
                record->clear_last_error_locked();
            }
        }

        wait_for_no_active_calls(*record);
        stop_runtime(*record);

        fill_ok_status(response->mutable_status());
        *response->mutable_session() = record->snapshot();
    }

    void DeleteSession(::google::protobuf::RpcController *controller,
                       const v1::DeleteSessionRequest *request,
                       v1::DeleteSessionResponse *response,
                       ::google::protobuf::Closure *done) override {
        brpc::ClosureGuard done_guard(done);
        (void)controller;

        auto record = registry_.find(request->session_id());
        if (!record) {
            set_error(response,
                      std::make_error_code(std::errc::no_such_file_or_directory),
                      "session not found");
            return;
        }

        v1::SessionInfo deleted_info;
        {
            std::lock_guard invoke_lk(record->invoke_mu);
            {
                std::lock_guard state_lk(record->state_mu);
                record->deleted = true;
                record->accepting_rpc_calls = false;
                if (record->hsms) {
                    record->selected_generation_cache =
                        record->hsms->selected_generation();
                }
                record->state = v1::SESSION_STATE_STOPPED;
            }
            wait_for_no_active_calls(*record);
            stop_runtime(*record);
            deleted_info = record->snapshot();
        }
        registry_.erase(request->session_id());

        fill_ok_status(response->mutable_status());
        *response->mutable_session() = deleted_info;
    }

private:
    SessionRegistry &registry_;
    std::atomic<std::uint64_t> next_sequence_{0};
};

class MessagingServiceImpl final : public v1::MessagingService {
public:
    explicit MessagingServiceImpl(SessionRegistry &registry)
        : registry_(registry) {}

    void Send(::google::protobuf::RpcController *controller,
              const v1::SendRequest *request,
              v1::SendResponse *response,
              ::google::protobuf::Closure *done) override {
        brpc::ClosureGuard done_guard(done);
        (void)controller;

        auto record = registry_.find(request->session_id());
        if (!record) {
            set_error(response,
                      std::make_error_code(std::errc::no_such_file_or_directory),
                      "session not found");
            return;
        }
        if (!request->has_message()) {
            set_error(response,
                      std::make_error_code(std::errc::invalid_argument),
                      "message is required");
            return;
        }

        std::string detail;
        auto ec = validate_primary_message(request->message(), false, detail);
        if (ec) {
            set_error(response, ec, detail);
            return;
        }

        std::vector<Byte> body;
        ec = encode_message_body(request->message(), body);
        if (ec) {
            set_error(response, ec, "failed to encode request body");
            return;
        }

        secs::protocol::Session *protocol = nullptr;
        asio::io_context *io = nullptr;
        ec = begin_message_call(*record, protocol, io, detail);
        if (ec) {
            set_error(response, ec, detail);
            return;
        }
        ActiveRpcCallGuard call_guard(*record);
        std::unique_lock<std::mutex> transport_call_lk;
        if (requires_serial_message_calls(*record)) {
            transport_call_lk = std::unique_lock<std::mutex>(record->transport_call_mu);
        }

        std::pair<std::error_code, secs::protocol::DataMessage> send_result{};
        try {
            send_result = detail::run_on_executor<
                std::pair<std::error_code, secs::protocol::DataMessage>>(
                *io,
                [protocol,
                 stream = static_cast<std::uint8_t>(request->message().stream()),
                 function = static_cast<std::uint8_t>(request->message().function()),
                 body = std::move(body)]() mutable
                    -> asio::awaitable<std::pair<std::error_code,
                                                 secs::protocol::DataMessage>> {
                    co_return co_await protocol->async_send_primary(
                        stream,
                        function,
                        secs::core::bytes_view{body.data(), body.size()});
                });
        } catch (...) {
            send_result.first = std::make_error_code(std::errc::io_error);
        }

        if (send_result.first) {
            set_error(response, send_result.first);
            return;
        }

        fill_ok_status(response->mutable_status());
        (void)fill_envelope_from_data_message(send_result.second,
                                              response->mutable_accepted());
    }

    void Request(::google::protobuf::RpcController *controller,
                 const v1::RequestRequest *request,
                 v1::RequestResponse *response,
                 ::google::protobuf::Closure *done) override {
        brpc::ClosureGuard done_guard(done);
        (void)controller;

        auto record = registry_.find(request->session_id());
        if (!record) {
            set_error(response,
                      std::make_error_code(std::errc::no_such_file_or_directory),
                      "session not found");
            return;
        }
        if (!request->has_request()) {
            set_error(response,
                      std::make_error_code(std::errc::invalid_argument),
                      "request message is required");
            return;
        }

        std::string detail;
        auto ec = validate_primary_message(request->request(), true, detail);
        if (ec) {
            set_error(response, ec, detail);
            return;
        }

        std::vector<Byte> body;
        ec = encode_message_body(request->request(), body);
        if (ec) {
            set_error(response, ec, "failed to encode request body");
            return;
        }

        secs::protocol::Session *protocol = nullptr;
        asio::io_context *io = nullptr;
        ec = begin_message_call(*record, protocol, io, detail);
        if (ec) {
            set_error(response, ec, detail);
            return;
        }
        ActiveRpcCallGuard call_guard(*record);
        std::unique_lock<std::mutex> transport_call_lk;
        if (requires_serial_message_calls(*record)) {
            transport_call_lk = std::unique_lock<std::mutex>(record->transport_call_mu);
        }

        std::pair<std::error_code, secs::protocol::DataMessage> result{};
        try {
            result = detail::run_on_executor<
                std::pair<std::error_code, secs::protocol::DataMessage>>(
                *io,
                [protocol,
                 timeout = effective_request_timeout(*record, *request),
                 stream = static_cast<std::uint8_t>(request->request().stream()),
                 function = static_cast<std::uint8_t>(request->request().function()),
                 body = std::move(body)]() mutable
                    -> asio::awaitable<std::pair<std::error_code,
                                                 secs::protocol::DataMessage>> {
                    co_return co_await protocol->async_request(
                        stream,
                        function,
                        secs::core::bytes_view{body.data(), body.size()},
                        timeout);
                });
        } catch (...) {
            result.first = std::make_error_code(std::errc::io_error);
        }

        if (result.first) {
            set_error(response, result.first);
            return;
        }

        fill_ok_status(response->mutable_status());
        (void)fill_envelope_from_data_message(result.second, response->mutable_reply());
    }

private:
    SessionRegistry &registry_;
};

} // namespace

struct Server::Impl final {
    Impl()
        : session_service(registry),
          messaging_service(registry) {}

    brpc::Server server{};
    SessionRegistry registry{};
    LibraryServiceImpl library_service{};
    SessionServiceImpl session_service;
    MessagingServiceImpl messaging_service;
    bool services_registered{false};
    bool started_once{false};
    bool running{false};
    bool joined{false};
};

Server::Server() : impl_(std::make_unique<Impl>()) {}

Server::~Server() noexcept {
    stop();
    join();
}

std::error_code Server::start(const ServerOptions &options) {
    if (!impl_) {
        return std::make_error_code(std::errc::not_enough_memory);
    }
    if (impl_->running) {
        return std::make_error_code(std::errc::device_or_resource_busy);
    }
    if (impl_->started_once) {
        return std::make_error_code(std::errc::operation_not_supported);
    }

    if (!impl_->services_registered) {
        errno = 0;
        const int library_rc = impl_->server.AddService(
            &impl_->library_service, brpc::SERVER_DOESNT_OWN_SERVICE);
        if (library_rc != 0) {
            return make_brpc_error(library_rc);
        }

        errno = 0;
        const int session_rc = impl_->server.AddService(
            &impl_->session_service, brpc::SERVER_DOESNT_OWN_SERVICE);
        if (session_rc != 0) {
            return make_brpc_error(session_rc);
        }

        errno = 0;
        const int messaging_rc = impl_->server.AddService(
            &impl_->messaging_service, brpc::SERVER_DOESNT_OWN_SERVICE);
        if (messaging_rc != 0) {
            return make_brpc_error(messaging_rc);
        }

        impl_->services_registered = true;
    }

    brpc::ServerOptions brpc_options;
    brpc_options.idle_timeout_sec = options.idle_timeout_sec;
    brpc_options.num_threads = options.num_threads;
    brpc_options.internal_port = options.internal_port;
    brpc_options.has_builtin_services = options.enable_builtin_services;
    brpc_options.enabled_protocols =
        normalize_enabled_protocols(options.enabled_protocols);

    errno = 0;
    const int start_rc =
        impl_->server.Start(options.listen_address.c_str(), &brpc_options);
    if (start_rc != 0) {
        return make_brpc_error(start_rc);
    }

    impl_->started_once = true;
    impl_->running = true;
    impl_->joined = false;
    return {};
}

void Server::stop() noexcept {
    if (!impl_ || !impl_->running) {
        return;
    }
    impl_->server.Stop(0);
    impl_->running = false;
}

void Server::join() noexcept {
    if (!impl_ || !impl_->started_once || impl_->joined) {
        return;
    }
    impl_->server.Join();
    shutdown_registry(impl_->registry);
    impl_->joined = true;
}

bool Server::running() const noexcept { return impl_ && impl_->running; }

brpc::Server &Server::raw() noexcept { return impl_->server; }

const brpc::Server &Server::raw() const noexcept { return impl_->server; }

} // namespace secs::rpc
