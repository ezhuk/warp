#include "warp/server.h"

#include <folly/io/async/AsyncSignalHandler.h>
#include <folly/io/async/ScopedEventBaseThread.h>

#include <memory>
#include <span>
#include <thread>

namespace warp {
namespace {
class SignalHandler final : private folly::ScopedEventBaseThread,
                            private folly::AsyncSignalHandler {
public:
  using SignalCallback = folly::Function<void(int)>;

  SignalHandler(std::span<int const> signals, SignalCallback func)
      : folly::ScopedEventBaseThread(),
        folly::AsyncSignalHandler(this->folly::ScopedEventBaseThread::getEventBase()),
        func_(std::move(func)) {
    for (auto signal : signals) {
      registerSignalHandler(signal);
    }
  }

private:
  void signalReceived(int signum) noexcept override {
    if (func_) {
      func_(signum);
    }
  }

  SignalCallback func_;
};

int maskSignals(std::span<int const> signals, bool block = true) {
  sigset_t set;
  sigemptyset(&set);
  for (auto signal : signals) {
    sigaddset(&set, signal);
  }
  return pthread_sigmask(block ? SIG_BLOCK : SIG_UNBLOCK, &set, nullptr);
}

std::unique_ptr<SignalHandler> signal;
std::unique_ptr<warp::http::Server> http_server;
std::unique_ptr<warp::mqtt::Server> mqtt_server;
}  // namespace

Server::Server(ServerOptions const& options) : options_(std::make_shared<ServerOptions>(options)) {
}

Server::~Server() {}

void Server::start() {
  if (!options_->signals.empty()) {
    maskSignals(options_->signals);
    signal = std::make_unique<SignalHandler>(options_->signals, [this](int) {
      this->stop();
    });
  }
  http_server = std::make_unique<warp::http::Server>(options_->http);
  mqtt_server = std::make_unique<warp::mqtt::Server>(options_->mqtt);
  std::thread http_thread([&]() { http_server->start(); });
  std::thread mqtt_thread([&]() { mqtt_server->start(); });
  http_thread.join();
  mqtt_thread.join();
}

void Server::stop() {
    if (http_server) {
        http_server->stop();
        http_server.reset();
    }
    if (mqtt_server) {
        mqtt_server->stop();
        mqtt_server.reset();
    }
}
}  // namespace warp
