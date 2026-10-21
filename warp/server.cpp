// server.cpp

#include "server.h"

#include <folly/Unit.h>
#include <folly/futures/Future.h>
#include <folly/io/IOBuf.h>
#include <folly/io/IOBufQueue.h>
#include <wangle/bootstrap/ServerBootstrap.h>
#include <wangle/channel/AsyncSocketHandler.h>
#include <wangle/channel/EventBaseHandler.h>
#include <wangle/channel/Handler.h>
#include <wangle/channel/Pipeline.h>
#include <wangle/service/ServerDispatcher.h>
#include <wangle/service/Service.h>

#include <array>
#include <memory>
#include <optional>
#include <variant>

#include "scoped_event_base.h"

namespace warp {
namespace {
struct Connect {};
struct ConnAck {};
struct Disconnect {};
struct PingReq {};
struct PingResp {};

using Packet = std::variant<Connect, ConnAck, Disconnect, PingReq, PingResp>;

using Pipeline = wangle::Pipeline<folly::IOBufQueue&, std::optional<Packet>>;

class PacketHandler final
    : public wangle::Handler<
          folly::IOBufQueue&, Packet, std::optional<Packet>, std::unique_ptr<folly::IOBuf>> {
public:
  PacketHandler() {}

  void read(Context* ctx, folly::IOBufQueue& data) override {
    // TODO
  }

  folly::Future<folly::Unit> write(Context* ctx, std::optional<Packet> pkt) override {
    return folly::makeFuture();
  }
};

class Service final : public wangle::Service<Packet, std::optional<Packet>> {
public:
  Service() {}

  folly::Future<std::optional<Packet>> operator()(Packet pkt) override {
    std::optional<Packet> res;
    return folly::makeFuture<std::optional<Packet>>(std::move(res));
  }
};

class PipelineFactory final : public wangle::PipelineFactory<Pipeline> {
public:
  PipelineFactory() {}

  Pipeline::Ptr newPipeline(std::shared_ptr<folly::AsyncTransport> sock) override {
    auto pipeline = Pipeline::create();
    pipeline->addBack(wangle::AsyncSocketHandler(sock));
    pipeline->addBack(wangle::EventBaseHandler());
    pipeline->addBack(PacketHandler());
    pipeline->addBack(wangle::MultiplexServerDispatcher<Packet, std::optional<Packet>>(&service_));
    pipeline->finalize();
    return pipeline;
  }

private:
  Service service_;
};

std::unique_ptr<wangle::ServerBootstrap<Pipeline>> server;
}  // namespace

Server::Server(ServerOptions options) : options_(std::move(options)) {
  // empty
}

Server::~Server() {
  stop();
  join();
}

void Server::start() {
  server = std::make_unique<wangle::ServerBootstrap<Pipeline>>();
  server->childPipeline(std::make_shared<PipelineFactory>());
  server->bind(1883);
}

void Server::stop() { server->stop(); }

void Server::join() { server->join(); }

void Server::run() {
  warp::ScopedEventBase base([this](int) { stop(); }, options_.signals);
  start();
  base.run();
  join();
}
}  // namespace warp
