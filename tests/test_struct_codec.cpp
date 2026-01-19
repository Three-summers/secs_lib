#include "secs/ii/struct_codec.hpp"

#include "secs/protocol/typed_handler.hpp"

#include "test_main.hpp"

#include <cstdint>
#include <string>
#include <tuple>
#include <vector>

namespace {

using secs::ii::Item;

struct Empty final {
    static constexpr auto secs_members() { return std::make_tuple(); }
};

struct Fixed final {
    std::string mdln;
    std::uint16_t id{};

    static constexpr auto secs_members() {
        return std::make_tuple(&Fixed::mdln, &Fixed::id);
    }
};

struct VecOnly final {
    std::vector<std::uint32_t> ids;

    static constexpr auto secs_members() { return std::make_tuple(&VecOnly::ids); }
};

struct Inner final {
    std::string name;

    static constexpr auto secs_members() { return std::make_tuple(&Inner::name); }
};

struct Outer final {
    Inner inner{};
    std::uint8_t x{};

    static constexpr auto secs_members() {
        return std::make_tuple(&Outer::inner, &Outer::x);
    }
};

void test_empty_allows_any_list() {
    {
        auto parsed = secs::ii::from_item<Empty>(Item::list({}));
        TEST_EXPECT(parsed.has_value());
    }
    {
        auto parsed = secs::ii::from_item<Empty>(Item::list({Item::ascii("x")}));
        TEST_EXPECT(parsed.has_value());
    }
    {
        auto parsed = secs::ii::from_item<Empty>(Item::ascii("x"));
        TEST_EXPECT(!parsed.has_value());
    }
}

void test_fixed_roundtrip_and_validation() {
    Fixed msg{"MDLN", 42};
    auto item = secs::ii::to_item(msg);
    TEST_EXPECT(item == Item::list({Item::ascii("MDLN"), Item::u2({42})}));

    auto parsed = secs::ii::from_item<Fixed>(item);
    TEST_EXPECT(parsed.has_value());
    TEST_EXPECT_EQ(parsed->mdln, std::string("MDLN"));
    TEST_EXPECT_EQ(parsed->id, static_cast<std::uint16_t>(42));

    TEST_EXPECT(!secs::ii::from_item<Fixed>(Item::ascii("x")).has_value());
    TEST_EXPECT(!secs::ii::from_item<Fixed>(Item::list({Item::ascii("MDLN")})).has_value());
    TEST_EXPECT(!secs::ii::from_item<Fixed>(Item::list({Item::u2({1}), Item::u2({2})})).has_value());
    TEST_EXPECT(
        !secs::ii::from_item<Fixed>(Item::list({Item::ascii("MDLN"), Item::u2({1, 2})}))
             .has_value());
}

void test_vector_only_roundtrip_and_validation() {
    VecOnly msg{};
    msg.ids = {1U, 2U, 3U};
    auto item = secs::ii::to_item(msg);
    TEST_EXPECT(item ==
                Item::list({Item::u4({1U}), Item::u4({2U}), Item::u4({3U})}));

    auto parsed = secs::ii::from_item<VecOnly>(item);
    TEST_EXPECT(parsed.has_value());
    TEST_EXPECT_EQ(parsed->ids.size(), std::size_t{3});
    TEST_EXPECT_EQ(parsed->ids[0], 1U);
    TEST_EXPECT_EQ(parsed->ids[1], 2U);
    TEST_EXPECT_EQ(parsed->ids[2], 3U);

    TEST_EXPECT(!secs::ii::from_item<VecOnly>(Item::ascii("x")).has_value());
    TEST_EXPECT(!secs::ii::from_item<VecOnly>(Item::list({Item::u4({1, 2})})).has_value());
}

void test_nested_roundtrip() {
    Outer msg{};
    msg.inner.name = "N";
    msg.x = 7;

    auto item = secs::ii::to_item(msg);
    TEST_EXPECT(item == Item::list({Item::list({Item::ascii("N")}), Item::u1({7})}));

    auto parsed = secs::ii::from_item<Outer>(item);
    TEST_EXPECT(parsed.has_value());
    TEST_EXPECT_EQ(parsed->inner.name, std::string("N"));
    TEST_EXPECT_EQ(parsed->x, static_cast<std::uint8_t>(7));

    // inner 解析失败 -> overall 失败
    TEST_EXPECT(!secs::ii::from_item<Outer>(Item::list({Item::ascii("bad"), Item::u1({7})}))
                     .has_value());
}

void test_secs_message_concept() {
    static_assert(secs::protocol::SecsMessage<Empty>);
    static_assert(secs::protocol::SecsMessage<Fixed>);
    static_assert(secs::protocol::SecsMessage<VecOnly>);
    static_assert(secs::protocol::SecsMessage<Inner>);
    static_assert(secs::protocol::SecsMessage<Outer>);
}

} // namespace

int main() {
    test_empty_allows_any_list();
    test_fixed_roundtrip_and_validation();
    test_vector_only_roundtrip_and_validation();
    test_nested_roundtrip();
    test_secs_message_concept();
    return secs::tests::run_and_report();
}
