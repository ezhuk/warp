// server.h

#pragma once

#include <csignal>
#include <vector>

namespace warp {
class ServerOptions final {
public:
  std::vector<int> signals{SIGINT, SIGTERM};
};

class Server final {
public:
  explicit Server(ServerOptions options = {});
  ~Server();

  void start();
  void stop();
  void join();

  // Starts the server and blocks the calling thread until one of the shutdown signals specified in
  // the options is received.
  void run();

private:
  ServerOptions options_;
};
}  // namespace warp
