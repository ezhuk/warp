#pragma once

#include <folly/io/IOBufQueue.h>

#include "warp/mqtt/message.h"

namespace warp::mqtt {
class Codec final {
public:
  static std::optional<Message> decode(folly::IOBufQueue& q);
  static std::unique_ptr<folly::IOBuf> encode(Message const& msg);
};
}  // namespace warp::mqtt
