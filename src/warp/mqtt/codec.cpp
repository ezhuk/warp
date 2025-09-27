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

  auto decodeAs = [&](auto tag) -> std::optional<Message> {
    using T = decltype(tag);
    auto msg = T::decode(head, cur);
    if (!msg) {
      return std::nullopt;
    }
    if constexpr (std::is_trivially_copy_constructible_v<T>) {
      return Message{*msg};
    } else {
      return Message{std::move(*msg)};
    }
  };

  auto const type = static_cast<Type>((head.data >> 4) & 0x0F);
  switch (type) {
    case Type::Connect:
      return decodeAs(Connect{});
    case Type::ConnAck:
      return decodeAs(ConnAck{});
    case Type::Publish:
      return decodeAs(Publish{});
    case Type::PubAck:
      return decodeAs(PubAck{});
    case Type::PubRec:
      return decodeAs(PubRec{});
    case Type::PubRel:
      return decodeAs(PubRel{});
    case Type::PubComp:
      return decodeAs(PubComp{});
    case Type::Subscribe:
      return decodeAs(Subscribe{});
    case Type::SubAck:
      return decodeAs(SubAck{});
    case Type::Unsubscribe:
      return decodeAs(Unsubscribe{});
    case Type::UnsubAck:
      return decodeAs(UnsubAck{});
    case Type::PingReq:
      return decodeAs(PingReq{});
    case Type::PingResp:
      return decodeAs(PingResp{});
    case Type::Disconnect:
      return decodeAs(Disconnect{});
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
