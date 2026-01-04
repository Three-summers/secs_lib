#pragma once

#include "secs/core/common.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <system_error>
#include <type_traits>
#include <vector>

namespace secs::secs1 {

inline constexpr secs::core::byte kEnq = 0x05;
inline constexpr secs::core::byte kEot = 0x04;
inline constexpr secs::core::byte kAck = 0x06;
inline constexpr secs::core::byte kNak = 0x15;

inline constexpr std::size_t kHeaderSize = 10;
inline constexpr std::size_t kMaxBlockDataSize = 244;
inline constexpr std::size_t kMaxBlockLength = kHeaderSize + kMaxBlockDataSize;
inline constexpr std::size_t kMaxBlockFrameSize = 1 + kMaxBlockLength + 2;

enum class errc : int {
    ok = 0,
    invalid_block = 1,
    checksum_mismatch = 2,
    device_id_mismatch = 3,
    protocol_error = 4,
    too_many_retries = 5,
    block_sequence_error = 6,
};

const std::error_category &error_category() noexcept;
std::error_code make_error_code(errc e) noexcept;

/**
 * @brief SECS-I Block Header（10B）。
 *
 * 编码布局（兼容 c_dump 实现）：
 * - Byte1: R(1b) + DeviceID[14:8](7b)
 * - Byte2: DeviceID[7:0]
 * - Byte3: W(1b) + Stream(7b)
 * - Byte4: Function(8b)
 * - Byte5: E(1b) + BlockNumber[14:8](7b)
 * - Byte6: BlockNumber[7:0](8b)
 * - Byte7..10: SystemBytes（big-endian）
 */
struct Header final {
    bool reverse_bit{false};
    std::uint16_t device_id{0}; // 15 位有效（高位与 reverse_bit 分离）

    bool wait_bit{false};
    std::uint8_t stream{0};
    std::uint8_t function{0};

    bool end_bit{false};
    std::uint16_t block_number{1}; // 15 位有效（1-32767）

    std::uint32_t system_bytes{0};
};

struct DecodedBlock final {
    Header header{};
    secs::core::bytes_view data{};
};

[[nodiscard]] std::uint16_t checksum(secs::core::bytes_view bytes) noexcept;

/**
 * @brief 编码一个完整 Block frame。
 *
 * frame 格式：
 * - Length(1B)：后续 Header(10B)+Data(N) 的总长度，要求 10<=Length<=254
 * - Header(10B)
 * - Data(N)（0<=N<=244）
 * - Checksum(2B, big-endian)：对 Length 之后的 (Length) 个字节求和（mod 65536）
 */
std::error_code encode_block(const Header &header,
                             secs::core::bytes_view data,
                             std::vector<secs::core::byte> &out);

/**
 * @brief 解码并校验一个完整 Block frame。
 *
 * 入参必须是完整 frame（含 Length + payload + checksum）。
 */
std::error_code decode_block(secs::core::bytes_view frame, DecodedBlock &out);

/**
 * @brief 将 payload 按 244B/block 切分并编码为多个 frame。
 *
 * base_header 中的 end_bit/block_number 会被覆盖；其余字段会继承。
 */
std::vector<std::vector<secs::core::byte>>
fragment_message(Header base_header, secs::core::bytes_view payload);

/**
 * @brief 多 Block 消息重组器（带 DeviceID/Block 序列校验）。
 *
 * 注意：
 * - 默认要求 BlockNumber 从 1 递增；可用于单元测试与链路层基本校验。
 * - 支持 data 为空的消息。
 */
class Reassembler final {
public:
    explicit Reassembler(
        std::optional<std::uint16_t> expected_device_id = std::nullopt);

    void reset() noexcept;

    [[nodiscard]] bool has_message() const noexcept;
    [[nodiscard]] const Header &message_header() const noexcept;
    [[nodiscard]] secs::core::bytes_view message_body() const noexcept;

    std::error_code accept(const DecodedBlock &block);

private:
    std::optional<std::uint16_t> expected_device_id_;
    bool has_header_{false};
    Header header_{};
    std::uint16_t next_block_{1};
    std::vector<secs::core::byte> body_{};
};

} // namespace secs::secs1

namespace std {
template <>
struct is_error_code_enum<secs::secs1::errc> : true_type {};
} // namespace std
