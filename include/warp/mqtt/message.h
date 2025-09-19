#pragma once

#include <cstdint>
#include <string>

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
}  // namespace warp::mqtt
