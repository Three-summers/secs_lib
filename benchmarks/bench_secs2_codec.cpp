#include "bench_main.hpp"
#include "secs/ii/codec.hpp"
#include "secs/ii/item.hpp"

#include <vector>
#include <string>

using namespace secs;
using namespace secs::ii;
using namespace secs::core;

static Item create_deep_nested_list(int depth) {
  // 创建深度嵌套的 List
  if (depth <= 0) {
    return Item::u4({42});
  }
  std::vector<Item> items;
  items.push_back(create_deep_nested_list(depth - 1));
  return Item::list(std::move(items));
}

static void bench_codec_deep_nested() {
  // 深度嵌套 List (64层)
  constexpr int depth = 64;

  Item item = create_deep_nested_list(depth);
  std::vector<byte> encoded;
  std::size_t encoded_bytes = 0;

  BENCH_RUN("SECS-II: Deep nested list (64 levels)", depth, 3, {
    encoded.clear();
    auto ec = encode(item, encoded);
    if (ec) {
      std::cerr << "Encode failed: " << ec.message() << "\n";
    }
    encoded_bytes = encoded.size();
  });

  // Decode benchmark
  BENCH_RUN("SECS-II: Decode deep nested list (64 levels)", encoded_bytes, 3, {
    Item decoded{Item::u1({})};
    std::size_t consumed = 0;
    auto ec = decode_one(bytes_view{encoded.data(), encoded.size()}, decoded, consumed);
    if (ec) {
      std::cerr << "Decode failed: " << ec.message() << "\n";
    }
  });
}

static void bench_codec_large_list() {
  // 大型 List (1000 items)
  constexpr std::size_t item_count = 1000;

  std::vector<Item> items;
  items.reserve(item_count);
  for (std::size_t i = 0; i < item_count; ++i) {
    items.push_back(Item::u4({static_cast<std::uint32_t>(i)}));
  }
  Item item = Item::list(std::move(items));

  std::vector<byte> encoded;
  std::size_t encoded_bytes = 0;

  BENCH_RUN("SECS-II: Large list encode (1000 items)", item_count, 3, {
    encoded.clear();
    auto ec = encode(item, encoded);
    if (ec) {
      std::cerr << "Encode failed: " << ec.message() << "\n";
    }
    encoded_bytes = encoded.size();
  });

  // Decode benchmark
  BENCH_RUN("SECS-II: Large list decode (1000 items)", encoded_bytes, 3, {
    Item decoded{Item::u1({})};
    std::size_t consumed = 0;
    auto ec = decode_one(bytes_view{encoded.data(), encoded.size()}, decoded, consumed);
    if (ec) {
      std::cerr << "Decode failed: " << ec.message() << "\n";
    }
  });
}

static void bench_codec_large_ascii() {
  // 大 ASCII (1MB)
  constexpr std::size_t ascii_size = 1024 * 1024;

  std::string large_string(ascii_size, 'A');
  Item item = Item::ascii(large_string);

  std::vector<byte> encoded;
  std::size_t encoded_bytes = 0;

  BENCH_RUN("SECS-II: Large ASCII encode (1MB)", ascii_size, 3, {
    encoded.clear();
    auto ec = encode(item, encoded);
    if (ec) {
      std::cerr << "Encode failed: " << ec.message() << "\n";
    }
    encoded_bytes = encoded.size();
  });

  // Decode benchmark
  BENCH_RUN("SECS-II: Large ASCII decode (1MB)", encoded_bytes, 3, {
    Item decoded{Item::u1({})};
    std::size_t consumed = 0;
    auto ec = decode_one(bytes_view{encoded.data(), encoded.size()}, decoded, consumed);
    if (ec) {
      std::cerr << "Decode failed: " << ec.message() << "\n";
    }
  });
}

static void bench_codec_large_binary() {
  // 大 Binary (1MB)
  constexpr std::size_t binary_size = 1024 * 1024;

  std::vector<byte> large_binary(binary_size, 0xFF);
  Item item = Item::binary(large_binary);

  std::vector<byte> encoded;
  std::size_t encoded_bytes = 0;

  BENCH_RUN("SECS-II: Large Binary encode (1MB)", binary_size, 3, {
    encoded.clear();
    auto ec = encode(item, encoded);
    if (ec) {
      std::cerr << "Encode failed: " << ec.message() << "\n";
    }
    encoded_bytes = encoded.size();
  });

  // Decode benchmark
  BENCH_RUN("SECS-II: Large Binary decode (1MB)", encoded_bytes, 3, {
    Item decoded{Item::u1({})};
    std::size_t consumed = 0;
    auto ec = decode_one(bytes_view{encoded.data(), encoded.size()}, decoded, consumed);
    if (ec) {
      std::cerr << "Decode failed: " << ec.message() << "\n";
    }
  });
}

static void bench_codec_numeric_arrays() {
  // 各种数值类型数组性能
  constexpr std::size_t array_size = 10000;

  // U4 array
  {
    std::vector<std::uint32_t> values(array_size);
    for (std::size_t i = 0; i < array_size; ++i) {
      values[i] = static_cast<std::uint32_t>(i);
    }
    Item item = Item::u4(values);
    std::vector<byte> encoded;

    BENCH_RUN("SECS-II: U4 array encode (10000 items)", array_size * sizeof(std::uint32_t), 3, {
      encoded.clear();
      auto ec = encode(item, encoded);
      if (ec) {
        std::cerr << "U4 encode failed: " << ec.message() << "\n";
      }
    });
  }

  // F8 array
  {
    std::vector<double> values(array_size);
    for (std::size_t i = 0; i < array_size; ++i) {
      values[i] = static_cast<double>(i) * 3.14159;
    }
    Item item = Item::f8(values);
    std::vector<byte> encoded;

    BENCH_RUN("SECS-II: F8 array encode (10000 items)", array_size * sizeof(double), 3, {
      encoded.clear();
      auto ec = encode(item, encoded);
      if (ec) {
        std::cerr << "F8 encode failed: " << ec.message() << "\n";
      }
    });
  }

  // I8 array
  {
    std::vector<std::int64_t> values(array_size);
    for (std::size_t i = 0; i < array_size; ++i) {
      values[i] = static_cast<std::int64_t>(i) - 5000;
    }
    Item item = Item::i8(values);
    std::vector<byte> encoded;

    BENCH_RUN("SECS-II: I8 array encode (10000 items)", array_size * sizeof(std::int64_t), 3, {
      encoded.clear();
      auto ec = encode(item, encoded);
      if (ec) {
        std::cerr << "I8 encode failed: " << ec.message() << "\n";
      }
    });
  }
}

int main() {
  bench_codec_deep_nested();
  bench_codec_large_list();
  bench_codec_large_ascii();
  bench_codec_large_binary();
  bench_codec_numeric_arrays();

  secs::benchmarks::print_results();
  return 0;
}
