#include "warp/mqtt/server.h"

#include <fmt/format.h>
#include <folly/Function.h>
#include <folly/executors/CPUThreadPoolExecutor.h>
#include <folly/io/async/AsyncSignalHandler.h>
#include <folly/io/async/AsyncTimeout.h>
#include <folly/io/async/ScopedEventBaseThread.h>
#include <folly/io/async/TimeoutManager.h>
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
namespace {
struct DataTraits {
  static inline const folly::RequestToken kToken{"warp.mqtt.handler"};
};
}  // namespace

class Handler final
    : public wangle::Handler<folly::IOBufQueue&, Message, Message, std::unique_ptr<folly::IOBuf>> {
public:
  using Context = typename wangle::Handler<
      folly::IOBufQueue&, Message, Message, std::unique_ptr<folly::IOBuf>>::Context;

  Handler() : context_(std::make_shared<folly::RequestContext>()) {}

  void read(Context* ctx, folly::IOBufQueue& q) override {
    folly::RequestContextScopeGuard guard(context_);
    for (;;) {
      auto msg = Codec::decode(q);
      if (!msg) {
        break;
      }
      ctx->fireRead(std::move(*msg));
    }
    if (timeout_) {
      timeout_->scheduleTimeout(std::chrono::seconds(90));
    }
  }

  folly::Future<folly::Unit> write(Context* ctx, Message msg) override {
    auto out = Codec::encode(msg);
    return ctx->fireWrite(std::move(out));
  }

  void transportActive(Context* ctx) override {
    context_->setContextDataIfAbsent(
        DataTraits::kToken, std::make_unique<folly::ImmutableRequestData<Handler*>>(this)
    );
    if (!timeout_) {
      timeout_ = folly::AsyncTimeout::schedule(
          std::chrono::seconds(90),
          static_cast<folly::TimeoutManager&>(*ctx->getTransport()->getEventBase()),
          [ctx]() noexcept { ctx->fireClose(); }
      );
    }
    ctx->fireTransportActive();
  }

  void transportInactive(Context* ctx) override {
    timeout_.reset();
    ctx->fireTransportInactive();
  }

private:
  std::shared_ptr<folly::RequestContext> context_;
  std::unique_ptr<folly::AsyncTimeout> timeout_;
};

namespace {
Handler* getHandler() noexcept {
  if (auto* rc = folly::RequestContext::try_get()) {
    if (auto* d = rc->getContextData(DataTraits::kToken)) {
      if (auto* p = static_cast<folly::ImmutableRequestData<Handler*>*>(d)) {
        return p->value();
      }
    }
  }
  return nullptr;
}
}  // namespace

class Service final : public wangle::Service<Message, Message> {
public:
  folly::Future<Message> operator()(Message msg) override {
    return std::visit(
        [](auto&& m) -> folly::Future<Message> {
          using T = std::decay_t<decltype(m)>;
          if constexpr (std::is_same_v<T, Connect>) {
            return folly::makeFuture<Message>(
                ConnAck::Builder{}.withSession(0).withReason(0).build()
            );
          } else if constexpr (std::is_same_v<T, Publish>) {
            if (m.head.qos == 1) {
              return folly::makeFuture<Message>(
                  PubAck::Builder{}.withPacketId(m.head.packetId).build()
              );
            } else if (m.head.qos == 2) {
              return folly::makeFuture<Message>(
                  PubRec::Builder{}.withPacketId(m.head.packetId).build()
              );
            }
            return folly::makeFuture<Message>(None{});
          } else if constexpr (std::is_same_v<T, PubRel>) {
            return folly::makeFuture<Message>(
                PubComp::Builder{}.withPacketId(m.head.packetId).build()
            );
          } else if constexpr (std::is_same_v<T, Subscribe>) {
            return folly::makeFuture<Message>(
                SubAck::Builder{}.withPacketId(m.head.packetId).withCodesFrom(m).build()
            );
          } else if constexpr (std::is_same_v<T, Unsubscribe>) {
            return folly::makeFuture<Message>(
                UnsubAck::Builder{}.withPacketId(m.head.packetId).build()
            );
          } else if constexpr (std::is_same_v<T, PingReq>) {
            return folly::makeFuture<Message>(PingResp::Builder{}.build());
          } else {
            return folly::makeFuture<Message>(None{});
          }
        },
        std::move(msg)
    );
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
