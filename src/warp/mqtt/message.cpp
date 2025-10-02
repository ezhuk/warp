#include "warp/mqtt/message.h"

namespace warp::mqtt {
void Connect::encode(folly::io::QueueAppender& a) const {
  const auto name = protocolNameForLevel(head.level);
  const uint32_t size = 2u + static_cast<uint32_t>(name.size()) + 1u + 1u + 2u + 2u +
                        static_cast<uint32_t>(data.client.size());
  writeFixedHeader(a, Type::Connect, Flags(0), size);
  writeUTF8(a, name);
  a.write<uint8_t>(static_cast<uint8_t>(head.level));
  a.write<uint8_t>(head.flags);
  a.writeBE<uint16_t>(head.timeout);
  writeUTF8(a, data.client);
}

std::optional<Connect> Connect::decode(FixedHeader const& head, folly::io::Cursor& cur) {
  if (((head.data >> 4) & 0x0F) != static_cast<uint8_t>(Type::Connect)) return std::nullopt;
  uint32_t left = head.size;

  std::string name;
  if (!readUTF8(cur, left, name)) return std::nullopt;

  if (left < 1) return std::nullopt;
  auto level = static_cast<Level>(cur.read<uint8_t>());
  left -= 1;

  if (!isValidProtocolNameForLevel(name, level)) return std::nullopt;

  if (left < 1) return std::nullopt;
  uint8_t flags = cur.read<uint8_t>();
  left -= 1;

  if (left < 2) return std::nullopt;
  uint16_t timeout = cur.readBE<uint16_t>();
  left -= 2;

  std::string client;
  if (!readUTF8(cur, left, client)) return std::nullopt;

  Connect msg;
  msg.head.head = head;
  msg.head.level = level;
  msg.head.flags = flags;
  msg.head.timeout = timeout;
  msg.data.client = std::move(client);
  return msg;
}

void ConnAck::encode(folly::io::QueueAppender& a) const {
  writeFixedHeader(a, Type::ConnAck, Flags(0), 2u);
  a.write<uint8_t>(head.session ? 1 : 0);
  a.write<uint8_t>(head.reason);
}

std::optional<ConnAck> ConnAck::decode(FixedHeader const& head, folly::io::Cursor& cur) {
  if (((head.data >> 4) & 0x0F) != static_cast<uint8_t>(Type::ConnAck)) return std::nullopt;
  if (head.size != 2) return std::nullopt;
  ConnAck msg;
  msg.head.session = cur.read<uint8_t>();
  msg.head.reason = cur.read<uint8_t>();
  return msg;
}

void Publish::encode(folly::io::QueueAppender& a) const {
  uint32_t size = 2u + static_cast<uint32_t>(head.topic.size()) + (head.qos ? 2u : 0u) +
                  static_cast<uint32_t>(data.data.size());
  const uint8_t flags = static_cast<uint8_t>(
      (head.dup ? 0x08 : 0x00) | ((head.qos & 0x03) << 1) | (head.retain ? 0x01 : 0x00)
  );
  writeFixedHeader(a, Type::Publish, Flags(flags), size);
  writeUTF8(a, head.topic);
  if (head.qos) {
    a.writeBE<uint16_t>(head.packetId);
  }
  if (!data.data.empty()) {
    a.push(reinterpret_cast<const uint8_t*>(data.data.data()), data.data.size());
  }
}

std::optional<Publish> Publish::decode(FixedHeader const& head, folly::io::Cursor& cur) {
  if (((head.data >> 4) & 0x0F) != static_cast<uint8_t>(Type::Publish)) {
    return std::nullopt;
  }
  uint8_t const flags = static_cast<uint8_t>(head.data & 0x0F);
  uint8_t const dup = (flags & 0x08) ? 1 : 0;
  uint8_t const qos = static_cast<uint8_t>((flags >> 1) & 0x03);
  uint8_t const retain = (flags & 0x01) ? 1 : 0;
  uint32_t left = head.size;

  std::string topic;
  if (!readUTF8(cur, left, topic)) return std::nullopt;

  uint16_t packetId = 0;
  if (qos > 0) {
    if (left < 2) {
      return std::nullopt;
    }
    packetId = cur.readBE<uint16_t>();
    left -= 2;
  }

  std::string payload;
  if (left > 0) {
    payload = cur.readFixedString(left);
  }

  Publish msg;
  msg.head.head = head;
  msg.head.topic = std::move(topic);
  msg.head.packetId = packetId;
  msg.head.qos = qos;
  msg.head.dup = dup;
  msg.head.retain = retain;
  msg.data.data = std::move(payload);
  return msg;
}

void PubAck::encode(folly::io::QueueAppender& a) const {
  writeFixedHeader(a, Type::PubAck, Flags(0), 2u);
  a.writeBE<uint16_t>(head.packetId);
}

std::optional<PubAck> PubAck::decode(FixedHeader const& head, folly::io::Cursor& cur) {
  if (((head.data >> 4) & 0x0F) != static_cast<uint8_t>(Type::PubAck)) return std::nullopt;
  if (head.size != 2) return std::nullopt;
  PubAck msg;
  msg.head.packetId = cur.readBE<uint16_t>();
  return msg;
}

void PubRec::encode(folly::io::QueueAppender& a) const {
  writeFixedHeader(a, Type::PubRec, Flags(0), 2u);
  a.writeBE<uint16_t>(head.packetId);
}

std::optional<PubRec> PubRec::decode(FixedHeader const& head, folly::io::Cursor& cur) {
  if (((head.data >> 4) & 0x0F) != static_cast<uint8_t>(Type::PubRec)) return std::nullopt;
  if ((head.data & 0x0F) != 0x00) return std::nullopt;
  if (head.size != 2) return std::nullopt;
  PubRec m;
  m.head.packetId = cur.readBE<uint16_t>();
  return m;
}

void PubRel::encode(folly::io::QueueAppender& a) const {
  writeFixedHeader(a, Type::PubRel, Flags(2), 2u);
  a.writeBE<uint16_t>(head.packetId);
}

std::optional<PubRel> PubRel::decode(FixedHeader const& head, folly::io::Cursor& cur) {
  if (((head.data >> 4) & 0x0F) != static_cast<uint8_t>(Type::PubRel)) return std::nullopt;
  if ((head.data & 0x0F) != 0x02) return std::nullopt;
  if (head.size != 2) return std::nullopt;
  PubRel msg;
  msg.head.packetId = cur.readBE<uint16_t>();
  return msg;
}

void PubComp::encode(folly::io::QueueAppender& a) const {
  writeFixedHeader(a, Type::PubComp, Flags(0), 2u);
  a.writeBE<uint16_t>(head.packetId);
}

std::optional<PubComp> PubComp::decode(FixedHeader const& head, folly::io::Cursor& cur) {
  if (((head.data >> 4) & 0x0F) != static_cast<uint8_t>(Type::PubComp)) return std::nullopt;
  if ((head.data & 0x0F) != 0x00) return std::nullopt;
  if (head.size != 2) return std::nullopt;
  PubComp msg;
  msg.head.packetId = cur.readBE<uint16_t>();
  return msg;
}

void Subscribe::encode(folly::io::QueueAppender& a) const {
  uint32_t size = 2;
  for (auto const& t : data.topics) {
    size += 2u + static_cast<uint32_t>(t.filter.size()) + 1u;
  }
  writeFixedHeader(a, Type::Subscribe, Flags(2), size);
  a.writeBE<uint16_t>(head.packetId);
  for (auto const& topic : data.topics) {
    writeUTF8(a, topic.filter);
    a.write<uint8_t>(topic.qos & 0x03);
  }
}

std::optional<Subscribe> Subscribe::decode(FixedHeader const& head, folly::io::Cursor& cur) {
  if (((head.data >> 4) & 0x0F) != static_cast<uint8_t>(Type::Subscribe)) return std::nullopt;
  if ((head.data & 0x0F) != 0x02) return std::nullopt;
  if (head.size < 2) return std::nullopt;
  uint32_t left = head.size;
  Subscribe msg;
  msg.head.packetId = cur.readBE<uint16_t>();
  left -= 2;
  while (left > 0) {
    std::string filter;
    if (!readUTF8(cur, left, filter)) return std::nullopt;
    if (left < 1) return std::nullopt;
    const uint8_t qos = cur.read<uint8_t>();
    left -= 1;
    msg.data.topics.push_back(Topic{std::move(filter), static_cast<uint8_t>(qos & 0x03)});
  }
  return msg;
}

void SubAck::encode(folly::io::QueueAppender& a) const {
  writeFixedHeader(a, Type::SubAck, Flags(0), data.codes.size() + 2);
  a.writeBE<uint16_t>(head.packetId);
  for (auto code : data.codes) a.write<uint8_t>(code);
}

std::optional<SubAck> SubAck::decode(FixedHeader const& head, folly::io::Cursor& cur) {
  if (((head.data >> 4) & 0x0F) != static_cast<uint8_t>(Type::SubAck)) return std::nullopt;
  if ((head.data & 0x0F) != 0x00) return std::nullopt;
  if (head.size < 2) return std::nullopt;
  uint32_t left = head.size;
  SubAck msg;
  msg.head.packetId = cur.readBE<uint16_t>();
  left -= 2;
  msg.data.codes.clear();
  msg.data.codes.reserve(left);
  while (left > 0) {
    msg.data.codes.push_back(cur.read<uint8_t>());
    left -= 1;
  }
  return msg;
}

void Unsubscribe::encode(folly::io::QueueAppender& a) const {
  uint32_t size = 2u;
  for (auto const& topic : data.topics) {
    size += 2u + static_cast<uint32_t>(topic.size());
  }
  writeFixedHeader(a, Type::Unsubscribe, Flags(2), size);
  a.writeBE<uint16_t>(head.packetId);
  for (auto const& topic : data.topics) {
    writeUTF8(a, topic);
  }
}

std::optional<Unsubscribe> Unsubscribe::decode(FixedHeader const& head, folly::io::Cursor& cur) {
  if (((head.data >> 4) & 0x0F) != static_cast<uint8_t>(Type::Unsubscribe)) return std::nullopt;
  if ((head.data & 0x0F) != 0x02) return std::nullopt;
  if (head.size < 2) return std::nullopt;

  uint32_t left = head.size;
  Unsubscribe msg;
  msg.head.packetId = cur.readBE<uint16_t>();
  left -= 2;

  while (left > 0) {
    std::string filter;
    if (!readUTF8(cur, left, filter)) {
      return std::nullopt;
    }
    msg.data.topics.push_back(std::move(filter));
  }
  return msg;
}

void UnsubAck::encode(folly::io::QueueAppender& a) const {
  writeFixedHeader(a, Type::UnsubAck, Flags(0), 2u);
  a.writeBE<uint16_t>(head.packetId);
}

std::optional<UnsubAck> UnsubAck::decode(FixedHeader const& head, folly::io::Cursor& cur) {
  if (((head.data >> 4) & 0x0F) != static_cast<uint8_t>(Type::UnsubAck)) return std::nullopt;
  if ((head.data & 0x0F) != 0x00) return std::nullopt;
  if (head.size != 2) return std::nullopt;
  UnsubAck msg;
  msg.head.packetId = cur.readBE<uint16_t>();
  return msg;
}

void PingReq::encode(folly::io::QueueAppender& a) const {
  writeFixedHeader(a, Type::PingReq, Flags(0), 0u);
}

std::optional<PingReq> PingReq::decode(FixedHeader const& head, folly::io::Cursor&) {
  if (((head.data >> 4) & 0x0F) != static_cast<uint8_t>(Type::PingReq)) return std::nullopt;
  if ((head.data & 0x0F) != 0x00) return std::nullopt;
  if (head.size != 0) return std::nullopt;
  return PingReq{};
}

void PingResp::encode(folly::io::QueueAppender& a) const {
  writeFixedHeader(a, Type::PingResp, Flags(0), 0u);
}

std::optional<PingResp> PingResp::decode(FixedHeader const& head, folly::io::Cursor&) {
  if (((head.data >> 4) & 0x0F) != static_cast<uint8_t>(Type::PingResp)) return std::nullopt;
  if ((head.data & 0x0F) != 0x00) return std::nullopt;
  if (head.size != 0) return std::nullopt;
  return PingResp{};
}

void Disconnect::encode(folly::io::QueueAppender& a) const {
  writeFixedHeader(a, Type::Disconnect, Flags(0), 0u);
}

std::optional<Disconnect> Disconnect::decode(FixedHeader const& head, folly::io::Cursor&) {
  if (((head.data >> 4) & 0x0F) != static_cast<uint8_t>(Type::Disconnect)) return std::nullopt;
  if ((head.data & 0x0F) != 0x00) return std::nullopt;
  if (head.size != 0) return std::nullopt;
  return Disconnect{};
}
}  // namespace warp::mqtt
