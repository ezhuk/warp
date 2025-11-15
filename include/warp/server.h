#pragma once

#include <warp/http/server.h>
#include <warp/mqtt/server.h>

#include <csignal>
#include <memory>

namespace warp {
class ServerOptions {
public:
  std::vector<int> signals{SIGINT, SIGTERM};
  http::ServerOptions http{};
  mqtt::ServerOptions mqtt{};
};

class Server final {
public:
  explicit Server(ServerOptions const& options);
  virtual ~Server();

  void start();
  void stop();

private:
  std::shared_ptr<ServerOptions> options_;
  std::unique_ptr<http::Server> http_;
  std::unique_ptr<mqtt::Server> mqtt_;
};
}  // namespace warp
