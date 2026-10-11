#include "warp/mqtt/client.h"

#include <folly/executors/IOThreadPoolExecutor.h>
#include <wangle/bootstrap/ClientBootstrap.h>
#include <wangle/channel/AsyncSocketHandler.h>
#include <wangle/channel/EventBaseHandler.h>
#include <wangle/channel/Pipeline.h>
#include <wangle/service/ClientDispatcher.h>

namespace warp::mqtt {
namespace {
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

using Pipeline = wangle::Pipeline<folly::IOBufQueue&, Message>;

class PipelineFactory final : public wangle::PipelineFactory<Pipeline> {
public:
  Pipeline::Ptr newPipeline(std::shared_ptr<folly::AsyncTransport> sock) override {
    auto pipeline = Pipeline::create();
    pipeline->addBack(wangle::AsyncSocketHandler(sock));
    pipeline->addBack(wangle::EventBaseHandler());
    pipeline->addBack(Handler());
    pipeline->finalize();
    return pipeline;
  }
};

class Service final : public wangle::Service<Message, Message> {
public:
  explicit Service(Pipeline* pipeline) { dispatcher_.setPipeline(pipeline); }

  folly::Future<Message> operator()(Message msg) override { return dispatcher_(std::move(msg)); }

private:
  wangle::SerialClientDispatcher<Pipeline, Message, Message> dispatcher_;
};

std::shared_ptr<wangle::ClientBootstrap<Pipeline>> client;
std::shared_ptr<Pipeline> pipeline;
std::shared_ptr<Service> service;
}  // namespace

Client::Client(ClientOptions const& options) : options_(std::make_shared<ClientOptions>(options)) {}

Client::~Client() { close(); }

void Client::connect() {
  client = std::make_shared<wangle::ClientBootstrap<Pipeline>>();
  client->group(std::make_shared<folly::IOThreadPoolExecutor>(1));
  client->pipelineFactory(std::make_shared<PipelineFactory>());
  pipeline.reset(client->connect(folly::SocketAddress(options_->host, options_->port)).get());
  service = std::make_shared<Service>(client->getPipeline());
}

void Client::close() {
  if (client) {
    service.reset();
    pipeline->close().get();
    pipeline.reset();
    client.reset();
  }
}

folly::Future<Message> Client::request(Message msg) {
  if (client) {
    return (*service)(std::move(msg));
  }
  return folly::makeFuture<Message>(Message{None{}});
}
}  // namespace warp::mqtt
