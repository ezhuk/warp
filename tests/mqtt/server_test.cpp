#include "warp/mqtt/server.h"

#include <gtest/gtest.h>

#include "warp/mqtt/client.h"

class ServerTest : public ::testing::Test {
protected:
  void SetUp() override {
    warp::mqtt::ServerOptions options;
    options.port = port_;
    server_ = std::make_unique<warp::mqtt::Server>(options);
    thread_ = std::thread([this]() { server_->start(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  void TearDown() override {
    if (server_) {
      server_->stop();
    }
    if (thread_.joinable()) {
      thread_.join();
    }
  }

  static constexpr uint16_t port_ = 11883;
  std::unique_ptr<warp::mqtt::Server> server_;
  std::thread thread_;
};

TEST_F(ServerTest, ConnectTest) {
  warp::mqtt::ClientOptions options;
  options.port = port_;
  warp::mqtt::Client client(options);
  client.connect();
  auto res = client
                 .request(
                     warp::mqtt::Connect::Builder{}
                         .withLevel(warp::mqtt::Level::V311)
                         .withCleanSession(true)
                         .withKeepAlive(30)
                         .withClient("test")
                         .build()
                 )
                 .get();
  ASSERT_TRUE(std::holds_alternative<warp::mqtt::ConnAck>(res));
  auto const& ack = std::get<warp::mqtt::ConnAck>(res);
  EXPECT_EQ(ack.head.reason, 0);
  client.close();
}
