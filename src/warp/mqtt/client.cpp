#include "warp/mqtt/client.h"

namespace warp::mqtt {
Client::Client(ClientOptions const& options) : options_(std::make_shared<ClientOptions>(options)) {
}

Client::~Client() {}
}  // namespace warp::mqtt
