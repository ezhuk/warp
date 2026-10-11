#pragma once

#include <proxygen/httpserver/HTTPServer.h>
#include <proxygen/httpserver/RequestHandlerFactory.h>

#include <memory>
#include <unordered_map>

namespace warp::http {
class ServerOptions {
public:
  uint16_t port{8080};
  size_t threads{0};
  std::chrono::seconds timeout{60};
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
  std::shared_ptr<proxygen::HTTPServer> server_;
  std::unordered_map<std::string, std::shared_ptr<proxygen::RequestHandlerFactory>> handlers_;
};
}  // namespace warp::http
