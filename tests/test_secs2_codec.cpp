#include "secs/ii/codec.hpp"

#include "test_main.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace {

using secs::ii::byte;
using secs::ii::bytes_view;
using secs::ii::decode_one;
using secs::ii::encode;
using secs::ii::encode_to;
using secs::ii::encoded_size;
using secs::ii::errc;
using secs::ii::Item;
using secs::ii::make_error_code;

Item placeholder_item() { return Item::binary(std::vector<byte>{}); }

std::vector<byte> encode_ok(const Item &item) {
    std::vector<byte> out;
    TEST_EXPECT_OK(encode(item, out));
    return out;
}

Item decode_ok(const std::vector<byte> &in, std::size_t &consumed) {
    Item out = placeholder_item();
    consumed = 0;
    TEST_EXPECT_OK(decode_one(bytes_view{in.data(), in.size()}, out, consumed));
    TEST_EXPECT_EQ(consumed, in.size());
    return out;
}

std::uint8_t header_length_bytes(const std::vector<byte> &encoded) {
    TEST_EXPECT(!encoded.empty());
    const auto length_bytes = static_cast<std::uint8_t>(encoded[0] & 0x03u);
    TEST_EXPECT(length_bytes >= 1u);
    TEST_EXPECT(length_bytes <= 3u);
    return length_bytes;
}

std::uint32_t header_length_value(const std::vector<byte> &encoded) {
    const auto length_bytes = header_length_bytes(encoded);
    TEST_EXPECT(encoded.size() >= 1u + length_bytes);
    std::uint32_t v = 0;
    for (std::uint8_t i = 0; i < length_bytes; ++i) {
        v = (v << 8) | static_cast<std::uint32_t>(encoded[1u + i]);
    }
    return v;
}

void expect_roundtrip(const Item &item) {
    std::size_t expected_size = 0;
    TEST_EXPECT_OK(encoded_size(item, expected_size));

    std::vector<byte> out;
    TEST_EXPECT_OK(encode(item, out));
    TEST_EXPECT_EQ(out.size(), expected_size);

    std::size_t consumed = 0;
    const auto decoded = decode_ok(out, consumed);
    TEST_EXPECT(decoded == item);
}

void test_roundtrip_all_types() {
    expect_roundtrip(Item::list({}));

    expect_roundtrip(Item::ascii(""));
    expect_roundtrip(Item::ascii("HELLO"));
    expect_roundtrip(
        Item::binary(std::vector<byte>{byte{0x00}, byte{0xFF}, byte{0x7E}}));
    expect_roundtrip(Item::boolean(std::vector<bool>{true, false, true}));

    expect_roundtrip(Item::i1(std::vector<std::int8_t>{
        std::int8_t{0},
        std::int8_t{1},
        std::int8_t{-1},
        std::int8_t{127},
        std::int8_t{-128},
    }));
    expect_roundtrip(
        Item::i2(std::vector<std::int16_t>{0, 1, -1, 0x1234, -0x1234}));
    expect_roundtrip(
        Item::i4(std::vector<std::int32_t>{0, 1, -1, 0x1234567, -0x1234567}));
    expect_roundtrip(Item::i8(
        std::vector<std::int64_t>{0, 1, -1, 0x123456789LL, -0x123456789LL}));

    expect_roundtrip(Item::u1(std::vector<std::uint8_t>{0u, 1u, 255u}));
    expect_roundtrip(
        Item::u2(std::vector<std::uint16_t>{0u, 1u, 0x1234u, 0xFFFFu}));
    expect_roundtrip(
        Item::u4(std::vector<std::uint32_t>{0u, 1u, 0x12345678u, 0xFFFFFFFFu}));
    expect_roundtrip(Item::u8(std::vector<std::uint64_t>{
        0u, 1u, 0x123456789ABCDEF0ull, 0xFFFFFFFFFFFFFFFFull}));

    const float f_nan = std::bit_cast<float>(std::uint32_t{0x7FC00001u});
    const double d_nan =
        std::bit_cast<double>(std::uint64_t{0x7FF8000000000001ull});
    expect_roundtrip(Item::f4(std::vector<float>{0.0f, -0.0f, 1.25f, f_nan}));
    expect_roundtrip(Item::f8(std::vector<double>{0.0, -0.0, 1.25, d_nan}));

    // 空数组：覆盖 length=0 的编码/解码路径
    expect_roundtrip(Item::i2(std::vector<std::int16_t>{}));
    expect_roundtrip(Item::u4(std::vector<std::uint32_t>{}));

    const auto nested = Item::list({
        Item::ascii("ABC"),
        Item::list({
            Item::binary(std::vector<byte>{byte{0x10}, byte{0x20}}),
            Item::u2(std::vector<std::uint16_t>{0x1234u, 0xABCDu}),
            Item::list({
                Item::boolean(std::vector<bool>{}),
                Item::i4(std::vector<std::int32_t>{-1, 0, 1}),
            }),
        }),
    });
    expect_roundtrip(nested);
}

