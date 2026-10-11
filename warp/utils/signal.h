#pragma once

#include <folly/Function.h>
#include <folly/io/async/AsyncSignalHandler.h>
#include <folly/io/async/ScopedEventBaseThread.h>

#include <span>

namespace warp::utils {
class SignalHandler final : private folly::ScopedEventBaseThread,
                            private folly::AsyncSignalHandler {
public:
  using SignalCallback = folly::Function<void(int)>;

  SignalHandler(std::span<int const> signals, SignalCallback func);

private:
  void signalReceived(int signum) noexcept override;

  SignalCallback func_;
};

int maskSignals(std::span<int const> signals, bool block = true);
}  // namespace warp::utils
