#pragma once

#include <memory>
#include <string>
#include <system_error>

namespace brpc {
class Server;
}

namespace secs::rpc {

struct ServerOptions final {
    // 监听地址，使用 brpc 支持的 ip:port 形式。
    std::string listen_address{"0.0.0.0:50051"};

    // 连接空闲超时，单位秒；-1 表示关闭。
    int idle_timeout_sec{-1};

    // brpc 工作线程数；0 表示使用默认值。
    int num_threads{0};

    // 内建服务端口；-1 表示不单独开放。
    int internal_port{-1};

    // 是否保留 brpc 内建服务。
    bool enable_builtin_services{true};

    // 显式限制可接受协议；空字符串表示沿用 brpc 默认行为（接受全部协议）。
    std::string enabled_protocols{};
};

class Server final {
public:
    Server();
    ~Server() noexcept;

    Server(const Server &) = delete;
    Server &operator=(const Server &) = delete;
    Server(Server &&) = delete;
    Server &operator=(Server &&) = delete;

    [[nodiscard]] std::error_code
    start(const ServerOptions &options = {});

    void stop() noexcept;
    void join() noexcept;

    [[nodiscard]] bool running() const noexcept;

    [[nodiscard]] brpc::Server &raw() noexcept;
    [[nodiscard]] const brpc::Server &raw() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_{};
};

} // namespace secs::rpc
