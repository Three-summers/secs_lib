#include "rpc/internal.hpp"

#include "test_main.hpp"

#include <atomic>
#include <chrono>
#include <thread>

namespace {

secs::rpc::v1::TransportConfig make_hsms_transport(std::uint32_t session_id) {
    secs::rpc::v1::TransportConfig transport;
    transport.set_kind(secs::rpc::v1::TRANSPORT_KIND_HSMS);
    auto *hsms = transport.mutable_hsms();
    hsms->set_ip("127.0.0.1");
    hsms->set_port(5000);
    hsms->set_session_id(session_id);
    hsms->set_auto_reconnect(false);
    return transport;
}

secs::rpc::v1::CreateSessionRequest make_hsms_request(std::uint32_t session_id) {
    secs::rpc::v1::CreateSessionRequest request;
    *request.mutable_transport() = make_hsms_transport(session_id);
    request.mutable_runtime()->set_request_timeout_ms(200);
    return request;
}

std::shared_ptr<secs::rpc::detail::SessionRecord> make_running_hsms_record() {
    auto record = std::make_shared<secs::rpc::detail::SessionRecord>(
        "rpc-session-test",
        "rpc-session-test",
        make_hsms_transport(1),
        secs::rpc::v1::SessionRuntimeConfig{});

    record->io = std::make_unique<asio::io_context>();
    record->hsms = std::make_unique<secs::hsms::Session>(
        record->io->get_executor(),
        secs::hsms::SessionOptions{.session_id = 1, .auto_reconnect = false});
    record->protocol = std::make_unique<secs::protocol::Session>(
        *record->hsms,
        1,
        secs::protocol::SessionOptions{});

    {
        std::lock_guard lk(record->state_mu);
        record->state = secs::rpc::v1::SESSION_STATE_RUNNING;
        record->accepting_rpc_calls = true;
    }
    return record;
}

std::size_t active_call_count(
    const std::shared_ptr<secs::rpc::detail::SessionRecord> &record) {
    std::lock_guard lk(record->state_mu);
    return record->active_rpc_calls;
}

void test_validate_hsms_session_id_range() {
    std::string detail;
    auto transport = make_hsms_transport(0x8000U);
    const auto ec = secs::rpc::detail::validate_transport_config(transport, detail);
    TEST_EXPECT(ec == std::make_error_code(std::errc::invalid_argument));
    TEST_EXPECT_EQ(detail, std::string{"hsms.session_id must be in range [0, 32767]"});

    detail.clear();
    transport = make_hsms_transport(0x7FFFU);
    TEST_EXPECT_OK(secs::rpc::detail::validate_transport_config(transport, detail));
    TEST_EXPECT(detail.empty());
}

void test_shutdown_registry_stops_worker_threads() {
    secs::rpc::detail::SessionRegistry registry;
    auto record = registry.create(make_hsms_request(1), "rpc-session-1", "session");
    {
        std::lock_guard lk(record->state_mu);
        record->state = secs::rpc::v1::SESSION_STATE_RUNNING;
        record->accepting_rpc_calls = true;
    }

    TEST_EXPECT_OK(secs::rpc::detail::ensure_worker(*record));
    TEST_EXPECT(record->worker.joinable());
    TEST_EXPECT(record->io != nullptr);

    secs::rpc::detail::shutdown_registry(registry);

    TEST_EXPECT(!record->worker.joinable());
    TEST_EXPECT(record->io == nullptr);
    TEST_EXPECT(record->protocol == nullptr);
    TEST_EXPECT(record->hsms == nullptr);
    TEST_EXPECT_EQ(record->state, secs::rpc::v1::SESSION_STATE_STOPPED);
    TEST_EXPECT(!record->accepting_rpc_calls);
}

void test_message_call_gate_allows_hsms_overlap_and_waits_on_shutdown() {
    auto record = make_running_hsms_record();

    secs::protocol::Session *protocol1 = nullptr;
    asio::io_context *io1 = nullptr;
    std::string detail1;
    TEST_EXPECT_OK(
        secs::rpc::detail::begin_message_call(*record, protocol1, io1, detail1));
    TEST_EXPECT(protocol1 != nullptr);
    TEST_EXPECT(io1 == record->io.get());

    secs::protocol::Session *protocol2 = nullptr;
    asio::io_context *io2 = nullptr;
    std::string detail2;
    TEST_EXPECT_OK(
        secs::rpc::detail::begin_message_call(*record, protocol2, io2, detail2));
    TEST_EXPECT(protocol2 != nullptr);
    TEST_EXPECT(io2 == record->io.get());
    TEST_EXPECT_EQ(active_call_count(record), std::size_t{2});

    {
        std::lock_guard lk(record->state_mu);
        record->accepting_rpc_calls = false;
    }

    secs::protocol::Session *blocked_protocol = nullptr;
    asio::io_context *blocked_io = nullptr;
    std::string blocked_detail;
    const auto blocked_ec = secs::rpc::detail::begin_message_call(
        *record, blocked_protocol, blocked_io, blocked_detail);
    TEST_EXPECT(blocked_ec == std::make_error_code(std::errc::operation_canceled));
    TEST_EXPECT_EQ(blocked_detail, std::string{"session is stopping"});
    TEST_EXPECT_EQ(active_call_count(record), std::size_t{2});

    {
        std::lock_guard lk(record->state_mu);
        record->accepting_rpc_calls = true;
    }

    std::atomic_bool shutdown_finished{false};
    std::thread shutdown_thread([&] {
        secs::rpc::detail::shutdown_record(*record);
        shutdown_finished.store(true, std::memory_order_relaxed);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    TEST_EXPECT(!shutdown_finished.load(std::memory_order_relaxed));

    secs::rpc::detail::end_message_call(*record);
    TEST_EXPECT_EQ(active_call_count(record), std::size_t{1});
    TEST_EXPECT(!shutdown_finished.load(std::memory_order_relaxed));

    secs::rpc::detail::end_message_call(*record);
    shutdown_thread.join();

    TEST_EXPECT(shutdown_finished.load(std::memory_order_relaxed));
    TEST_EXPECT_EQ(active_call_count(record), std::size_t{0});
    TEST_EXPECT_EQ(record->state, secs::rpc::v1::SESSION_STATE_STOPPED);
    TEST_EXPECT(!record->accepting_rpc_calls);
    TEST_EXPECT(record->io == nullptr);
}

} // namespace

int main() {
    test_validate_hsms_session_id_range();
    test_shutdown_registry_stops_worker_threads();
    test_message_call_gate_allows_hsms_overlap_and_waits_on_shutdown();
    return secs::tests::run_and_report();
}
