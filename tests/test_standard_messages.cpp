#include "secs/messages/standard.hpp"

#include "secs/protocol/typed_handler.hpp"

#include "test_main.hpp"

namespace {

using secs::ii::Item;
using secs::ii::List;

using secs::messages::S1F1Request;
using secs::messages::S1F2Response;
using secs::messages::S2F13Request;
using secs::messages::S2F14Response;

void test_s1f1_roundtrip() {
    S1F1Request req{};
    auto item = req.to_item();
    auto parsed = S1F1Request::from_item(item);
    TEST_EXPECT(parsed.has_value());
}

void test_s1f2_roundtrip() {
    S1F2Response rsp{"MDLN", "SOFTREV"};
    auto item = rsp.to_item();
    auto parsed = S1F2Response::from_item(item);
    TEST_EXPECT(parsed.has_value());
    TEST_EXPECT_EQ(parsed->mdln, std::string("MDLN"));
    TEST_EXPECT_EQ(parsed->softrev, std::string("SOFTREV"));
}

void test_s2f13_roundtrip() {
    S2F13Request req{};
    req.ecids = {1U, 2U, 3U};
    auto item = req.to_item();
    auto parsed = S2F13Request::from_item(item);
    TEST_EXPECT(parsed.has_value());
    TEST_EXPECT_EQ(parsed->ecids.size(), std::size_t{3});
    TEST_EXPECT_EQ(parsed->ecids[0], 1U);
    TEST_EXPECT_EQ(parsed->ecids[1], 2U);
    TEST_EXPECT_EQ(parsed->ecids[2], 3U);
}

void test_s2f14_roundtrip() {
    S2F14Response rsp{};
    rsp.ecvs = {"A", "B"};
    auto item = rsp.to_item();
    auto parsed = S2F14Response::from_item(item);
    TEST_EXPECT(parsed.has_value());
    TEST_EXPECT_EQ(parsed->ecvs.size(), std::size_t{2});
    TEST_EXPECT_EQ(parsed->ecvs[0], std::string("A"));
    TEST_EXPECT_EQ(parsed->ecvs[1], std::string("B"));
}

void test_invalid_structures() {
    // S1F1 必须是 List
    {
        auto parsed = S1F1Request::from_item(Item::ascii("x"));
        TEST_EXPECT(!parsed.has_value());
    }

    // S1F2 必须是 2 元素 List，且都是 ASCII
    {
        auto parsed = S1F2Response::from_item(Item::list({}));
        TEST_EXPECT(!parsed.has_value());
    }
    {
        auto parsed = S1F2Response::from_item(
            Item::list({Item::ascii("x"), Item::u4({1})}));
        TEST_EXPECT(!parsed.has_value());
    }

    // S2F13：每个元素必须是单值 U4
    {
        auto parsed = S2F13Request::from_item(
            Item::list({Item::u4({1, 2})}));
        TEST_EXPECT(!parsed.has_value());
    }

    // S2F14：每个元素必须是 ASCII
    {
        auto parsed = S2F14Response::from_item(
            Item::list({Item::u4({1})}));
        TEST_EXPECT(!parsed.has_value());
    }
}

void test_secs_message_concept() {
    static_assert(secs::protocol::SecsMessage<S1F1Request>);
    static_assert(secs::protocol::SecsMessage<S1F2Response>);
    static_assert(secs::protocol::SecsMessage<S2F13Request>);
    static_assert(secs::protocol::SecsMessage<S2F14Response>);
}

} // namespace

int main() {
    test_s1f1_roundtrip();
    test_s1f2_roundtrip();
    test_s2f13_roundtrip();
    test_s2f14_roundtrip();
    test_invalid_structures();
    test_secs_message_concept();
    return secs::tests::run_and_report();
}