void test_error_category_and_messages() {
    using std::string_view;

    const auto &category = secs::ii::error_category();
    TEST_EXPECT_EQ(string_view(category.name()), "secs.ii");

    TEST_EXPECT_EQ(make_error_code(errc::ok).message(), "ok");
    TEST_EXPECT_EQ(make_error_code(errc::truncated).message(),
                   "truncated input");
    TEST_EXPECT_EQ(make_error_code(errc::invalid_header).message(),
                   "invalid secs-ii header");
    TEST_EXPECT_EQ(make_error_code(errc::invalid_format).message(),
                   "invalid secs-ii format code");
    TEST_EXPECT_EQ(make_error_code(errc::length_overflow).message(),
                   "secs-ii length overflow");
    TEST_EXPECT_EQ(make_error_code(errc::length_mismatch).message(),
                   "secs-ii length mismatch");
    TEST_EXPECT_EQ(make_error_code(errc::buffer_overflow).message(),
                   "output buffer overflow");

    std::error_code unknown(9999, category);
    TEST_EXPECT_EQ(unknown.message(), "unknown secs.ii error");
}

void test_item_comparisons() {
    TEST_EXPECT(!(Item::ascii("A") == Item::binary(std::vector<byte>{})));

    const auto f4a = Item::f4(std::vector<float>{0.0f});
    const auto f4b = Item::f4(std::vector<float>{0.0f, 0.0f});
    TEST_EXPECT(!(f4a == f4b)); // 元素数量不匹配

    const auto f4c = Item::f4(std::vector<float>{0.0f, 1.0f});
    const auto f4d = Item::f4(std::vector<float>{0.0f, 2.0f});
    TEST_EXPECT(!(f4c == f4d)); // 元素内容不匹配

    const auto f8c = Item::f8(std::vector<double>{0.0, 1.0});
    const auto f8d = Item::f8(std::vector<double>{0.0, 2.0});
    TEST_EXPECT(!(f8c == f8d));

    const auto f8a = Item::f8(std::vector<double>{0.0});
    const auto f8b = Item::f8(std::vector<double>{0.0, 0.0});
    TEST_EXPECT(!(f8a == f8b)); // 元素数量不匹配
}

void test_length_field_boundaries_ascii() {
    const struct {
        std::size_t size;
        std::uint8_t expected_length_bytes;
    } cases[] = {
        {0u, 1u},
        {127u, 1u},
        {255u, 1u},
        {256u, 2u},
        {65535u, 2u},
        {65536u, 3u},
    };

    for (const auto &c : cases) {
        const auto item = Item::ascii(std::string(c.size, 'A'));
        const auto encoded = encode_ok(item);
        TEST_EXPECT_EQ(header_length_bytes(encoded), c.expected_length_bytes);
        TEST_EXPECT_EQ(static_cast<std::size_t>(header_length_value(encoded)),
                       c.size);
        TEST_EXPECT_EQ(encoded.size(), 1u + c.expected_length_bytes + c.size);
    }
}

