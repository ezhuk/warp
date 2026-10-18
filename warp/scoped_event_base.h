// scoped_event_base.h

#pragma once

#include <folly/io/async/AsyncSignalHandler.h>
#include <folly/io/async/EventBase.h>

#include <csignal>
#include <utility>
#include <vector>

namespace warp {
// Runs an EventBase loop on the calling thread and provides built-in asynchronous
// shutdown handling for the specified signals.
template <typename Callback>
class ScopedEventBase final : private folly::EventBase, private folly::AsyncSignalHandler {
public:
  explicit ScopedEventBase(Callback&& callback, std::vector<int> const& signals = {SIGINT, SIGTERM})
      : folly::EventBase(),
        folly::AsyncSignalHandler(static_cast<folly::EventBase*>(this)),
        callback_(std::forward<Callback>(callback)) {
    for (int signum : signals) {
      registerSignalHandler(signum);
    }
  }

  void run() { folly::EventBase::loopForever(); }

private:
  void signalReceived(int signum) noexcept override {
    callback_(signum);
    terminateLoopSoon();
  }

  Callback callback_;
};
}  // namespace warp
