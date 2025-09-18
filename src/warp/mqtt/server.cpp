#include "warp/mqtt/server.h"

#include <fmt/format.h>
#include <folly/Function.h>
#include <folly/executors/CPUThreadPoolExecutor.h>
#include <folly/io/async/AsyncSignalHandler.h>
#include <folly/io/async/ScopedEventBaseThread.h>
#include <folly/system/HardwareConcurrency.h>
#include <wangle/bootstrap/ServerBootstrap.h>
#include <wangle/channel/AsyncSocketHandler.h>
#include <wangle/channel/EventBaseHandler.h>
#include <wangle/channel/Pipeline.h>
#include <wangle/service/ExecutorFilter.h>
#include <wangle/service/ServerDispatcher.h>

#include <span>

#include "warp/mqtt/codec.h"

namespace warp::mqtt {
class Handler final
    : public wangle::Handler<folly::IOBufQueue&, Message, Message, std::unique_ptr<folly::IOBuf>> {
public:
  using Context = typename wangle::Handler<
      folly::IOBufQueue&, Message, Message, std::unique_ptr<folly::IOBuf>>::Context;

  void read(Context* ctx, folly::IOBufQueue& q) override {
    for (;;) {
      auto msg = Codec::decode(q);
      if (!msg) {
        break;
      }
      ctx->fireRead(std::move(*msg));
    }
  }

  folly::Future<folly::Unit> write(Context* ctx, Message msg) override {
    auto out = Codec::encode(msg);
    return ctx->fireWrite(std::move(out));
  }
};

class Service final : public wangle::Service<Message, Message> {
public:
  folly::Future<Message> operator()(Message msg) override {
    switch (msg.type) {
      case Type::Connect:
        return folly::makeFuture<Message>(Message{.type = Type::ConnAck});
      case Type::Publish:
        return folly::makeFuture<Message>(Message{.type = Type::None});
      case Type::Subscribe:
        return folly::makeFuture<Message>(
            Message{.type = Type::SubAck, .id = msg.id, .topics = msg.topics}
        );
      case Type::PingReq:
        return folly::makeFuture<Message>(Message{.type = Type::PingResp});
      case Type::Disconnect:
        return folly::makeFuture<Message>(Message{.type = Type::None});
      default:
        break;
    }
    return folly::makeFuture<Message>(Message{.type = Type::None});
  }
};

using Pipeline = wangle::Pipeline<folly::IOBufQueue&, Message>;

class PipelineFactory final : public wangle::PipelineFactory<Pipeline> {
public:
  explicit PipelineFactory(size_t threads)
      : service_(
            std::make_shared<folly::CPUThreadPoolExecutor>(threads), std::make_shared<Service>()
        ) {}

  Pipeline::Ptr newPipeline(std::shared_ptr<folly::AsyncTransport> sock) override {
    auto pipeline = Pipeline::create();
    pipeline->addBack(wangle::AsyncSocketHandler(sock));
    pipeline->addBack(wangle::EventBaseHandler());
    pipeline->addBack(Handler());
    pipeline->addBack(wangle::MultiplexServerDispatcher<Message, Message>(&service_));
    pipeline->finalize();
    return pipeline;
  }

private:
  wangle::ExecutorFilter<Message, Message> service_;
};

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

std::shared_ptr<wangle::ServerBootstrap<Pipeline>> server;
std::unique_ptr<SignalHandler> signal;
}  // namespace

Server::Server(ServerOptions const& options) : options_(std::make_shared<ServerOptions>(options)) {
  if (0 == options_->threads) {
    options_->threads = std::max(4u, folly::hardware_concurrency());
  }
  maskSignals(options_->signals);
}

Server::~Server() {}

void Server::start() {
  signal = std::make_unique<SignalHandler>(options_->signals, [this](int) { this->stop(); });
  server = std::make_shared<wangle::ServerBootstrap<Pipeline>>();
  server->childPipeline(std::make_shared<PipelineFactory>(options_->threads));
  server->bind(options_->port);
  server->waitForStop();
  server.reset();
  signal.reset();
}

void Server::stop() { server->stop(); }
}  // namespace warp::mqtt
