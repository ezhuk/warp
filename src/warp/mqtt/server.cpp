#include <fmt/format.h>
#include <folly/io/IOBuf.h>
#include <folly/io/IOBufQueue.h>
#include <wangle/bootstrap/ServerBootstrap.h>
#include <wangle/channel/AsyncSocketHandler.h>
#include <wangle/channel/Pipeline.h>
#include <warp/mqtt/server.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace warp::mqtt {
namespace {
struct VarintResult {
  uint32_t value{0};
  uint8_t bytes{0};
  bool ok{false};
};

inline VarintResult readVarint(folly::io::Cursor& cur) {
  VarintResult r{};
  uint32_t mul = 1;
  for (;;) {
    if (!cur.canAdvance(1)) return r;
    uint8_t b = cur.read<uint8_t>();
    r.value += (b & 0x7F) * mul;
    mul *= 128;
    ++r.bytes;
    if ((b & 0x80) == 0 || r.bytes == 4) break;
  }
  r.ok = true;
  return r;
}

inline void writeVarint(uint32_t v, std::vector<uint8_t>& out) {
  do {
    uint8_t b = v % 128;
    v /= 128;
    if (v) b |= 0x80;
    out.push_back(b);
  } while (v);
}
}  // namespace

using Pipeline = wangle::Pipeline<folly::IOBufQueue&, std::unique_ptr<folly::IOBuf>>;

enum class Proto : uint8_t { V311 = 4, V5 = 5 };

enum class Type : uint8_t {
  CONNECT,
  SUBSCRIBE,
  PUBLISH,
  PINGREQ,
  DISCONNECT,
  CONNACK,
  SUBACK,
  PUBACK,
  PINGRESP
};

struct Packet {
  Type type{};

  uint8_t protoLevel{4};

  struct {
    uint16_t pktId{0};
    std::vector<std::string> topics;
    std::vector<uint8_t> reqQoS;
  } subscribe;

  struct {
    std::string topic;
    std::vector<uint8_t> payload;
    uint8_t qos{0};
    uint16_t pktId{0};
  } publish;

  struct {
  } connack;

  struct {
    uint16_t pktId{0};
    std::vector<uint8_t> grants;
  } suback;

  struct {
    uint16_t pktId{0};
  } puback;

  struct {
  } pingresp;
};

