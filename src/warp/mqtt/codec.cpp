#include "warp/mqtt/codec.h"

#include <folly/Varint.h>
#include <folly/io/Cursor.h>

namespace warp::mqtt {
std::optional<Message> Codec::decode(folly::IOBufQueue& q) {
  if (q.chainLength() < 2) {
    return std::nullopt;
  }

  folly::io::Cursor p(q.front());
  if (!p.canAdvance(1)) {
    return std::nullopt;
  }

  [[maybe_unused]] const uint8_t header_peek = p.read<uint8_t>();

  uint32_t remaining = 0;
  uint32_t multiplier = 1;
  size_t length = 0;
  {
    folly::io::Cursor v(q.front());
    v.skip(1);
    for (; length < 4; ++length) {
      if (!v.canAdvance(1)) {
        return std::nullopt;
      }
      const uint8_t b = v.read<uint8_t>();
      remaining += static_cast<uint32_t>(b & 0x7F) * multiplier;
      if ((b & 0x80) == 0) {
        break;
      }
      multiplier *= 128u;
    }
    if (length == 4 && multiplier >= 128u) {
      return std::nullopt;
    }
    ++length;
    if (remaining > 0x0FFFFFFF) {
      return std::nullopt;
    }
  }

  const size_t headerSize = 1 + length;
  const size_t totalFrame = headerSize + static_cast<size_t>(remaining);
  if (q.chainLength() < totalFrame) {
    return std::nullopt;
  }

  auto frame = q.split(totalFrame);
  folly::io::Cursor c(frame.get());
  const uint8_t hdr = c.read<uint8_t>();
  const uint8_t typeNibble = hdr >> 4;

  c.skip(length);
  uint32_t left = remaining;

  Message msg{};
  switch (typeNibble) {
    case 0x01: {
      msg.type = Type::Connect;
      return msg;
    }
    case 0x0E: {
      msg.type = Type::Disconnect;
      return msg;
    }
    case 0x0C: {
      msg.type = Type::PingReq;
      return msg;
    }
    case 0x03: {
      if (left < 2) {
        return std::nullopt;
      }
      const uint16_t tlen = c.readBE<uint16_t>();
      left -= 2;
      if (left < static_cast<uint32_t>(tlen)) {
        return std::nullopt;
      }
      std::string topic = c.readFixedString(tlen);
      left -= static_cast<uint32_t>(tlen);
      std::string payload;
      if (left > 0) {
        payload = c.readFixedString(left);
      }
      msg.type = Type::Publish;
      msg.topic = std::move(topic);
      msg.payload = std::move(payload);
      return msg;
    }
    case 0x08: {
      if (left < 2) {
        return std::nullopt;
      }
      const uint16_t pid = c.readBE<uint16_t>();
      left -= 2;
      uint8_t topics = 0;
      while (left >= 3) {
        const uint16_t tlen = c.readBE<uint16_t>();
        left -= 2;
        const uint32_t need = static_cast<uint32_t>(tlen) + 1u;
        if (left < need) {
          break;
        }
        c.skip(tlen);
        left -= static_cast<uint32_t>(tlen);
        c.skip(1);
        left -= 1;
        ++topics;
      }
      msg.type = Type::Subscribe;
      msg.id = pid;
      msg.topics = topics;
      return msg;
    }
    default:
      return std::nullopt;
  }
  return std::nullopt;
}

std::unique_ptr<folly::IOBuf> Codec::encode(const Message& msg) {
  folly::IOBufQueue q(folly::IOBufQueue::cacheChainLength());
  folly::io::QueueAppender a(&q, 1024);
  auto writeRemainingLength = [&](uint32_t value) -> void {
    uint8_t tmp[5];
    size_t const len = folly::encodeVarint(static_cast<uint64_t>(value), tmp);
    a.push(tmp, len);
  };
  switch (msg.type) {
    case Type::ConnAck: {
      a.write<uint8_t>(0x20);
      writeRemainingLength(2u);
      a.write<uint8_t>(0x00);
      a.write<uint8_t>(0x00);
      break;
    }
    case Type::SubAck: {
      a.write<uint8_t>(0x90);
      writeRemainingLength(2u + msg.topics);
      a.writeBE<uint16_t>(msg.id);
      for (uint8_t i = 0; i < msg.topics; ++i) {
        a.write<uint8_t>(0x00);
      }
      break;
    }
    case Type::PingResp: {
      a.write<uint8_t>(0xD0);
      writeRemainingLength(0u);
      break;
    }
    default:
      break;
  }
  return (0 < q.chainLength()) ? q.move() : nullptr;
}
}  // namespace warp::mqtt
