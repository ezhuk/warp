// scoped_event_base.h

#include <folly/io/async/AsyncSignalHandler.h>
#include <folly/io/async/EventBase.h>

#include <csignal>

namespace warp {
// Runs an EventBase loop on the calling thread and provides built-in asynchronous
// shutdown signal handling.
template <typename Callback>
class ScopedEventBase final : private folly::EventBase, private folly::AsyncSignalHandler {
public:
  explicit ScopedEventBase(Callback&& callback)
      : folly::EventBase(),
        folly::AsyncSignalHandler(static_cast<folly::EventBase*>(this)),
        callback_(std::forward<Callback>(callback)) {
    registerSignalHandler(SIGINT);
    registerSignalHandler(SIGTERM);
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
