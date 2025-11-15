#pragma once

#include <proxygen/httpserver/RequestHandlerFactory.h>

#include <memory>

namespace warp::mqtt {
class ServerOptions {
public:
  uint16_t port{1883};
  size_t threads{0};
  std::string path{"/mqtt"};
};

class Server final {
public:
  explicit Server(ServerOptions const& options);
  virtual ~Server();

  void start();
  void stop();

  std::shared_ptr<proxygen::RequestHandlerFactory> getHandlerFactory();

private:
  std::shared_ptr<ServerOptions> options_;
};
}  // namespace warp::mqtt
