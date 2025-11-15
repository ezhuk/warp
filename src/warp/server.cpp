#include "warp/server.h"

#include <folly/io/async/AsyncSignalHandler.h>
#include <folly/io/async/ScopedEventBaseThread.h>

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
}  // namespace

Server::Server(ServerOptions const& options) : options_(std::make_shared<ServerOptions>(options)) {}

Server::~Server() {}

void Server::start() {
  if (!options_->signals.empty()) {
    maskSignals(options_->signals);
    signal = std::make_unique<SignalHandler>(options_->signals, [this](int) { this->stop(); });
  }
  http_ = std::make_unique<warp::http::Server>(options_->http);
  mqtt_ = std::make_unique<warp::mqtt::Server>(options_->mqtt);
  http_->addHandler("/mqtt", mqtt_->getHandlerFactory());
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
