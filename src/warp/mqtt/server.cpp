#include <fmt/core.h>
#include <folly/io/IOBuf.h>
#include <folly/io/IOBufQueue.h>
#include <wangle/bootstrap/ServerBootstrap.h>
#include <wangle/channel/AsyncSocketHandler.h>
#include <wangle/channel/Pipeline.h>
#include <warp/mqtt/server.h>

namespace warp::mqtt {
namespace {
inline bool readVarint(folly::io::Cursor& c, uint32_t& out, int& used) {
  out = 0;
  used = 0;
  uint32_t mul = 1;
  for (;;) {
    if (!c.canAdvance(1)) return false;
    uint8_t b = c.read<uint8_t>();
    out += (b & 0x7F) * mul;
    mul *= 128;
    ++used;
    if ((b & 0x80) == 0 || used == 4) break;
  }
  return true;
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
    auto buf = encode(std::move(pkt));
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

    uint32_t rl = 0;
    int rlBytes = 0;
    if (!readVarint(c, rl, rlBytes)) return false;

    const size_t headerLen = 1 + rlBytes;
    const size_t totalLen = headerLen + rl;
    if (in.chainLength() < totalLen) return false;

    if (auto* head = in.front()) {
      auto v = head->clone();
      v->coalesce();
      fmt::print("READ: {} bytes (type={})\n", totalLen, (int)type);
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
        protoLevel_ = d.read<uint8_t>();
        out = Packet{Type::CONNECT};
        out.protoLevel = protoLevel_;
        in.trimStart(totalLen);
        return true;
      }
      case 8: {  // SUBSCRIBE
        Packet p{Type::SUBSCRIBE};
        p.protoLevel = protoLevel_;
        if (!d.canAdvance(2)) break;
        p.subscribe.pktId = d.readBE<uint16_t>();
        size_t consumed = 2;

        if (protoLevel_ == 5) {
          uint32_t propLen = 0;
          int propB = 0;
          if (!readVarint(d, propLen, propB)) break;
          if (!d.canAdvance(propLen)) break;
          d.skip(propLen);
          consumed += propB + propLen;
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
        p.protoLevel = protoLevel_;
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

        if (protoLevel_ == 5) {
          uint32_t propLen = 0;
          int propB = 0;
          if (!readVarint(d, propLen, propB)) break;
          if (!d.canAdvance(propLen)) break;
          d.skip(propLen);
          consumed += propB + propLen;
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

  static std::unique_ptr<folly::IOBuf> encode(Packet p) {
    switch (p.type) {
      case Type::CONNACK:
        return encConnack(p.protoLevel);
      case Type::SUBACK:
        return encSuback(p.protoLevel, p.suback.pktId, p.suback.grants);
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

  static std::unique_ptr<folly::IOBuf> encSuback(uint8_t proto, uint16_t pid,
                                                 const std::vector<uint8_t>& grants) {
    std::vector<uint8_t> out;
    out.reserve(4 + grants.size());
    out.push_back(0x90);  // SUBACK
    uint32_t rl = (proto == 5) ? (uint32_t)(2 + 1 + grants.size()) : (uint32_t)(2 + grants.size());
    writeVarint(rl, out);
    out.push_back(uint8_t(pid >> 8));
    out.push_back(uint8_t(pid & 0xFF));
    if (proto == 5) out.push_back(0x00);  // properties len = 0
    out.insert(out.end(), grants.begin(), grants.end());
    return folly::IOBuf::copyBuffer(out.data(), out.size());
  }

  static std::unique_ptr<folly::IOBuf> encPuback(uint16_t pid) {
    uint8_t b[4] = {0x40, 0x02, uint8_t(pid >> 8), uint8_t(pid & 0xFF)};
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
        if (i + j < show)
          fmt::print("{:02x} ", data[i + j]);
        else
          fmt::print("   ");
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
  uint8_t protoLevel_{4};
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
                     (int)pkt.subscribe.reqQoS[i]);
        }
        write(ctx, std::move(resp));
        break;
      }
      case Type::PUBLISH: {
        fmt::print("PUBLISH topic='{}' qos={} payloadLen={}\n", pkt.publish.topic,
                   (int)pkt.publish.qos, pkt.publish.payload.size());
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
