#include "secs/tools/recording.hpp"

#include "secs/core/error.hpp"
#include "secs/utils/hex.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <iomanip>
#include <limits>
#include <ostream>
#include <sstream>
#include <thread>

namespace secs::tools {
namespace {

[[nodiscard]] std::uint64_t now_rel_us_(
    std::chrono::steady_clock::time_point start) noexcept {
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - start)
                        .count();
    return (us < 0) ? 0u : static_cast<std::uint64_t>(us);
}

static void json_escape_to(std::ostream &os, std::string_view s) {
    for (const unsigned char c : s) {
        switch (c) {
        case '\\':
            os << "\\\\";
            break;
        case '"':
            os << "\\\"";
            break;
        case '\b':
            os << "\\b";
            break;
        case '\f':
            os << "\\f";
            break;
        case '\n':
            os << "\\n";
            break;
        case '\r':
            os << "\\r";
            break;
        case '\t':
            os << "\\t";
            break;
        default:
            if (c < 0x20) {
                os << "\\u00" << std::hex << std::setw(2) << std::setfill('0')
                   << static_cast<int>(c) << std::dec;
            } else {
                os << static_cast<char>(c);
            }
            break;
        }
    }
}

static void json_string(std::ostream &os, std::string_view s) {
    os << '"';
    json_escape_to(os, s);
    os << '"';
}

static void write_hex_to(std::ostream &os, secs::core::bytes_view bytes) {
    static constexpr char kHex[] = "0123456789abcdef";
    for (const auto b : bytes) {
        const auto v = static_cast<unsigned char>(b);
        os << kHex[(v >> 4) & 0x0F] << kHex[v & 0x0F];
    }
}

struct Cursor final {
    std::string_view s{};
    std::size_t i{0};
};

static void skip_ws_(Cursor &c) noexcept {
    while (c.i < c.s.size() &&
           std::isspace(static_cast<unsigned char>(c.s[c.i])) != 0) {
        ++c.i;
    }
}

[[nodiscard]] static bool consume_(Cursor &c, char ch) noexcept {
    skip_ws_(c);
    if (c.i >= c.s.size() || c.s[c.i] != ch) {
        return false;
    }
    ++c.i;
    return true;
}

[[nodiscard]] static std::error_code parse_string_(Cursor &c,
                                                   std::string &out) noexcept {
    skip_ws_(c);
    if (c.i >= c.s.size() || c.s[c.i] != '"') {
        return secs::core::make_error_code(secs::core::errc::invalid_argument);
    }
    ++c.i;
    out.clear();

    while (c.i < c.s.size()) {
        const auto ch = c.s[c.i++];
        if (ch == '"') {
            return {};
        }
        if (ch != '\\') {
            out.push_back(ch);
            continue;
        }
        if (c.i >= c.s.size()) {
            return secs::core::make_error_code(secs::core::errc::invalid_argument);
        }
        const auto esc = c.s[c.i++];
        switch (esc) {
        case '"':
        case '\\':
        case '/':
            out.push_back(esc);
            break;
        case 'b':
            out.push_back('\b');
            break;
        case 'f':
            out.push_back('\f');
            break;
        case 'n':
            out.push_back('\n');
            break;
        case 'r':
            out.push_back('\r');
            break;
        case 't':
            out.push_back('\t');
            break;
        case 'u': {
            // 最小实现：仅支持 \u00XX（ASCII），避免引入完整 UTF-16 处理。
            if (c.i + 4 > c.s.size()) {
                return secs::core::make_error_code(secs::core::errc::invalid_argument);
            }
            const auto h0 = c.s[c.i + 0];
            const auto h1 = c.s[c.i + 1];
            const auto h2 = c.s[c.i + 2];
            const auto h3 = c.s[c.i + 3];
            c.i += 4;
            if (h0 != '0' || h1 != '0') {
                return secs::core::make_error_code(secs::core::errc::invalid_argument);
            }
            const auto hex = [&](char x) -> int {
                if (x >= '0' && x <= '9')
                    return x - '0';
                if (x >= 'a' && x <= 'f')
                    return x - 'a' + 10;
                if (x >= 'A' && x <= 'F')
                    return x - 'A' + 10;
                return -1;
            };
            const int v2 = hex(h2);
            const int v3 = hex(h3);
            if (v2 < 0 || v3 < 0) {
                return secs::core::make_error_code(secs::core::errc::invalid_argument);
            }
            out.push_back(static_cast<char>(((v2 & 0x0F) << 4) | (v3 & 0x0F)));
            break;
        }
        default:
            return secs::core::make_error_code(secs::core::errc::invalid_argument);
        }
    }
    return secs::core::make_error_code(secs::core::errc::invalid_argument);
}

[[nodiscard]] static std::error_code parse_bool_(Cursor &c,
                                                 bool &out) noexcept {
    skip_ws_(c);
    const auto rest = c.s.substr(c.i);
    if (rest.rfind("true", 0) == 0) {
        c.i += 4;
        out = true;
        return {};
    }
    if (rest.rfind("false", 0) == 0) {
        c.i += 5;
        out = false;
        return {};
    }
    return secs::core::make_error_code(secs::core::errc::invalid_argument);
}

[[nodiscard]] static std::error_code parse_u64_(Cursor &c,
                                                std::uint64_t &out) noexcept {
    skip_ws_(c);
    if (c.i >= c.s.size()) {
        return secs::core::make_error_code(secs::core::errc::invalid_argument);
    }
    if (c.s[c.i] == '-') {
        return secs::core::make_error_code(secs::core::errc::invalid_argument);
    }

    const auto *begin = c.s.data() + c.i;
    const auto *end = c.s.data() + c.s.size();
    std::uint64_t v = 0;
    auto [ptr, ec] = std::from_chars(begin, end, v, 10);
    if (ec != std::errc{}) {
        return secs::core::make_error_code(secs::core::errc::invalid_argument);
    }
    if (ptr == begin) {
        return secs::core::make_error_code(secs::core::errc::invalid_argument);
    }
    c.i = static_cast<std::size_t>(ptr - c.s.data());
    out = v;
    return {};
}

[[nodiscard]] static std::error_code skip_value_(Cursor &c) noexcept {
    skip_ws_(c);
    if (c.i >= c.s.size()) {
        return secs::core::make_error_code(secs::core::errc::invalid_argument);
    }
    const char ch = c.s[c.i];
    if (ch == '"') {
        std::string tmp;
        return parse_string_(c, tmp);
    }
    if (ch == 't' || ch == 'f') {
        bool tmp = false;
        return parse_bool_(c, tmp);
    }
    if (ch == 'n') {
        const auto rest = c.s.substr(c.i);
        if (rest.rfind("null", 0) != 0) {
            return secs::core::make_error_code(secs::core::errc::invalid_argument);
        }
        c.i += 4;
        return {};
    }
    if (ch >= '0' && ch <= '9') {
        std::uint64_t tmp = 0;
        return parse_u64_(c, tmp);
    }
    return secs::core::make_error_code(secs::core::errc::invalid_argument);
}

} // namespace

