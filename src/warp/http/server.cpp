#include "warp/http/server.h"

#include <folly/system/HardwareConcurrency.h>
#include <proxygen/httpserver/HTTPServer.h>
#include <proxygen/httpserver/RequestHandlerFactory.h>
#include <proxygen/httpserver/filters/DirectResponseHandler.h>

namespace warp::http {
namespace {
class HandlerFactory final : public proxygen::RequestHandlerFactory {
public:
  void onServerStart(folly::EventBase*) noexcept override {}

  void onServerStop() noexcept override {}

  proxygen::RequestHandler* onRequest(
      proxygen::RequestHandler*, proxygen::HTTPMessage* msg
  ) noexcept override {
    return new proxygen::DirectResponseHandler(404, "Not Found", "{\"error\":\"Not Found\"}");
  }
};

std::shared_ptr<proxygen::HTTPServer> server;
}  // namespace

Server::Server(ServerOptions const& options) : options_(std::make_shared<ServerOptions>(options)) {
  if (0 == options_->threads) {
    options_->threads = std::max(4u, folly::hardware_concurrency());
  }
}

Server::~Server() {}

void Server::start() {
  proxygen::HTTPServerOptions options;
  options.threads = options_->threads;
  options.idleTimeout = std::chrono::seconds(60);
  options.supportsConnect = true;
  options.handlerFactories = proxygen::RequestHandlerChain().addThen<HandlerFactory>().build();
  server = std::make_shared<proxygen::HTTPServer>(std::move(options));
  server->bind(
      {{folly::SocketAddress("0.0.0.0", options_->port, true),
        proxygen::HTTPServer::Protocol::HTTP}}
  );
  server->start();
}

void Server::stop() { server->stop(); }
}  // namespace warp::http
