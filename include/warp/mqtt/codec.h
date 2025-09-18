#pragma once

#include <folly/io/IOBufQueue.h>

namespace warp::mqtt {
enum class Type : uint8_t {
  None = 0,
  Connect = 1,
  ConnAck = 2,
  Publish = 3,
  PubAck = 4,
  Subscribe = 8,
  SubAck = 9,
  PingReq = 12,
  PingResp = 13,
  Disconnect = 14,
};

struct Message {
  Type type{};
  uint16_t id{0};
  uint8_t topics{0};
  std::string topic;
  std::string payload;
};

class Codec final {
public:
  static std::optional<Message> decode(folly::IOBufQueue& q);
  static std::unique_ptr<folly::IOBuf> encode(const Message& msg);
};
}  // namespace warp::mqtt