MessageRecorder::MessageRecorder(const std::string &output_path) {
    start_ = std::chrono::steady_clock::now();
    file_.open(output_path, std::ios::out | std::ios::binary | std::ios::trunc);
    if (!file_) {
        last_error_ = std::make_error_code(std::errc::io_error);
    }
}

bool MessageRecorder::is_open() const noexcept {
    std::lock_guard lk(mu_);
    return file_.is_open();
}

std::error_code MessageRecorder::last_error() const noexcept {
    std::lock_guard lk(mu_);
    return last_error_;
}

void MessageRecorder::set_filter(
    std::function<bool(const secs::protocol::DataMessage &)> filter) {
    std::lock_guard lk(mu_);
    filter_ = std::move(filter);
}

void MessageRecorder::clear_filter() noexcept {
    std::lock_guard lk(mu_);
    filter_ = {};
}

void MessageRecorder::set_include_sml_header(bool enable) noexcept {
    std::lock_guard lk(mu_);
    include_sml_header_ = enable;
}

void MessageRecorder::record_tx(const secs::protocol::DataMessage &msg) noexcept {
    record_(msg, true);
}

void MessageRecorder::record_rx(const secs::protocol::DataMessage &msg) noexcept {
    record_(msg, false);
}

void MessageRecorder::flush() noexcept {
    std::lock_guard lk(mu_);
    if (!file_.is_open()) {
        return;
    }
    try {
        file_.flush();
    } catch (...) {
        last_error_ = std::make_error_code(std::errc::io_error);
        return;
    }
    if (!file_) {
        last_error_ = std::make_error_code(std::errc::io_error);
    }
}