void test_length_field_boundaries_list() {
    {
        const auto empty = encode_ok(Item::list({}));
        TEST_EXPECT_EQ(header_length_bytes(empty), 1u);
        TEST_EXPECT_EQ(header_length_value(empty), 0u);
    }

    // 256 个空 Binary 子元素 -> List 的 length 需要 2 字节
    std::vector<Item> children;
    children.reserve(256u);
    for (std::size_t i = 0; i < 256u; ++i) {
        children.push_back(Item::binary(std::vector<byte>{}));
    }
    const auto list256 = encode_ok(Item::list(std::move(children)));
    TEST_EXPECT_EQ(header_length_bytes(list256), 2u);
    TEST_EXPECT_EQ(header_length_value(list256), 256u);
}

void test_stream_decode_consumed() {
    const auto a = Item::ascii("A");
    const auto b = Item::u4(std::vector<std::uint32_t>{0x12345678u});

    std::vector<byte> stream;
    TEST_EXPECT_OK(encode(a, stream));
    const auto a_size = stream.size();
    TEST_EXPECT_OK(encode(b, stream));

    Item out_a = placeholder_item();
    std::size_t consumed_a = 0;
    TEST_EXPECT_OK(decode_one(
        bytes_view{stream.data(), stream.size()}, out_a, consumed_a));
    TEST_EXPECT_EQ(consumed_a, a_size);
    TEST_EXPECT(out_a == a);

    Item out_b = placeholder_item();
    std::size_t consumed_b = 0;
    const auto tail =
        bytes_view{stream.data(), stream.size()}.subspan(consumed_a);
    TEST_EXPECT_OK(decode_one(tail, out_b, consumed_b));
    TEST_EXPECT_EQ(consumed_b, stream.size() - a_size);
    TEST_EXPECT(out_b == b);
}

void test_decode_errors() {
    {
        Item out = placeholder_item();
        std::size_t consumed = 123;
        const auto ec = decode_one(bytes_view{}, out, consumed);
        TEST_EXPECT_EQ(ec, make_error_code(errc::truncated));
        TEST_EXPECT_EQ(consumed, 0u);
    }

    // length_bytes == 0（低 2 位为 00）视为非法头部
    {
        const std::vector<byte> in{byte{0x40}};
        Item out = placeholder_item();
        std::size_t consumed = 0;
        const auto ec =
            decode_one(bytes_view{in.data(), in.size()}, out, consumed);
        TEST_EXPECT_EQ(ec, make_error_code(errc::invalid_header));
        TEST_EXPECT_EQ(consumed, 0u);
    }

    // 非法格式码
    {
        const std::vector<byte> in{
            byte{0x1D},
            byte{0x00}}; // format_bits=0x07（非法），lenBytes=1，length=0
        Item out = placeholder_item();
        std::size_t consumed = 0;
        const auto ec =
            decode_one(bytes_view{in.data(), in.size()}, out, consumed);
        TEST_EXPECT_EQ(ec, make_error_code(errc::invalid_format));
        TEST_EXPECT_EQ(consumed, 0u);
    }

    // 负载截断
    {
        const std::vector<byte> in{
            byte{0x41},
            byte{0x05},
            byte{'a'},
            byte{'b'}}; // ASCII，length=5，但实际只有 2 字节
        Item out = placeholder_item();
        std::size_t consumed = 0;
        const auto ec =
            decode_one(bytes_view{in.data(), in.size()}, out, consumed);
        TEST_EXPECT_EQ(ec, make_error_code(errc::truncated));
        TEST_EXPECT_EQ(consumed, 0u);
    }

    // 数值类型长度不匹配（I2 长度=1）
    {
        const std::vector<byte> in{
            byte{0x69},
            byte{0x01},
            byte{0x00}}; // I2，lenBytes=1，length=1（不满足 2 字节对齐）
        Item out = placeholder_item();
        std::size_t consumed = 0;
        const auto ec =
            decode_one(bytes_view{in.data(), in.size()}, out, consumed);
        TEST_EXPECT_EQ(ec, make_error_code(errc::length_mismatch));
        TEST_EXPECT_EQ(consumed, 0u);
    }

    // 长度字段截断（lenBytes=3，但只给了 2 字节 length）
    {
        const std::vector<byte> in{
            byte{0x43},
            byte{0x00},
            byte{0x01}}; // ASCII，lenBytes=3（长度字段被截断）
        Item out = placeholder_item();
        std::size_t consumed = 0;
        const auto ec =
            decode_one(bytes_view{in.data(), in.size()}, out, consumed);
        TEST_EXPECT_EQ(ec, make_error_code(errc::truncated));
        TEST_EXPECT_EQ(consumed, 0u);
    }

    // 浮点长度不匹配（F4 长度=3）
    {
        const std::vector<byte> in{
            byte{0x91}, byte{0x03}, byte{0x00}, byte{0x00}, byte{0x00}};
        Item out = placeholder_item();
        std::size_t consumed = 0;
        const auto ec =
            decode_one(bytes_view{in.data(), in.size()}, out, consumed);
        TEST_EXPECT_EQ(ec, make_error_code(errc::length_mismatch));
        TEST_EXPECT_EQ(consumed, 0u);
    }
}

