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

class Service final {
public:
  using Context = typename wangle::HandlerAdapter<Message, Message>::Context;

  Message operator()(Context* ctx, Message msg) {
    return std::visit(
        [ctx](auto&& m) -> Message {
          using T = std::decay_t<decltype(m)>;
          if constexpr (std::is_same_v<T, Connect>) {
            return ConnAck::Builder{}.withSession(0).withReason(0).build();
          } else if constexpr (std::is_same_v<T, Publish>) {
            if (m.head.qos == 1) {
              return PubAck::Builder{}.withPacketId(m.head.packetId).build();
            } else if (m.head.qos == 2) {
              return PubRec::Builder{}.withPacketId(m.head.packetId).build();
            }
            return None{};
          } else if constexpr (std::is_same_v<T, PubRel>) {
            return PubComp::Builder{}.withPacketId(m.head.packetId).build();
          } else if constexpr (std::is_same_v<T, Subscribe>) {
            return SubAck::Builder{}.withPacketId(m.head.packetId).withCodesFrom(m).build();
          } else if constexpr (std::is_same_v<T, Unsubscribe>) {
            return UnsubAck::Builder{}.withPacketId(m.head.packetId).build();
          } else if constexpr (std::is_same_v<T, PingReq>) {
            return PingResp::Builder{}.build();
          } else {
            return None{};
          }
        },
        std::move(msg)
    );
  }
};

class ServiceDispatcher : public wangle::HandlerAdapter<Message, Message> {
public:
  using Context = typename wangle::HandlerAdapter<Message, Message>::Context;

  ServiceDispatcher(
      std::shared_ptr<folly::CPUThreadPoolExecutor> executor, std::shared_ptr<Service> service
  )
      : executor_(std::move(executor)), service_(std::move(service)) {}

  void read(Context* ctx, Message req) override {
    auto* evb = ctx->getTransport()->getEventBase();
    auto* svc = service_.get();
    folly::via(
        executor_.get(),
        [svc, ctx, msg = std::move(req)]() mutable -> Message {
          return (*svc)(ctx, std::move(msg));
        }
    )
        .via(evb)
        .thenValue([ctx](Message res) mutable {
          if (!std::holds_alternative<None>(res)) {
            ctx->fireWrite(std::move(res));
          }
        });
  }

private:
  std::shared_ptr<folly::CPUThreadPoolExecutor> executor_;
  std::shared_ptr<Service> service_;
};

using Pipeline = wangle::Pipeline<folly::IOBufQueue&, Message>;

class PipelineFactory final : public wangle::PipelineFactory<Pipeline> {
public:
  explicit PipelineFactory(size_t threads)
      : executor_(std::make_shared<folly::CPUThreadPoolExecutor>(threads)),
        service_(std::make_shared<Service>()) {}

  Pipeline::Ptr newPipeline(std::shared_ptr<folly::AsyncTransport> sock) override {
    auto pipeline = Pipeline::create();
    pipeline->addBack(wangle::AsyncSocketHandler(sock));
    pipeline->addBack(wangle::EventBaseHandler());
    pipeline->addBack(Handler());
    pipeline->addBack(ServiceDispatcher(executor_, service_));
    pipeline->finalize();
    return pipeline;
  }

private:
  std::shared_ptr<folly::CPUThreadPoolExecutor> executor_;
  std::shared_ptr<Service> service_;
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