void MessageRecorder::record_(const secs::protocol::DataMessage &msg,
                              bool is_tx) noexcept {
    std::lock_guard lk(mu_);
    if (!file_.is_open()) {
        return;
    }

    if (filter_) {
        bool keep = false;
        try {
            keep = filter_(msg);
        } catch (const std::bad_alloc &) {
            last_error_ = secs::core::make_error_code(secs::core::errc::out_of_memory);
            return;
        } catch (...) {
            last_error_ =
                secs::core::make_error_code(secs::core::errc::invalid_argument);
            return;
        }
        if (!keep) {
            return;
        }
    }

    try {
        const auto ts_us = now_rel_us_(start_);
        file_ << "{\"ts_us\":" << ts_us << ",\"dir\":\"" << (is_tx ? "TX" : "RX")
              << "\",\"s\":" << static_cast<int>(msg.stream)
              << ",\"f\":" << static_cast<int>(msg.function)
              << ",\"w\":" << (msg.w_bit ? "true" : "false")
              << ",\"sb\":" << msg.system_bytes << ",\"body_hex\":\"";
        write_hex_to(file_,
                     secs::core::bytes_view{msg.body.data(), msg.body.size()});
        file_ << '"';
        if (include_sml_header_) {
            file_ << ",\"sml\":\"S" << static_cast<int>(msg.stream) << 'F'
                  << static_cast<int>(msg.function);
            if (msg.w_bit) {
                file_ << " W";
            }
            file_ << ".\"";
        }
        file_ << "}\n";
        if (!file_) {
            last_error_ = std::make_error_code(std::errc::io_error);
        }
    } catch (const std::bad_alloc &) {
        last_error_ = secs::core::make_error_code(secs::core::errc::out_of_memory);
    } catch (...) {
        last_error_ = std::make_error_code(std::errc::io_error);
    }
}

MessagePlayer::MessagePlayer(const std::string &input_path) {
    file_.open(input_path, std::ios::in | std::ios::binary);
    if (!file_) {
        last_error_ = std::make_error_code(std::errc::no_such_file_or_directory);
    }
}

bool MessagePlayer::is_open() const noexcept { return file_.is_open(); }

std::error_code MessagePlayer::last_error() const noexcept { return last_error_; }

void MessagePlayer::set_mode(PlaybackMode mode) noexcept { mode_ = mode; }

void MessagePlayer::set_speed(double multiplier) noexcept {
    if (multiplier > 0.0) {
        speed_ = multiplier;
    } else {
        speed_ = 1.0;
    }
}

void MessagePlayer::apply_realtime_delay_(std::uint64_t last_ts_us,
                                          std::uint64_t cur_ts_us,
                                          double speed) noexcept {
    if (cur_ts_us <= last_ts_us) {
        return;
    }
    if (speed <= 0.0) {
        return;
    }
    const auto delta_us = cur_ts_us - last_ts_us;
    const auto scaled_us =
        static_cast<std::uint64_t>(static_cast<double>(delta_us) / speed);
    if (scaled_us == 0) {
        return;
    }
    std::this_thread::sleep_for(std::chrono::microseconds{scaled_us});
}

std::optional<RecordedMessage> MessagePlayer::next_message() noexcept {
    if (!file_.is_open()) {
        return std::nullopt;
    }

    std::string line;
    while (std::getline(file_, line)) {
        ++stats_.total_lines;
        // 允许空行/纯空白
        const auto non_ws =
            std::find_if_not(line.begin(), line.end(), [](unsigned char c) {
                return std::isspace(c) != 0;
            });
        if (non_ws == line.end()) {
            continue;
        }

        RecordedMessage msg{};
        const auto ec = parse_jsonl_line(line, msg);
        if (ec) {
            ++stats_.parse_errors;
            last_error_ = ec;
            return std::nullopt;
        }
        ++stats_.parsed_messages;

        if (mode_ == PlaybackMode::realtime && has_last_ts_) {
            apply_realtime_delay_(last_ts_us_, msg.timestamp_us, speed_);
        }
        has_last_ts_ = true;
        last_ts_us_ = msg.timestamp_us;
        last_error_.clear();
        return msg;
    }

    last_error_.clear();
    return std::nullopt;
}

