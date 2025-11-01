#include <fmt/core.h>
#include <folly/io/async/AsyncSignalHandler.h>
#include <folly/io/async/ScopedEventBaseThread.h>
#include <warp/http/server.h>
#include <warp/mqtt/server.h>

#include <span>
#include <thread>

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
}  // namespace

int main() {
  fmt::print("START\n");
  std::vector<int> signals{SIGINT, SIGTERM};
  maskSignals(signals);
  warp::http::Server http({});
  warp::mqtt::Server mqtt({});
  std::thread http_thread([&]() { http.start(); });
  std::thread mqtt_thread([&]() { mqtt.start(); });
  std::unique_ptr<SignalHandler> signal = std::make_unique<SignalHandler>(signals, [&](int) {
    http.stop();
    mqtt.stop();
  });
  http_thread.join();
  mqtt_thread.join();
  fmt::print("DONE\n");
  return 0;
}
