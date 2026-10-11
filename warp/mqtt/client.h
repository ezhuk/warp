#pragma once

#include <folly/futures/Future.h>

#include <memory>
#include <string>

#include "warp/mqtt/codec.h"

namespace warp::mqtt {
class ClientOptions {
public:
  std::string host{"127.0.0.1"};
  uint16_t port{1883};
};

class Client final {
public:
  explicit Client(ClientOptions const& options);
  virtual ~Client();

  void connect();
  void close();

  folly::Future<Message> request(Message msg);

private:
  std::shared_ptr<ClientOptions> options_;
};
}  // namespace warp::mqtt