class Codec final
    : public wangle::Handler<folly::IOBufQueue&, Packet, Packet, std::unique_ptr<folly::IOBuf>> {
 public:
  using Context = typename wangle::Handler<folly::IOBufQueue&, Packet, Packet,
                                           std::unique_ptr<folly::IOBuf>>::Context;

  void read(Context* ctx, folly::IOBufQueue& q) override {
    if (auto buf = q.move()) in_.append(std::move(buf));
    for (;;) {
      Packet pkt;
      if (!tryReadOne(in_, pkt)) break;
      ctx->fireRead(std::move(pkt));
    }
  }

  void readEOF(Context* ctx) override { ctx->fireClose(); }
  void readException(Context* ctx, folly::exception_wrapper e) override {
    fmt::print("ERROR(dec): {}\n", e.what());
    ctx->fireClose();
  }

  folly::Future<folly::Unit> write(Context* ctx, Packet pkt) override {
    auto buf = encode(pkt);
    if (buf) {
      auto v = buf->clone();
      v->coalesce();
      fmt::print("WRITE {} bytes\n", v->length());
    }
    return ctx->fireWrite(std::move(buf));
  }

  folly::Future<folly::Unit> writeException(Context* ctx, folly::exception_wrapper e) override {
    fmt::print("ERROR(enc): {}\n", e.what());
    return ctx->fireWrite(nullptr);
  }

 private:
  bool tryReadOne(folly::IOBufQueue& in, Packet& out) {
    if (in.chainLength() < 2) return false;

    folly::io::Cursor c(in.front());
    uint8_t h1 = c.read<uint8_t>();
    uint8_t type = h1 >> 4;
    uint8_t flags = h1 & 0x0F;

    auto rlRes = readVarint(c);
    if (!rlRes.ok) return false;
    uint32_t rl = rlRes.value;
    size_t rlBytes = rlRes.bytes;

    const size_t headerLen = 1 + rlBytes;
    const size_t totalLen = headerLen + rl;
    if (in.chainLength() < totalLen) return false;

    if (auto* head = in.front()) {
      auto v = head->clone();
      v->coalesce();
      fmt::print("READ: {} bytes (type={})\n", totalLen, static_cast<int>(type));
      dumpHex(v->data(), std::min(v->length(), totalLen));
    }

    folly::io::Cursor d(in.front());
    d.skip(headerLen);

    switch (type) {
      case 1: {  // CONNECT
        if (!d.canAdvance(2)) break;
        uint16_t nameLen = d.readBE<uint16_t>();
        if (!d.canAdvance(nameLen + 1)) break;
        d.skip(nameLen);
        proto_level_ = d.read<uint8_t>();
        out = Packet{Type::CONNECT};
        out.protoLevel = proto_level_;
        in.trimStart(totalLen);
        return true;
      }
      case 8: {  // SUBSCRIBE
        Packet p{Type::SUBSCRIBE};
        p.protoLevel = proto_level_;
        if (!d.canAdvance(2)) break;
        p.subscribe.pktId = d.readBE<uint16_t>();
        size_t consumed = 2;

        if (proto_level_ == 5) {
          auto prop = readVarint(d);
          if (!prop.ok) break;
          if (!d.canAdvance(prop.value)) break;
          d.skip(prop.value);
          consumed += prop.bytes + prop.value;
        }

        while (consumed < rl) {
          if (!d.canAdvance(2)) break;
          uint16_t tlen = d.readBE<uint16_t>();
          if (!d.canAdvance(tlen + 1)) break;
          std::string topic(tlen, '\0');
          d.pull(reinterpret_cast<uint8_t*>(&topic[0]), tlen);
          uint8_t opts = d.read<uint8_t>();
          p.subscribe.topics.push_back(std::move(topic));
          p.subscribe.reqQoS.push_back(opts & 0x03);
          consumed += 2 + tlen + 1;
        }

        in.trimStart(totalLen);
        out = std::move(p);
        return true;
      }
      case 3: {  // PUBLISH
        Packet p{Type::PUBLISH};
        p.protoLevel = proto_level_;
        uint8_t qos = (flags >> 1) & 0x03;

        if (!d.canAdvance(2)) break;
        uint16_t tlen = d.readBE<uint16_t>();
        if (!d.canAdvance(tlen)) break;
        p.publish.topic.resize(tlen);
        d.pull(reinterpret_cast<uint8_t*>(&p.publish.topic[0]), tlen);
        size_t consumed = 2 + tlen;

        if (qos == 1) {
          if (!d.canAdvance(2)) break;
          p.publish.pktId = d.readBE<uint16_t>();
          consumed += 2;
        } else if (qos == 2) {
          // drop QoS2 in minimal impl
          in.trimStart(totalLen);
          out = Packet{Type::PUBLISH};
          out.publish.qos = 2;
          return true;
        }

        if (proto_level_ == 5) {
          auto prop = readVarint(d);
          if (!prop.ok) break;
          if (!d.canAdvance(prop.value)) break;
          d.skip(prop.value);
          consumed += prop.bytes + prop.value;
        }

        p.publish.qos = qos;
        size_t payLen = rl > consumed ? rl - consumed : 0;
        p.publish.payload.resize(payLen);
        if (payLen) d.pull(p.publish.payload.data(), payLen);

        in.trimStart(totalLen);
        out = std::move(p);
        return true;
      }
      case 12: {
        in.trimStart(totalLen);
        out = Packet{Type::PINGREQ};
        return true;
      }
      case 14: {
        in.trimStart(totalLen);
        out = Packet{Type::DISCONNECT};
        return true;
      }
      default: {
        in.trimStart(totalLen);
        return false;
      }
    }

    in.trimStart(totalLen);
    return false;
  }

  static std::unique_ptr<folly::IOBuf> encode(Packet const& p) {
    switch (p.type) {
      case Type::CONNACK:
        return encConnack(p.protoLevel);
      case Type::SUBACK:
        return encSuback(static_cast<Proto>(p.protoLevel), p.suback.pktId, p.suback.grants);
      case Type::PUBACK:
        return encPuback(p.puback.pktId);
      case Type::PINGRESP:
        return encPingresp();
      default:
        return nullptr;
    }
  }

  static std::unique_ptr<folly::IOBuf> encConnack(uint8_t proto) {
    if (proto == 5) {
      static const uint8_t v5[] = {0x20, 0x03, 0x00, 0x00, 0x00};
      return folly::IOBuf::copyBuffer(v5, sizeof(v5));
    } else {
      static const uint8_t v4[] = {0x20, 0x02, 0x00, 0x00};
      return folly::IOBuf::copyBuffer(v4, sizeof(v4));
    }
  }

  static std::unique_ptr<folly::IOBuf> encSuback(Proto proto, uint16_t pid,
                                                 const std::vector<uint8_t>& grants) {
    std::vector<uint8_t> out;
    out.push_back(0x90);
    const bool is_v5 = (proto == Proto::V5);
    uint32_t rl = is_v5 ? static_cast<uint32_t>(2 + 1 + grants.size())
                        : static_cast<uint32_t>(2 + grants.size());
    writeVarint(rl, out);
    out.push_back(static_cast<uint8_t>(pid >> 8));
    out.push_back(static_cast<uint8_t>(pid & 0xFF));
    if (is_v5) out.push_back(0x00);
    out.insert(out.end(), grants.begin(), grants.end());
    return folly::IOBuf::copyBuffer(out.data(), out.size());
  }

  static std::unique_ptr<folly::IOBuf> encPuback(uint16_t pid) {
    uint8_t b[4] = {0x40, 0x02, static_cast<uint8_t>(pid >> 8), static_cast<uint8_t>(pid & 0xFF)};
    return folly::IOBuf::copyBuffer(b, sizeof(b));
  }

  static std::unique_ptr<folly::IOBuf> encPingresp() {
    static const uint8_t b[] = {0xD0, 0x00};
    return folly::IOBuf::copyBuffer(b, sizeof(b));
  }

  static void dumpHex(const uint8_t* data, size_t len) {
    constexpr size_t kPreview = 128;
    size_t show = std::min(len, kPreview);
    for (size_t i = 0; i < show; i += 16) {
      fmt::print("{:08x}: ", i);
      for (size_t j = 0; j < 16; ++j) {
        if (i + j < show) {
          fmt::print("{:02x} ", data[i + j]);
        } else {
          fmt::print("   ");
        }
      }
      fmt::print(" |");
      for (size_t j = 0; j < 16 && i + j < show; ++j) {
        unsigned char c = data[i + j];
        fmt::print("{}", (c >= 32 && c <= 126) ? static_cast<char>(c) : '.');
      }
      fmt::print("|\n");
    }
  }

  folly::IOBufQueue in_{folly::IOBufQueue::cacheChainLength()};
  uint8_t proto_level_{4};
};

