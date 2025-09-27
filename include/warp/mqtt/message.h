#pragma once

#include <folly/Varint.h>
#include <folly/io/Cursor.h>

#include <cstdint>
#include <string>
#include <variant>

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

enum class QOS : uint8_t {
  QOS0 = 0,
  QOS1,
  QOS2,
};

enum class Level : uint8_t {
  V31 = 3,
  V311,
  V5,
};

struct FixedHeader {
  uint8_t data{0};
  uint32_t size{0};
};

enum class Flags : uint8_t {};

static inline folly::StringPiece protocolNameForLevel(Level level) {
  return (level == Level::V31) ? folly::StringPiece("MQIsdp") : folly::StringPiece("MQTT");
}

static inline bool isValidProtocolNameForLevel(std::string const& name, Level level) {
  if (level == Level::V31) return name == "MQIsdp";
  if (level == Level::V311) return name == "MQTT";
  if (level == Level::V5) return name == "MQTT";
  return false;
}

static inline void writeUTF8(folly::io::QueueAppender& a, folly::StringPiece str) {
  if (str.size() > 0xFFFF) throw std::invalid_argument("UTF8 too long");
  a.writeBE<uint16_t>(static_cast<uint16_t>(str.size()));
  if (!str.empty()) a.push(reinterpret_cast<const uint8_t*>(str.data()), str.size());
}

static inline bool readUTF8(folly::io::Cursor& cur, uint32_t& left, std::string& out) {
  if (left < 2) return false;
  uint16_t n = cur.readBE<uint16_t>();
  left -= 2;
  if (left < n) return false;
  out = n ? cur.readFixedString(n) : std::string{};
  left -= n;
  return true;
}

static inline void writeFixedHeader(
    folly::io::QueueAppender& a, Type type, Flags flags, uint32_t size
) {
  const uint8_t first = static_cast<uint8_t>(
      (static_cast<uint8_t>(type) << 4) | (static_cast<uint8_t>(flags) & 0x0F)
  );
  a.write<uint8_t>(first);
  uint8_t tmp[5];
  size_t const n = folly::encodeVarint(static_cast<uint64_t>(size), tmp);
  a.push(tmp, n);
}

static inline std::optional<FixedHeader> readFixedHeader(folly::io::Cursor& cur, size_t& size) {
  if (!cur.canAdvance(1)) return std::nullopt;
  const uint8_t first = cur.read<uint8_t>();
  uint32_t value = 0;
  uint32_t multiplier = 1;
  size_t bytes = 0;

  for (int i = 0; i < 4; ++i) {
    if (!cur.canAdvance(1)) return std::nullopt;
    const uint8_t encoded = cur.read<uint8_t>();
    ++bytes;
    value += static_cast<uint32_t>(encoded & 0x7F) * multiplier;
    if ((encoded & 0x80) == 0) break;
    multiplier *= 128;
    if (i == 3 && (encoded & 0x80)) return std::nullopt;
  }

  size = 1 + bytes;
  return FixedHeader{first, value};
}

struct Connect {
  struct Header {
    FixedHeader head{};
    Level level{Level::V311};
    uint8_t flags{0};
    uint16_t timeout{0};
  };

  struct Payload {
    std::string client;
  };

  struct Builder final {
    Level level_{Level::V311};
    uint8_t flags_{0};
    uint16_t timeout_{0};
    std::string client_{};

    Builder& withLevel(Level const& level) {
      level_ = level;
      return *this;
    }

    Builder& withCleanSession(bool on = true) {
      if (on) {
        flags_ |= 0x02;
      } else {
        flags_ &= ~static_cast<uint8_t>(0x02);
      }
      return *this;
    }

    Builder& withKeepAlive(uint16_t timeout) {
      timeout_ = timeout;
      return *this;
    }

    Builder& withClient(std::string client) {
      client_ = std::move(client);
      return *this;
    }

