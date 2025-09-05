#pragma once

namespace warp::mqtt {
class Server {
public:
  Server();
  virtual ~Server();

  void start();
};
}  // namespace warp::mqtt
