#include <fmt/core.h>
#include <warp/server.h>

int main() {
  fmt::print("START\n");
  warp::Server server({});
  server.start();
  fmt::print("DONE\n");
  return 0;
}