void test_decode_deep_list_nesting_rejected() {
    // 恶意输入：构造极深嵌套 List，验证解码有深度上限且不会栈溢出。
    constexpr std::size_t depth = 256;
    std::vector<byte> in;
    in.reserve(depth * 2 + 2);
    for (std::size_t i = 0; i < depth; ++i) {
        in.push_back(byte{0x01}); // List（列表），lenBytes=1
        in.push_back(byte{0x01}); // length=1（单子元素）
    }
    in.push_back(byte{0x21}); // Binary（二进制），lenBytes=1
    in.push_back(byte{0x00}); // length=0（空 Binary）

    Item out = placeholder_item();
    std::size_t consumed = 123;
    const auto ec = decode_one(bytes_view{in.data(), in.size()}, out, consumed);
    TEST_EXPECT_EQ(ec, make_error_code(errc::invalid_header));
    TEST_EXPECT_EQ(consumed, 0u);
}

void test_decode_depth_limit_boundary() {
    // 边界值：嵌套深度=64 应该可正常解码（深度>64 才触发拒绝）。
    constexpr std::size_t depth = 64;
    std::vector<byte> in;
    in.reserve(depth * 2 + 2);
    for (std::size_t i = 0; i < depth; ++i) {
        in.push_back(byte{0x01}); // List（列表），lenBytes=1
        in.push_back(byte{0x01}); // length=1（单子元素）
    }
    in.push_back(byte{0x21}); // Binary（二进制），lenBytes=1
    in.push_back(byte{0x00}); // length=0（空 Binary）

    Item out = placeholder_item();
    std::size_t consumed = 0;
    TEST_EXPECT_OK(decode_one(bytes_view{in.data(), in.size()}, out, consumed));
    TEST_EXPECT_EQ(consumed, in.size());

    // 校验结构：每层都是 length=1 的 List，最终落到空 Binary。
    const Item *current = &out;
    for (std::size_t i = 0; i < depth; ++i) {
        const auto *list = current->get_if<secs::ii::List>();
        TEST_EXPECT(list != nullptr);
        TEST_EXPECT_EQ(list->size(), 1u);
        current = &(*list)[0];
    }
    const auto *bin = current->get_if<secs::ii::Binary>();
    TEST_EXPECT(bin != nullptr);
    TEST_EXPECT_EQ(bin->value.size(), 0u);
}

