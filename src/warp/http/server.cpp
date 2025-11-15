#include "warp/http/server.h"

#include <folly/system/HardwareConcurrency.h>
#include <proxygen/httpserver/filters/DirectResponseHandler.h>

namespace warp::http {
namespace {
class HandlerFactory final : public proxygen::RequestHandlerFactory {
public:
  explicit HandlerFactory(
      std::unordered_map<std::string, std::shared_ptr<proxygen::RequestHandlerFactory>>* handlers
  )
      : handlers_(handlers) {}

  void onServerStart(folly::EventBase*) noexcept override {}

  void onServerStop() noexcept override {}

  proxygen::RequestHandler* onRequest(
      proxygen::RequestHandler* h, proxygen::HTTPMessage* msg
  ) noexcept override {
    auto const& path = msg->getPath();
    for (auto const& [p, handler] : *handlers_) {
      if (folly::StringPiece(path).startsWith(p)) {
        return handler->onRequest(h, msg);
      }
    }
    return new proxygen::DirectResponseHandler(404, "Not Found", "{\"error\":\"Not Found\"}");
  }

private:
  std::unordered_map<std::string, std::shared_ptr<proxygen::RequestHandlerFactory>>* handlers_;
};
}  // namespace

Server::Server(ServerOptions const& options) : options_(std::make_shared<ServerOptions>(options)) {
  if (0 == options_->threads) {
    options_->threads = std::max(4u, folly::hardware_concurrency());
  }
}

Server::~Server() = default;

void Server::start() {
  if (!server_) {
    proxygen::HTTPServerOptions options;
    options.threads = options_->threads;
    options.idleTimeout = options_->timeout;
    options.handlerFactories =
        proxygen::RequestHandlerChain().addThen<HandlerFactory>(&handlers_).build();
    server_ = std::make_shared<proxygen::HTTPServer>(std::move(options));
    server_->bind(
        {{folly::SocketAddress("0.0.0.0", options_->port, true),
          proxygen::HTTPServer::Protocol::HTTP}}
    );
    server_->start();
  }
}

void Server::stop() {
  if (server_) {
    server_->stop();
    server_.reset();
  }
}

void Server::addHandler(
    std::string const& path, std::shared_ptr<proxygen::RequestHandlerFactory> handler
) {
  handlers_.emplace(std::move(path), std::move(handler));
}
}  // namespace warp::http
