#pragma once

#include <warp/http/server.h>
#include <warp/mqtt/server.h>

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
};
}  // namespace warp