void test_decode_depth_limit_plus_one_rejected() {
    // 边界值+1：嵌套深度=65 应该被拒绝（防 off-by-one 回归）。
    constexpr std::size_t depth = 65;
    std::vector<byte> in;
    in.reserve(depth * 2 + 2);
    for (std::size_t i = 0; i < depth; ++i) {
        in.push_back(byte{0x01}); // List（列表），lenBytes=1
        in.push_back(byte{0x01}); // length=1（单子元素）
    }
    in.push_back(byte{0x21}); // Binary（二进制），lenBytes=1
    in.push_back(byte{0x00}); // length=0

    Item out = placeholder_item();
    std::size_t consumed = 123;
    const auto ec = decode_one(bytes_view{in.data(), in.size()}, out, consumed);
    TEST_EXPECT_EQ(ec, make_error_code(errc::invalid_header));
    TEST_EXPECT_EQ(consumed, 0u);
}

void test_decode_boolean_non_zero_normalized_to_true() {
    // Boolean 的元素按字节编码；当前实现将“非 0”视为 true（兼容性更好）。
    const std::vector<byte> in{
        byte{0x25}, // Boolean（format_code=0x09），lenBytes=1
        byte{0x03}, // length=3
        byte{0x00},
        byte{0x02},
        byte{0xFF},
    };

    Item out = placeholder_item();
    std::size_t consumed = 0;
    TEST_EXPECT_OK(decode_one(bytes_view{in.data(), in.size()}, out, consumed));
    TEST_EXPECT_EQ(consumed, in.size());

    const auto *v = out.get_if<secs::ii::Boolean>();
    TEST_EXPECT(v != nullptr);
    TEST_EXPECT_EQ(v->values.size(), 3u);
    TEST_EXPECT(v->values[0] == false);
    TEST_EXPECT(v->values[1] == true);
    TEST_EXPECT(v->values[2] == true);
}

void test_decode_list_too_large_rejected() {
    // List 的 length 是“子元素个数”，这里构造 length=65536（超出默认上限 65535）。
    const std::vector<byte> in{
        byte{0x03}, // List，lenBytes=3
        byte{0x01},
        byte{0x00},
        byte{0x00}, // length=0x010000
    };

    Item out = placeholder_item();
    std::size_t consumed = 0;
    const auto ec = decode_one(bytes_view{in.data(), in.size()}, out, consumed);
    TEST_EXPECT_EQ(ec, make_error_code(errc::list_too_large));
    TEST_EXPECT_EQ(consumed, 0u);
}

void test_decode_total_item_budget_exceeded_rejected() {
    // 使用自定义 DecodeLimits，把 total_items 上限压到很小，以便快速覆盖分支。
    secs::ii::DecodeLimits limits{};
    limits.max_total_items = 5; // root(1)+children(>4) 会触发 total_budget_exceeded

    const std::vector<byte> in{
        byte{0x01}, // List，lenBytes=1
        byte{0x06}, // length=6（子元素个数）
    };

    Item out = placeholder_item();
    std::size_t consumed = 0;
    const auto ec =
        decode_one(bytes_view{in.data(), in.size()}, out, consumed, limits);
    TEST_EXPECT_EQ(ec, make_error_code(errc::total_budget_exceeded));
    TEST_EXPECT_EQ(consumed, 0u);
}

void test_decode_total_byte_budget_exceeded_before_payload_read() {
    // 使用自定义 DecodeLimits，把 total_bytes 上限压到 3，构造 ASCII length=4
    // 的头部，验证在读取 payload 前即可拒绝（避免不必要的读取/分配）。
    secs::ii::DecodeLimits limits{};
    limits.max_total_bytes = 3;

    const std::vector<byte> in{
        byte{0x41}, // ASCII，lenBytes=1
        byte{0x04}, // length=4
    };

    Item out = placeholder_item();
    std::size_t consumed = 0;
    const auto ec =
        decode_one(bytes_view{in.data(), in.size()}, out, consumed, limits);
    TEST_EXPECT_EQ(ec, make_error_code(errc::total_budget_exceeded));
    TEST_EXPECT_EQ(consumed, 0u);
}

