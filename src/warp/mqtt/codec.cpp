#include "warp/mqtt/codec.h"

#include <folly/io/Cursor.h>

namespace warp::mqtt {
std::optional<Message> Codec::decode(folly::IOBufQueue& q) {
  if (q.empty()) return std::nullopt;

  folly::io::Cursor peek(q.front());
  size_t size = 0;
  auto opt = readFixedHeader(peek, size);
  if (!opt) return std::nullopt;

  const auto head = *opt;
  if (q.chainLength() < size + head.size) return std::nullopt;

  auto frame = q.split(size + head.size);
  folly::io::Cursor cur(frame.get());
  cur.skip(size);

  auto const type = static_cast<Type>((head.data >> 4) & 0x0F);
  switch (type) {
    case Type::Connect: {
      auto msg = Connect::decode(head, cur);
      if (!msg) return std::nullopt;
      return Message{std::move(*msg)};
    }
    case Type::ConnAck: {
      auto msg = ConnAck::decode(head, cur);
      if (!msg) return std::nullopt;
      return Message{*msg};
    }
    case Type::Publish: {
      auto msg = Publish::decode(head, cur);
      if (!msg) return std::nullopt;
      return Message{std::move(*msg)};
    }
    case Type::PubAck: {
      auto msg = PubAck::decode(head, cur);
      if (!msg) return std::nullopt;
      return Message{*msg};
    }
    case Type::Subscribe: {
      auto msg = Subscribe::decode(head, cur);
      if (!msg) return std::nullopt;
      return Message{std::move(*msg)};
    }
    case Type::SubAck: {
      auto msg = SubAck::decode(head, cur);
      if (!msg) return std::nullopt;
      return Message{std::move(*msg)};
    }
    case Type::Unsubscribe: {
      auto msg = Unsubscribe::decode(head, cur);
      if (!msg) return std::nullopt;
      return Message{std::move(*msg)};
    }
    case Type::UnsubAck: {
      auto msg = UnsubAck::decode(head, cur);
      if (!msg) return std::nullopt;
      return Message{*msg};
    }
    case Type::PingReq: {
      auto msg = PingReq::decode(head, cur);
      if (!msg) return std::nullopt;
      return Message{*msg};
    }
    case Type::PingResp: {
      auto msg = PingResp::decode(head, cur);
      if (!msg) return std::nullopt;
      return Message{*msg};
    }
    case Type::Disconnect: {
      auto msg = Disconnect::decode(head, cur);
      if (!msg) return std::nullopt;
      return Message{*msg};
    }
    default:
      return std::nullopt;
  }
}

std::unique_ptr<folly::IOBuf> Codec::encode(const Message& msg) {
  folly::IOBufQueue q(folly::IOBufQueue::cacheChainLength());
  folly::io::QueueAppender a(&q, 1024);
  std::visit([&](auto const& m) { m.encode(a); }, msg);
  return q.chainLength() ? q.move() : nullptr;
}
}  // namespace warp::mqtt
