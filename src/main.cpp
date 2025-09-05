#include <fmt/core.h>

#include <warp/mqtt/server.h>

int main() {
  fmt::print("OK\n");
  warp::mqtt::Server server;
  server.start();
  return 0;
}
