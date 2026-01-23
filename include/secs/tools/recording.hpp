#pragma once

#include "secs/core/common.hpp"
#include "secs/protocol/router.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace secs::tools {

/**
 * @brief 录制文件中的单条消息记录。
 *
 * 约定：
 * - timestamp_us：以 steady_clock 为基准的“相对时间”（微秒），保证单调。
 * - direction：相对本端的方向（TX=发送，RX=接收）。
 */
struct RecordedMessage final {
    std::uint64_t timestamp_us{0};
    bool is_tx{false};

    std::uint8_t stream{0};
    std::uint8_t function{0};
    bool w_bit{false};
    std::uint32_t system_bytes{0};
    std::vector<secs::core::byte> body{};

    // 可选：面向人类阅读的文本（例如 "S1F1 W."），不参与回放必需字段。
    std::optional<std::string> decoded_sml{};
};

/**
 * @brief 录制器：将 DataMessage 以 JSON Lines（每行一条）写入文件。
 *
 * 注意：
 * - 本类的 record_* 接口为 noexcept，适合直接挂到 protocol::SessionOptions::tap 回调；
 * - 录制 body 使用 hex 字符串（字段名 body_hex），便于直接与 utils::parse_hex 互通；
 * - 如果文件写入失败，可通过 last_error() 查看最近一次错误。
 */
class MessageRecorder final {
public:
    explicit MessageRecorder(const std::string &output_path);

    [[nodiscard]] bool is_open() const noexcept;
    [[nodiscard]] std::error_code last_error() const noexcept;

    // 设置过滤器：返回 true 表示允许录制该消息。
    void set_filter(
        std::function<bool(const secs::protocol::DataMessage &)> filter);
    void clear_filter() noexcept;

    // 设置是否写入 decoded_sml（仅写入头部 "SxFy[ W]."）。
    void set_include_sml_header(bool enable) noexcept;

    void record_tx(const secs::protocol::DataMessage &msg) noexcept;
    void record_rx(const secs::protocol::DataMessage &msg) noexcept;

    void flush() noexcept;

private:
    void record_(const secs::protocol::DataMessage &msg, bool is_tx) noexcept;

    mutable std::mutex mu_{};
    std::ofstream file_{};
    std::function<bool(const secs::protocol::DataMessage &)> filter_{};
    bool include_sml_header_{false};

    std::chrono::steady_clock::time_point start_{};
    std::error_code last_error_{};
};

enum class PlaybackMode : std::uint8_t {
    realtime = 0,
    fast = 1,
    step_by_step = 2,
};

/**
 * @brief 回放器：流式读取 JSONL 录制文件，按需提供下一条 RecordedMessage。
 *
 * 说明：
 * - 默认不一次性加载全部数据，适合大文件；
 * - realtime 模式会按录制时间间隔 sleep（受 speed 影响）；
 * - 解析失败时返回 nullopt，且 last_error() 为非零。
 */
class MessagePlayer final {
public:
    explicit MessagePlayer(const std::string &input_path);

    [[nodiscard]] bool is_open() const noexcept;
    [[nodiscard]] std::error_code last_error() const noexcept;

    void set_mode(PlaybackMode mode) noexcept;
    void set_speed(double multiplier) noexcept; // 1.0=原速，2.0=2倍速

    [[nodiscard]] PlaybackMode mode() const noexcept { return mode_; }
    [[nodiscard]] double speed() const noexcept { return speed_; }

    std::optional<RecordedMessage> next_message() noexcept;

    struct Stats final {
        std::size_t total_lines{0};
        std::size_t parsed_messages{0};
        std::size_t parse_errors{0};
    };
    [[nodiscard]] Stats get_stats() const noexcept { return stats_; }

    // 便捷：解析/序列化单行 JSONL（用于单测/工具复用）。
    static std::error_code parse_jsonl_line(std::string_view line,
                                           RecordedMessage &out) noexcept;
    static std::string to_jsonl_line(const RecordedMessage &msg);

private:
    static void apply_realtime_delay_(std::uint64_t last_ts_us,
                                      std::uint64_t cur_ts_us,
                                      double speed) noexcept;

    std::ifstream file_{};
    PlaybackMode mode_{PlaybackMode::fast};
    double speed_{1.0};

    bool has_last_ts_{false};
    std::uint64_t last_ts_us_{0};

    Stats stats_{};
    std::error_code last_error_{};
};

} // namespace secs::tools