void test_decode_large_ascii_length_truncated() {
    // 恶意输入：ASCII 声明 length=1MB，但负载只有 100 字节，必须返回
    // truncated。
    constexpr std::uint32_t declared = 1024u * 1024u; // 1MB（兆字节）
    std::vector<byte> in;
    in.reserve(1u + 3u + 100u);
    in.push_back(byte{0x43}); // ASCII，lenBytes=3（声明超大 length）
    in.push_back(byte{static_cast<std::uint8_t>((declared >> 16) & 0xFFu)});
    in.push_back(byte{static_cast<std::uint8_t>((declared >> 8) & 0xFFu)});
    in.push_back(byte{static_cast<std::uint8_t>(declared & 0xFFu)});
    in.insert(in.end(), 100u, byte{'a'});

    Item out = placeholder_item();
    std::size_t consumed = 0;
    const auto ec = decode_one(bytes_view{in.data(), in.size()}, out, consumed);
    TEST_EXPECT_EQ(ec, make_error_code(errc::truncated));
    TEST_EXPECT_EQ(consumed, 0u);
}

void test_decode_large_binary_length_truncated() {
    // 恶意输入：Binary 声明超大 length，但负载实际不足：
    // - 若超过解码硬上限，应优先返回 payload_too_large；
    // - 关键点：不得提前分配超大缓冲区。
    const std::vector<byte> in{
        byte{0x23}, // Binary（二进制），lenBytes=3
        byte{0xFF},
        byte{0xFF},
        byte{0xFF}, // length=kMaxLength（16MB 上限）
        byte{0x00}, // 负载截断
    };

    Item out = placeholder_item();
    std::size_t consumed = 0;
    const auto ec = decode_one(bytes_view{in.data(), in.size()}, out, consumed);
    TEST_EXPECT_EQ(ec, make_error_code(errc::payload_too_large));
    TEST_EXPECT_EQ(consumed, 0u);
}

void test_decode_binary_length_truncated() {
    // Binary 声明 length=2，但实际只有 1 字节 payload，必须返回 truncated。
    const std::vector<byte> in{
        byte{0x21}, // Binary，lenBytes=1
        byte{0x02}, // length=2
        byte{0xAA}, // payload 被截断（只有 1B）
    };

    Item out = placeholder_item();
    std::size_t consumed = 0;
    const auto ec = decode_one(bytes_view{in.data(), in.size()}, out, consumed);
    TEST_EXPECT_EQ(ec, make_error_code(errc::truncated));
    TEST_EXPECT_EQ(consumed, 0u);
}

void test_encode_to_buffer_overflow() {
    const auto item = Item::list({
        Item::ascii("hello"),
        Item::u2(std::vector<std::uint16_t>{0x1234u}),
    });

    std::size_t need = 0;
    TEST_EXPECT_OK(encoded_size(item, need));
    TEST_EXPECT(need > 0u);

    std::vector<byte> out(need - 1);
    std::size_t written = 0;
    const auto ec = encode_to(
        secs::ii::mutable_bytes_view{out.data(), out.size()}, item, written);
    TEST_EXPECT_EQ(ec, make_error_code(errc::buffer_overflow));
}

