#pragma once

#include <cstdint>
#include <string>

namespace warp::mqtt {
enum class Type : uint8_t {
  None = 0,
  Connect,
  ConnAck,
  Publish,
  PubAck,
  PubRec,
  PubRel,
  PubComp,
  Subscribe,
  SubAck,
  Unsubscribe,
  UnsubAck,
  PingReq,
  PingResp,
  Disconnect,
};

struct Message {
  Type type{};
  uint16_t id{0};
  uint8_t topics{0};
  std::string topic;
  std::string payload;
};
}  // namespace warp::mqtt
