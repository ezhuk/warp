#include <fmt/core.h>
#include <warp/mqtt/server.h>

#include <thread>

int main() {
  fmt::print("START\n");
  warp::mqtt::ServerOptions options;
  warp::mqtt::Server server(options);
  std::thread thread([&]() { server.start(); });
  thread.join();
  fmt::print("DONE\n");
  return 0;
}