class Handler final : public wangle::HandlerAdapter<Packet> {
 public:
  void read(Context* ctx, Packet pkt) override {
    switch (pkt.type) {
      case Type::CONNECT: {
        proto_ = pkt.protoLevel;
        Packet resp{Type::CONNACK};
        resp.protoLevel = proto_;
        write(ctx, std::move(resp));
        break;
      }
      case Type::SUBSCRIBE: {
        Packet resp{Type::SUBACK};
        resp.protoLevel = proto_;
        resp.suback.pktId = pkt.subscribe.pktId;
        resp.suback.grants.reserve(pkt.subscribe.reqQoS.size());
        for (auto q : pkt.subscribe.reqQoS) resp.suback.grants.push_back(q >= 1 ? 1 : 0);
        for (size_t i = 0; i < pkt.subscribe.topics.size(); ++i) {
          fmt::print("SUBSCRIBE topic='{}' reqQoS={}\n", pkt.subscribe.topics[i],
                     pkt.subscribe.reqQoS[i]);
        }
        write(ctx, std::move(resp));
        break;
      }
      case Type::PUBLISH: {
        fmt::print("PUBLISH topic='{}' qos={} payloadLen={}\n", pkt.publish.topic, pkt.publish.qos,
                   pkt.publish.payload.size());
        if (pkt.publish.qos == 1) {
          Packet resp{Type::PUBACK};
          resp.puback.pktId = pkt.publish.pktId;
          write(ctx, std::move(resp));
        }
        break;
      }
      case Type::PINGREQ: {
        Packet resp{Type::PINGRESP};
        write(ctx, std::move(resp));
        break;
      }
      case Type::DISCONNECT: {
        ctx->fireClose();
        break;
      }
      default:
        break;
    }
  }

  folly::Future<folly::Unit> write(Context* ctx, Packet pkt) override {
    return ctx->fireWrite(std::move(pkt));
  }

  void readEOF(Context* ctx) override { ctx->fireClose(); }
  void readException(Context* ctx, folly::exception_wrapper e) override {
    fmt::print("ERROR: {}\n", e.what());
    ctx->fireClose();
  }

 private:
  uint8_t proto_{4};
};

class PipelineFactory final : public wangle::PipelineFactory<Pipeline> {
 public:
  Pipeline::Ptr newPipeline(std::shared_ptr<folly::AsyncTransport> sock) override {
    auto pipeline = Pipeline::create();
    pipeline->addBack(wangle::AsyncSocketHandler(sock));
    pipeline->addBack(Codec{});
    pipeline->addBack(Handler{});
    pipeline->finalize();
    return pipeline;
  }
};

Server::Server() {}
Server::~Server() {}

void Server::start() {
  fmt::print("Server::start\n");
  wangle::ServerBootstrap<Pipeline> server;
  server.childPipeline(std::make_shared<PipelineFactory>());
  server.bind(1883);
  server.waitForStop();
}
}  // namespace warp::mqtt
