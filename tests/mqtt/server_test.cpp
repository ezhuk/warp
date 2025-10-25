#include "warp/mqtt/server.h"

#include <gtest/gtest.h>

class ServerTest : public ::testing::Test {
protected:
  void SetUp() override {
    warp::mqtt::ServerOptions options;
    options.port = port_;
    server_ = std::make_unique<warp::mqtt::Server>(options);
    thread_ = std::thread([this]() { server_->start(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
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
  // TODO
}
