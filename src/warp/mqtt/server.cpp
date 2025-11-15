#include "warp/mqtt/server.h"

#include <folly/executors/CPUThreadPoolExecutor.h>
#include <folly/io/async/AsyncTimeout.h>
#include <folly/io/async/TimeoutManager.h>
#include <folly/system/HardwareConcurrency.h>
#include <wangle/bootstrap/ServerBootstrap.h>
#include <wangle/channel/AsyncSocketHandler.h>
#include <wangle/channel/EventBaseHandler.h>
#include <wangle/channel/Pipeline.h>
#include <wangle/service/ExecutorFilter.h>
#include <wangle/service/ServerDispatcher.h>

#include "warp/mqtt/codec.h"
#include "warp/websocket/handler.h"

namespace warp::mqtt {
namespace {
struct DataTraits {
  static inline const folly::RequestToken kToken{"warp.mqtt.handler"};
};
}  // namespace

class HandlerOptions {
public:
  std::chrono::seconds timeout{90};
};

class Handler final
    : public wangle::Handler<folly::IOBufQueue&, Message, Message, std::unique_ptr<folly::IOBuf>> {
public:
  using Context = typename wangle::Handler<
      folly::IOBufQueue&, Message, Message, std::unique_ptr<folly::IOBuf>>::Context;

  Handler()
      : context_(std::make_shared<folly::RequestContext>()),
        options_(std::make_unique<HandlerOptions>()) {}

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
      timeout_->scheduleTimeout(options_->timeout);
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
      timeout_ = folly::AsyncTimeout::make(
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

  void setTimeout(uint32_t timeout) {
    if (timeout_) {
      auto* evb = const_cast<folly::EventBase*>(
          dynamic_cast<folly::EventBase const*>(timeout_->getTimeoutManager())
      );
      evb->runInEventBaseThread([this, timeout]() noexcept {
        options_->timeout =
            timeout > 0 ? std::chrono::seconds(timeout) : std::chrono::seconds::zero();
        if (timeout_) {
          if (options_->timeout > std::chrono::seconds::zero()) {
            timeout_->scheduleTimeout(options_->timeout);
          } else {
            timeout_->cancelTimeout();
          }
        }
      });
    }
  }

private:
  std::shared_ptr<folly::RequestContext> context_;
  std::unique_ptr<HandlerOptions> options_;
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
            if (auto* handler = getHandler()) {
              if (0 < m.head.timeout) {
                handler->setTimeout(m.head.timeout + m.head.timeout / 2);
              }
            }
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

namespace {
std::shared_ptr<Service> service;
std::shared_ptr<wangle::ServerBootstrap<Pipeline>> server;
}  // namespace

class PipelineFactory final : public wangle::PipelineFactory<Pipeline> {
public:
  explicit PipelineFactory(size_t threads)
      : service_(std::make_shared<folly::CPUThreadPoolExecutor>(threads), service) {}

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

class WebSocketHandler final : public warp::websocket::Handler {
public:
  void onDataFrame(std::unique_ptr<folly::IOBuf> data, bool fin) override {
    queue_.append(std::move(data));
    for (;;) {
      auto msg = Codec::decode(queue_);
      if (!msg) {
        break;
      }
      (*service)(std::move(*msg)).thenValue([this](Message out) {
        auto buf = Codec::encode(out);
        if (buf) {
          sendData(std::move(buf));
        }
      });
    }
  }

private:
  folly::IOBufQueue queue_{folly::IOBufQueue::cacheChainLength()};
};

class WebSocketHandlerFactory final : public proxygen::RequestHandlerFactory {
public:
  void onServerStart(folly::EventBase*) noexcept override {}

  void onServerStop() noexcept override {}

  proxygen::RequestHandler* onRequest(
      proxygen::RequestHandler*, proxygen::HTTPMessage*
  ) noexcept override {
    return new WebSocketHandler();
  }
};

namespace {
std::shared_ptr<WebSocketHandlerFactory> factory;
}  // namespace

Server::Server(ServerOptions const& options) : options_(std::make_shared<ServerOptions>(options)) {
  if (0 == options_->threads) {
    options_->threads = std::max(4u, folly::hardware_concurrency());
  }
}

Server::~Server() {}

void Server::start() {
  service = std::make_shared<Service>();
  server = std::make_shared<wangle::ServerBootstrap<Pipeline>>();
  server->childPipeline(std::make_shared<PipelineFactory>(options_->threads));
  server->bind(options_->port);
  server->waitForStop();
  server.reset();
  service.reset();
}

void Server::stop() { server->stop(); }

std::shared_ptr<proxygen::RequestHandlerFactory> Server::getHandlerFactory() {
  if (!factory) {
    factory = std::make_shared<WebSocketHandlerFactory>();
  }
  return factory;
}
}  // namespace warp::mqtt
