#pragma once

#include <folly/system/HardwareConcurrency.h>

#include <memory>

namespace warp::mqtt {
class ServerOptions {
public:
  uint16_t port = 1883;
  size_t threads = std::max(4u, folly::hardware_concurrency());
};

class Server final {
public:
  explicit Server(ServerOptions const& options);
  virtual ~Server();

  void start();
  void stop();

private:
  std::shared_ptr<ServerOptions> options_;
};
}  // namespace warp::mqtt
