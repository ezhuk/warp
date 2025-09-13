#include <fmt/core.h>
#include <warp/mqtt/server.h>

#include <thread>

int main() {
  fmt::print("OK\n");
  warp::mqtt::ServerOptions options;
  warp::mqtt::Server server(options);
  std::thread t([&]() { server.start(); });
  t.join();
  return 0;
}
