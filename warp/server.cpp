// server.cpp

#include "server.h"

#include "scoped_event_base.h"

namespace warp {
Server::Server(ServerOptions options) : options_(std::move(options)) {
  // empty
}

Server::~Server() {
  stop();
  join();
}

void Server::start() {
  // TODO
}

void Server::stop() {
  // TODO
}

void Server::join() {
  // TODO
}

void Server::run() {
  warp::ScopedEventBase base([this](int) { stop(); }, options_.signals);
  start();
  base.run();
  join();
}
}  // namespace warp