std::error_code MessagePlayer::parse_jsonl_line(std::string_view line,
                                               RecordedMessage &out) noexcept {
    out = RecordedMessage{};

    Cursor c{line, 0};
    skip_ws_(c);
    if (!consume_(c, '{')) {
        return secs::core::make_error_code(secs::core::errc::invalid_argument);
    }

    bool have_ts = false;
    bool have_dir = false;
    bool have_s = false;
    bool have_f = false;
    bool have_w = false;
    bool have_sb = false;
    bool have_body = false;

    std::uint64_t ts_us = 0;
    bool is_tx = false;
    std::uint64_t s64 = 0;
    std::uint64_t f64 = 0;
    bool w = false;
    std::uint64_t sb64 = 0;
    std::string body_hex;
    std::string sml;

    for (;;) {
        skip_ws_(c);
        if (consume_(c, '}')) {
            break;
        }

        std::string key;
        if (auto ec = parse_string_(c, key)) {
            return ec;
        }
        if (!consume_(c, ':')) {
            return secs::core::make_error_code(secs::core::errc::invalid_argument);
        }

        if (key == "ts_us" || key == "ts") {
            if (auto ec = parse_u64_(c, ts_us)) {
                return ec;
            }
            have_ts = true;
        } else if (key == "dir" || key == "direction") {
            std::string dir;
            if (auto ec = parse_string_(c, dir)) {
                return ec;
            }
            if (dir == "TX" || dir == "tx") {
                is_tx = true;
            } else if (dir == "RX" || dir == "rx") {
                is_tx = false;
            } else {
                return secs::core::make_error_code(secs::core::errc::invalid_argument);
            }
            have_dir = true;
        } else if (key == "s" || key == "stream") {
            if (auto ec = parse_u64_(c, s64)) {
                return ec;
            }
            have_s = true;
        } else if (key == "f" || key == "function") {
            if (auto ec = parse_u64_(c, f64)) {
                return ec;
            }
            have_f = true;
        } else if (key == "w" || key == "w_bit") {
            if (auto ec = parse_bool_(c, w)) {
                return ec;
            }
            have_w = true;
        } else if (key == "sb" || key == "system_bytes") {
            if (auto ec = parse_u64_(c, sb64)) {
                return ec;
            }
            have_sb = true;
        } else if (key == "body_hex" || key == "body") {
            if (auto ec = parse_string_(c, body_hex)) {
                return ec;
            }
            have_body = true;
        } else if (key == "sml" || key == "decoded_sml") {
            if (auto ec = parse_string_(c, sml)) {
                return ec;
            }
            out.decoded_sml = sml;
        } else {
            if (auto ec = skip_value_(c)) {
                return ec;
            }
        }

        skip_ws_(c);
        if (consume_(c, ',')) {
            continue;
        }
        if (consume_(c, '}')) {
            break;
        }
        return secs::core::make_error_code(secs::core::errc::invalid_argument);
    }

    skip_ws_(c);
    if (c.i != c.s.size()) {
        return secs::core::make_error_code(secs::core::errc::invalid_argument);
    }

    if (!have_ts || !have_dir || !have_s || !have_f || !have_w || !have_sb ||
        !have_body) {
        return secs::core::make_error_code(secs::core::errc::invalid_argument);
    }
    if (s64 > std::numeric_limits<std::uint8_t>::max() ||
        f64 > std::numeric_limits<std::uint8_t>::max() ||
        sb64 > std::numeric_limits<std::uint32_t>::max()) {
        return secs::core::make_error_code(secs::core::errc::invalid_argument);
    }

    out.timestamp_us = ts_us;
    out.is_tx = is_tx;
    out.stream = static_cast<std::uint8_t>(s64);
    out.function = static_cast<std::uint8_t>(f64);
    out.w_bit = w;
    out.system_bytes = static_cast<std::uint32_t>(sb64);

    std::vector<secs::core::byte> bytes;
    const auto hex_ec = secs::utils::parse_hex(body_hex, bytes);
    if (hex_ec) {
        return hex_ec;
    }
    out.body = std::move(bytes);
    return {};
}

std::string MessagePlayer::to_jsonl_line(const RecordedMessage &msg) {
    std::ostringstream os;
    os << "{\"ts_us\":" << msg.timestamp_us << ",\"dir\":\""
       << (msg.is_tx ? "TX" : "RX") << "\",\"s\":" << static_cast<int>(msg.stream)
       << ",\"f\":" << static_cast<int>(msg.function) << ",\"w\":"
       << (msg.w_bit ? "true" : "false") << ",\"sb\":" << msg.system_bytes
       << ",\"body_hex\":\"";
    write_hex_to(os, secs::core::bytes_view{msg.body.data(), msg.body.size()});
    os << '"';
    if (msg.decoded_sml.has_value()) {
        os << ",\"sml\":";
        json_string(os, *msg.decoded_sml);
    }
    os << '}';
    return os.str();
}

} // namespace secs::tools
