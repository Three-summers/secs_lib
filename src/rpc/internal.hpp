#pragma once

#include "secs/core/common.hpp"
#include "secs/hsms/session.hpp"
#include "secs/protocol/session.hpp"
#include "secs/rpc/contracts.hpp"
#include "secs/secs1/link.hpp"
#include "secs/secs1/state_machine.hpp"

#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>

#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <vector>

namespace secs::rpc::detail {

struct SessionRecord final : std::enable_shared_from_this<SessionRecord> {
    SessionRecord(std::string session_id_value,
                  std::string session_name,
                  const v1::TransportConfig &transport_config,
                  const v1::SessionRuntimeConfig &runtime_config);

    std::string id;
    std::string name;
    v1::TransportConfig transport;
    v1::SessionRuntimeConfig runtime;

    std::mutex state_mu{};
    std::condition_variable rpc_calls_cv{};
    std::mutex invoke_mu{};
    std::mutex transport_call_mu{};
    v1::SessionState state{v1::SESSION_STATE_CREATED};
    bool deleted{false};
    bool has_last_error{false};
    bool accepting_rpc_calls{false};
    std::size_t active_rpc_calls{0};
    v1::RpcError last_error{};
    std::uint64_t selected_generation_cache{0};

    std::unique_ptr<asio::io_context> io{};
    std::optional<asio::executor_work_guard<asio::io_context::executor_type>>
        work_guard{};
    std::thread worker{};

    std::unique_ptr<secs::protocol::Session> protocol{};
    std::unique_ptr<secs::hsms::Session> hsms{};
    std::unique_ptr<asio::ip::tcp::acceptor> acceptor{};
    std::unique_ptr<secs::secs1::Link> secs1_link{};
    std::unique_ptr<secs::secs1::StateMachine> secs1_state_machine{};

    void clear_last_error_locked();
    void set_last_error_locked(const std::error_code &ec,
                               std::string_view override_message = {});
    v1::SessionInfo snapshot_locked();
    v1::SessionInfo snapshot();
};

class SessionRegistry final {
public:
    std::shared_ptr<SessionRecord>
    create(const v1::CreateSessionRequest &request,
           const std::string &session_id,
           const std::string &session_name);

    std::shared_ptr<SessionRecord> find(std::string_view session_id) const;
    std::vector<std::shared_ptr<SessionRecord>> list() const;
    bool erase(std::string_view session_id);

private:
    mutable std::mutex mu_{};
    std::unordered_map<std::string, std::shared_ptr<SessionRecord>> sessions_{};
};

std::error_code validate_transport_config(const v1::TransportConfig &transport,
                                          std::string &detail);
void clear_runtime_objects(SessionRecord &record);
void cleanup_finished_worker(SessionRecord &record);
std::error_code ensure_worker(SessionRecord &record);
void stop_runtime(SessionRecord &record);
void wait_for_no_active_calls(SessionRecord &record);
std::error_code begin_message_call(SessionRecord &record,
                                   secs::protocol::Session *&protocol_out,
                                   asio::io_context *&io_out,
                                   std::string &detail);
void end_message_call(SessionRecord &record) noexcept;
bool requires_serial_message_calls(const SessionRecord &record) noexcept;
void shutdown_record(SessionRecord &record);
void shutdown_registry(SessionRegistry &registry);

} // namespace secs::rpc::detail
