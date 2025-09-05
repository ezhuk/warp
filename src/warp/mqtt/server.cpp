#include <fmt/core.h>
#include <warp/mqtt/server.h>

namespace warp::mqtt {
Server::Server() {}
Server::~Server() {}

void Server::start() { fmt::print("Server::start\n"); }
}  // namespace warp::mqtt
