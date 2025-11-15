#include "warp/utils/signal.h"

#include <csignal>

namespace warp::utils {
SignalHandler::SignalHandler(std::span<int const> signals, SignalCallback func)
    : folly::ScopedEventBaseThread(),
      folly::AsyncSignalHandler(this->folly::ScopedEventBaseThread::getEventBase()),
      func_(std::move(func)) {
  for (auto signal : signals) {
    registerSignalHandler(signal);
  }
}

void SignalHandler::signalReceived(int signum) noexcept {
  if (func_) {
    func_(signum);
  }
}

int maskSignals(std::span<int const> signals, bool block) {
  sigset_t set;
  sigemptyset(&set);
  for (auto signal : signals) {
    sigaddset(&set, signal);
  }
  return pthread_sigmask(block ? SIG_BLOCK : SIG_UNBLOCK, &set, nullptr);
}
}  // namespace warp::utils
