#pragma once

#include <memory>
#include <string>

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

private:
  std::shared_ptr<ClientOptions> options_;
};
}  // namespace warp::mqtt
