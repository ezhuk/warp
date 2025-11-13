#pragma once

#include <proxygen/httpserver/RequestHandlerFactory.h>

#include <memory>

namespace warp::http {
class ServerOptions {
public:
  uint16_t port{8080};
  size_t threads{0};
};

class Server final {
public:
  explicit Server(ServerOptions const& options);
  virtual ~Server();

  void start();
  void stop();

  void addHandler(
      std::string const& path, std::shared_ptr<proxygen::RequestHandlerFactory> handler
  );

private:
  std::shared_ptr<ServerOptions> options_;
};
}  // namespace warp::http
