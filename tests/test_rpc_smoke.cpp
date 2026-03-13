#include "secs/rpc/contracts.hpp"
#include "secs/rpc/server.hpp"

#include "test_main.hpp"

#include <string>

int main() {
    secs::rpc::v1::GetLibraryInfoResponse response;
    response.mutable_status()->set_ok(true);
    response.add_supported_transports("HSMS");

    TEST_EXPECT(response.has_status());
    TEST_EXPECT(response.status().ok());
    TEST_EXPECT_EQ(response.supported_transports_size(), 1);
    TEST_EXPECT_EQ(response.supported_transports(0), std::string{"HSMS"});

    secs::rpc::Server server;
    TEST_EXPECT(!server.running());

    brpc::Server *raw_server = &server.raw();
    TEST_EXPECT(raw_server != nullptr);

    secs::rpc::ServerOptions options;
    TEST_EXPECT_EQ(options.listen_address, std::string{"0.0.0.0:50051"});

    return secs::tests::run_and_report();
}