    Connect buid() const {
      return Connect{
          .head{
              .head{.data = static_cast<uint8_t>(Type::Connect) << 4, .size = 0},
              .level = level_,
              .flags = flags_,
              .timeout = timeout_
          },
          .data{.client = client_}
      };
    }
  };

  Header head{};
  Payload data{};

  void encode(folly::io::QueueAppender& a) const {
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

  static std::optional<Connect> decode(FixedHeader const& head, folly::io::Cursor& cur) {
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
};

struct ConnAck {
  struct Header {
    uint8_t session{0};
    uint8_t reason{0};
  };

  struct Builder final {
    uint8_t session_{0};
    uint8_t reason_{0};

    Builder& withSession(uint8_t const session) {
      session_ = session;
      return *this;
    }

    Builder& withReason(uint8_t const reason) {
      reason_ = reason;
      return *this;
    }

    ConnAck build() const { return ConnAck{.head{.session = session_, .reason = reason_}}; }
  };

  Header head{};

  void encode(folly::io::QueueAppender& a) const {
    writeFixedHeader(a, Type::ConnAck, Flags(0), 2u);
    a.write<uint8_t>(head.session ? 1 : 0);
    a.write<uint8_t>(head.reason);
  }

  static std::optional<ConnAck> decode(FixedHeader const& head, folly::io::Cursor& cur) {
    if (((head.data >> 4) & 0x0F) != static_cast<uint8_t>(Type::ConnAck)) return std::nullopt;
    if (head.size != 2) return std::nullopt;
    ConnAck msg;
    msg.head.session = cur.read<uint8_t>();
    msg.head.reason = cur.read<uint8_t>();
    return msg;
  }
};

struct Publish {
  struct Header {
    FixedHeader head{};
    std::string topic;
    uint16_t packetId{0};
    uint8_t qos{0};
    uint8_t dup{0};
    uint8_t retain{0};
  };

  struct Payload {
    std::string data;
  };

  struct Builder final {
    std::string topic_{};
    std::string data_{};
    uint16_t packetId_{0};
    uint8_t qos_{0};
    uint8_t dup_{0};
    uint8_t retain_{0};

    Builder& withTopic(std::string const& topic) {
      topic_ = topic;
      return *this;
    }

    Builder& withPayload(std::string const& data) {
      data_ = data;
      return *this;
    }

    Builder& withQos(uint8_t qos) {
      qos_ = static_cast<uint8_t>(qos & 0x03);
      return *this;
    }

    Builder& withPacketId(uint16_t id) {
      packetId_ = id;
      return *this;
    }

    Builder& withDup(bool on = true) {
      dup_ = on ? 1 : 0;
      return *this;
    }

    Builder& withRetain(bool on = true) {
      retain_ = on ? 1 : 0;
      return *this;
    }

    Publish build() const {
      Publish msg;
      msg.head.topic = topic_;
      msg.head.packetId = packetId_;
      msg.head.qos = qos_;
      msg.head.dup = dup_;
      msg.head.retain = retain_;
      msg.data.data = data_;
      return msg;
    }
  };

  Header head{};
  Payload data{};

  void encode(folly::io::QueueAppender& a) const {
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

  static std::optional<Publish> decode(FixedHeader const& head, folly::io::Cursor& cur) {
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
};

struct PubAck {
  struct Header {
    uint16_t packetId{0};
  };

  struct Builder final {
    uint16_t packetId_{0};

    Builder& withPacketId(uint16_t id) {
      packetId_ = id;
      return *this;
    }
    PubAck build() const {
      PubAck msg;
      msg.head.packetId = packetId_;
      return msg;
    }
  };

  Header head{};

  void encode(folly::io::QueueAppender& a) const {
    writeFixedHeader(a, Type::PubAck, Flags(0), 2u);
    a.writeBE<uint16_t>(head.packetId);
  }

  static std::optional<PubAck> decode(FixedHeader const& head, folly::io::Cursor& cur) {
    if (((head.data >> 4) & 0x0F) != static_cast<uint8_t>(Type::PubAck)) return std::nullopt;
    if (head.size != 2) return std::nullopt;
    PubAck msg;
    msg.head.packetId = cur.readBE<uint16_t>();
    return msg;
  }
};

struct Subscribe {
  struct Topic {
    std::string filter;
    uint8_t qos{0};
  };

  struct Header {
    uint16_t packetId{0};
  };

  struct Payload {
    std::vector<Topic> topics;
  };

  Header head{};
  Payload data{};

  struct Builder final {
    uint16_t packetId_{0};
    std::vector<Topic> topics_;

    Builder& withPacketId(uint16_t id) {
      packetId_ = id;
      return *this;
    }
    Builder& addTopic(std::string filter, uint8_t qos) {
      topics_.push_back(Topic{std::move(filter), static_cast<uint8_t>(qos & 0x03)});
      return *this;
    }

    Subscribe build() const {
      Subscribe msg;
      msg.head.packetId = packetId_;
      msg.data.topics = topics_;
      return msg;
    }
  };

  void encode(folly::io::QueueAppender& a) const {
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

  static std::optional<Subscribe> decode(FixedHeader const& head, folly::io::Cursor& cur) {
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
};

struct SubAck {
  struct Header {
    uint16_t packetId{0};
  };

  struct Payload {
    std::vector<uint8_t> codes;
  };

  Header head{};
  Payload data{};

  struct Builder final {
    uint16_t packetId_{0};
    std::vector<uint8_t> codes_;

    Builder& withPacketId(uint16_t id) {
      packetId_ = id;
      return *this;
    }

    Builder& addCode(uint8_t code) {
      codes_.push_back(code);
      return *this;
    }

    Builder& withCodesFrom(Subscribe const& msg, uint8_t err = 0x80) {
      codes_.clear();
      codes_.reserve(msg.data.topics.size());
      for (auto const& topic : msg.data.topics) {
        const uint8_t qos = static_cast<uint8_t>(topic.qos & 0x03);
        const uint8_t code = (qos <= 2) ? qos : err;
        codes_.push_back(code);
      }
      return *this;
    }

    SubAck build() const {
      SubAck msg;
      msg.head.packetId = packetId_;
      msg.data.codes = codes_;
      return msg;
    }
  };

  void encode(folly::io::QueueAppender& a) const {
    writeFixedHeader(a, Type::SubAck, Flags(0), data.codes.size() + 2);
    a.writeBE<uint16_t>(head.packetId);
    for (auto code : data.codes) a.write<uint8_t>(code);
  }

  static std::optional<SubAck> decode(FixedHeader const& head, folly::io::Cursor& cur) {
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
};

struct PingReq {
  struct Builder final {
    PingReq build() const { return PingReq{}; }
  };

  void encode(folly::io::QueueAppender& a) const {
    writeFixedHeader(a, Type::PingReq, Flags(0), 0u);
  }

  static std::optional<PingReq> decode(FixedHeader const& head, folly::io::Cursor&) {
    if (((head.data >> 4) & 0x0F) != static_cast<uint8_t>(Type::PingReq)) return std::nullopt;
    if ((head.data & 0x0F) != 0x00) return std::nullopt;
    if (head.size != 0) return std::nullopt;
    return PingReq{};
  }
};

struct PingResp {
  struct Builder final {
    PingResp build() const { return PingResp{}; }
  };

  void encode(folly::io::QueueAppender& a) const {
    writeFixedHeader(a, Type::PingResp, Flags(0), 0u);
  }

  static std::optional<PingResp> decode(FixedHeader const& head, folly::io::Cursor&) {
    if (((head.data >> 4) & 0x0F) != static_cast<uint8_t>(Type::PingResp)) return std::nullopt;
    if ((head.data & 0x0F) != 0x00) return std::nullopt;
    if (head.size != 0) return std::nullopt;
    return PingResp{};
  }
};

struct None {
  void encode(folly::io::QueueAppender&) const {}
};

using Message =
    std::variant<Connect, ConnAck, Publish, PubAck, Subscribe, SubAck, PingReq, PingResp, None>;
}  // namespace warp::mqtt
