#include "warp/server.h"

#include <folly/io/async/AsyncSignalHandler.h>
#include <folly/io/async/ScopedEventBaseThread.h>

#include <span>
#include <thread>

#include "warp/utils/signal.h"

namespace warp {
namespace {
std::unique_ptr<utils::SignalHandler> signal;
}  // namespace

Server::Server(ServerOptions const& options) : options_(std::make_shared<ServerOptions>(options)) {
  if (!options_->signals.empty()) {
    utils::maskSignals(options_->signals);
    signal =
        std::make_unique<utils::SignalHandler>(options_->signals, [this](int) { this->stop(); });
  }
}

Server::~Server() = default;

void Server::start() {
  http_ = std::make_unique<http::Server>(options_->http);
  mqtt_ = std::make_unique<mqtt::Server>(options_->mqtt);
  http_->addHandler(options_->mqtt.path, mqtt_->getHandlerFactory());
  std::thread http_thread([&]() { http_->start(); });
  std::thread mqtt_thread([&]() { mqtt_->start(); });
  http_thread.join();
  mqtt_thread.join();
}

void Server::stop() {
  if (http_) {
    http_->stop();
    http_.reset();
  }
  if (mqtt_) {
    mqtt_->stop();
    mqtt_.reset();
  }
}
}  // namespace warp