void test_encode_to_buffer_overflow_paths() {
    // 1) 连格式字节都放不下
    {
        const auto item = Item::ascii("A");
        std::vector<byte> out;
        std::size_t written = 0;
        const auto ec =
            encode_to(secs::ii::mutable_bytes_view{out.data(), out.size()},
                      item,
                      written);
        TEST_EXPECT_EQ(ec, make_error_code(errc::buffer_overflow));
        TEST_EXPECT_EQ(written, 0u);
    }

    // 2) 格式字节写入成功，但 length 字段写不下（write_be_u32 溢出）
    {
        const auto item = Item::ascii("A");
        std::vector<byte> out(1);
        std::size_t written = 0;
        const auto ec =
            encode_to(secs::ii::mutable_bytes_view{out.data(), out.size()},
                      item,
                      written);
        TEST_EXPECT_EQ(ec, make_error_code(errc::buffer_overflow));
        TEST_EXPECT_EQ(written, 1u);
    }

    // 3) 头部写入成功，但负载写不下（write_bytes 溢出）
    {
        const auto item = Item::ascii("AA"); // header=2，负载=2
        std::vector<byte> out(3);
        std::size_t written = 0;
        const auto ec =
            encode_to(secs::ii::mutable_bytes_view{out.data(), out.size()},
                      item,
                      written);
        TEST_EXPECT_EQ(ec, make_error_code(errc::buffer_overflow));
        TEST_EXPECT_EQ(written, 2u);
    }

    // 4) 多字节数值写入途中溢出（write_be_uint 内部返回）
    {
        const auto item =
            Item::u4(std::vector<std::uint32_t>{0x12345678u}); // 总长度=6
        std::vector<byte> out(5);
        std::size_t written = 0;
        const auto ec =
            encode_to(secs::ii::mutable_bytes_view{out.data(), out.size()},
                      item,
                      written);
        TEST_EXPECT_EQ(ec, make_error_code(errc::buffer_overflow));
        TEST_EXPECT_EQ(written, 5u);
    }
}

void test_length_overflow_limits() {
    // Boolean 的 length=values.size()，这里构造 length=kMaxLength+1 触发
    // length_overflow
    std::vector<bool> many;
    many.resize(secs::ii::kMaxLength + 1u);

    const auto item = Item::boolean(std::move(many));

    std::size_t size = 0;
    TEST_EXPECT_EQ(encoded_size(item, size),
                   make_error_code(errc::length_overflow));

    std::vector<byte> out;
    TEST_EXPECT_EQ(encode(item, out), make_error_code(errc::length_overflow));

    std::size_t written = 0;
    std::vector<byte> buf;
    TEST_EXPECT_EQ(
        encode_to(secs::ii::mutable_bytes_view{buf.data(), buf.size()},
                  item,
                  written),
        make_error_code(errc::length_overflow));

    // ASCII 的 length=value.size()，同样覆盖 length_overflow 分支
    {
        auto big = std::string(secs::ii::kMaxLength + 1u, 'A');
        const auto ascii_item = Item::ascii(std::move(big));

        std::size_t ascii_size = 0;
        TEST_EXPECT_EQ(encoded_size(ascii_item, ascii_size),
                       make_error_code(errc::length_overflow));

        std::vector<byte> ascii_out;
        TEST_EXPECT_EQ(encode(ascii_item, ascii_out),
                       make_error_code(errc::length_overflow));

        std::size_t ascii_written = 0;
        std::vector<byte> ascii_buf;
        TEST_EXPECT_EQ(encode_to(secs::ii::mutable_bytes_view{ascii_buf.data(),
                                                              ascii_buf.size()},
                                 ascii_item,
                                 ascii_written),
                       make_error_code(errc::length_overflow));
    }
}

} // namespace

int main() {
    test_roundtrip_all_types();
    test_error_category_and_messages();
    test_item_comparisons();
    test_length_field_boundaries_ascii();
    test_length_field_boundaries_list();
    test_stream_decode_consumed();
    test_decode_errors();
    test_decode_deep_list_nesting_rejected();
    test_decode_depth_limit_boundary();
    test_decode_depth_limit_plus_one_rejected();
    test_decode_large_ascii_length_truncated();
    test_decode_large_binary_length_truncated();
    test_decode_binary_length_truncated();
    test_decode_boolean_non_zero_normalized_to_true();
    test_decode_list_too_large_rejected();
    test_decode_total_item_budget_exceeded_rejected();
    test_decode_total_byte_budget_exceeded_before_payload_read();
    test_encode_to_buffer_overflow();
    test_encode_to_buffer_overflow_paths();
    test_length_overflow_limits();
    return ::secs::tests::run_and_report();
}
