#include "test_main.hpp"
#
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#
#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>
#
namespace {
#
using namespace std::chrono_literals;
#
struct UniqueFd final {
    int fd{-1};
    UniqueFd() = default;
    explicit UniqueFd(int v) : fd(v) {}
    UniqueFd(const UniqueFd &) = delete;
    UniqueFd &operator=(const UniqueFd &) = delete;
    UniqueFd(UniqueFd &&o) noexcept : fd(o.fd) { o.fd = -1; }
    UniqueFd &operator=(UniqueFd &&o) noexcept {
        if (this == &o) {
            return *this;
        }
        reset();
        fd = o.fd;
        o.fd = -1;
        return *this;
    }
    ~UniqueFd() { reset(); }
#
    void reset() noexcept {
        if (fd >= 0) {
            ::close(fd);
        }
        fd = -1;
    }
#
    [[nodiscard]] int release() noexcept {
        const int v = fd;
        fd = -1;
        return v;
    }
};
#
static void close_quietly(int fd) noexcept {
    if (fd >= 0) {
        (void)::close(fd);
    }
}
#
static std::string errno_message(const char *what) {
    std::string s(what);
    s += ": ";
    s += std::strerror(errno);
    return s;
}
#
static std::optional<int> wait_exit_code(pid_t pid,
                                         std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        int status = 0;
        const pid_t r = ::waitpid(pid, &status, WNOHANG);
        if (r == pid) {
            if (WIFEXITED(status)) {
                return WEXITSTATUS(status);
            }
            if (WIFSIGNALED(status)) {
                return 128 + WTERMSIG(status);
            }
            return -1;
        }
        if (r < 0) {
            return std::nullopt;
        }
#
        if (std::chrono::steady_clock::now() >= deadline) {
            return std::nullopt;
        }
        std::this_thread::sleep_for(10ms);
    }
}
#
static pid_t spawn_process(const char *path,
                           const char *arg0,
                           const char *arg1,
                           int stdin_fd,
                           int stdout_fd,
                           int close_fd1,
                           int close_fd2,
                           int close_fd3,
                           int close_fd4) {
    const pid_t pid = ::fork();
    if (pid != 0) {
        return pid;
    }
#
    // child: 绑定 stdin/stdout
    if (::dup2(stdin_fd, STDIN_FILENO) < 0) {
        std::fprintf(stderr,
                     "[hsms_pipe_examples] dup2(stdin) 失败: %s\n",
                     std::strerror(errno));
        std::_Exit(127);
    }
    if (::dup2(stdout_fd, STDOUT_FILENO) < 0) {
        std::fprintf(stderr,
                     "[hsms_pipe_examples] dup2(stdout) 失败: %s\n",
                     std::strerror(errno));
        std::_Exit(127);
    }
#
    // child: 关闭不再需要的 fd，避免引用计数导致对端无法感知关闭。
    close_quietly(stdin_fd);
    close_quietly(stdout_fd);
    close_quietly(close_fd1);
    close_quietly(close_fd2);
    close_quietly(close_fd3);
    close_quietly(close_fd4);
#
    // exec（stderr 继承父进程，用于输出日志，不影响 HSMS 二进制流）
    ::execl(path, arg0, arg1, static_cast<char *>(nullptr));
    std::fprintf(stderr,
                 "[hsms_pipe_examples] exec 失败: %s\n",
                 std::strerror(errno));
    std::_Exit(127);
}
#
} // namespace
#
int main(int argc, char **argv) {
    // 避免写入断管触发 SIGPIPE 终止进程（pipe 用例必需）。
    ::signal(SIGPIPE, SIG_IGN);
#
    int c2s[2]{-1, -1};
    int s2c[2]{-1, -1};
#
    if (::pipe(c2s) != 0) {
        TEST_FAIL(errno_message("pipe(c2s) 失败"));
        return ::secs::tests::run_and_report();
    }
    if (::pipe(s2c) != 0) {
        close_quietly(c2s[0]);
        close_quietly(c2s[1]);
        TEST_FAIL(errno_message("pipe(s2c) 失败"));
        return ::secs::tests::run_and_report();
    }
#
    UniqueFd c2s_r(c2s[0]);
    UniqueFd c2s_w(c2s[1]);
    UniqueFd s2c_r(s2c[0]);
    UniqueFd s2c_w(s2c[1]);
#
    // 根据 test 可执行文件所在目录推导 examples 输出目录：
    // - test 位于 build/tests/
    // - examples 位于 build/examples/
    const auto base =
        std::filesystem::path(argc > 0 ? argv[0] : "").parent_path();
    const auto server_path_fs =
        (base / "../examples/hsms_pipe_server").lexically_normal();
    const auto client_path_fs =
        (base / "../examples/hsms_pipe_client").lexically_normal();
    const std::string server_path = server_path_fs.string();
    const std::string client_path = client_path_fs.string();

    if (::access(server_path.c_str(), X_OK) != 0 ||
        ::access(client_path.c_str(), X_OK) != 0) {
        std::fprintf(stderr,
                     "[hsms_pipe_examples] SKIP: examples 未构建或不可执行\n");
        return 77;
    }
#
    // server: stdin <- c2s_r, stdout -> s2c_w
    const pid_t server_pid = spawn_process(server_path.c_str(),
                                           "hsms_pipe_server",
                                           "1",
                                           c2s_r.release(),
                                           s2c_w.release(),
                                           c2s_w.fd,
                                           s2c_r.fd,
                                           -1,
                                           -1);
    if (server_pid < 0) {
        TEST_FAIL(errno_message("fork(server) 失败"));
        return ::secs::tests::run_and_report();
    }
#
    // client: stdin <- s2c_r, stdout -> c2s_w
    const pid_t client_pid = spawn_process(client_path.c_str(),
                                           "hsms_pipe_client",
                                           "1",
                                           s2c_r.release(),
                                           c2s_w.release(),
                                           c2s_r.fd,
                                           s2c_w.fd,
                                           -1,
                                           -1);
    if (client_pid < 0) {
        TEST_FAIL(errno_message("fork(client) 失败"));
        (void)::kill(server_pid, SIGKILL);
        (void)wait_exit_code(server_pid, 1s);
        return ::secs::tests::run_and_report();
    }
#
    // parent: 关闭剩余 fd（child 已 dup2）
    c2s_r.reset();
    c2s_w.reset();
    s2c_r.reset();
    s2c_w.reset();
#
    const auto server_exit = wait_exit_code(server_pid, 3s);
    const auto client_exit = wait_exit_code(client_pid, 3s);
#
    if (!server_exit.has_value()) {
        TEST_FAIL("server 超时未退出（已 kill）");
        (void)::kill(server_pid, SIGKILL);
        (void)wait_exit_code(server_pid, 1s);
    }
    if (!client_exit.has_value()) {
        TEST_FAIL("client 超时未退出（已 kill）");
        (void)::kill(client_pid, SIGKILL);
        (void)wait_exit_code(client_pid, 1s);
    }
#
    if (server_exit.has_value()) {
        TEST_EXPECT_EQ(*server_exit, 0);
    }
    if (client_exit.has_value()) {
        TEST_EXPECT_EQ(*client_exit, 0);
    }
#
    return ::secs::tests::run_and_report();
}
