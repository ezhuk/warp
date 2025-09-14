#include <fmt/format.h>
#include <folly/Varint.h>
#include <folly/executors/CPUThreadPoolExecutor.h>
#include <folly/system/HardwareConcurrency.h>
#include <wangle/bootstrap/ServerBootstrap.h>
#include <wangle/channel/AsyncSocketHandler.h>
#include <wangle/channel/EventBaseHandler.h>
#include <wangle/channel/Pipeline.h>
#include <wangle/service/ExecutorFilter.h>
#include <wangle/service/ServerDispatcher.h>
#include <warp/mqtt/server.h>

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

class Codec {
public:
  static std::optional<Message> decode(folly::IOBufQueue& q) {
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

  static std::unique_ptr<folly::IOBuf> encode(const Message& msg) {
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
};

class Handler final
    : public wangle::Handler<folly::IOBufQueue&, Message, Message, std::unique_ptr<folly::IOBuf>> {
public:
  using Context = typename wangle::Handler<
      folly::IOBufQueue&, Message, Message, std::unique_ptr<folly::IOBuf>>::Context;

  void read(Context* ctx, folly::IOBufQueue& q) override {
    for (;;) {
      auto msg = Codec::decode(q);
      if (!msg) {
        break;
      }
      ctx->fireRead(std::move(*msg));
    }
  }

  folly::Future<folly::Unit> write(Context* ctx, Message msg) override {
    auto out = Codec::encode(msg);
    return ctx->fireWrite(std::move(out));
  }
};

class Service final : public wangle::Service<Message, Message> {
public:
  folly::Future<Message> operator()(Message msg) override {
    switch (msg.type) {
      case Type::Connect:
        return folly::makeFuture<Message>(Message{.type = Type::ConnAck});
      case Type::Publish:
        return folly::makeFuture<Message>(Message{.type = Type::None});
      case Type::Subscribe:
        return folly::makeFuture<Message>(
            Message{.type = Type::SubAck, .id = msg.id, .topics = msg.topics}
        );
      case Type::PingReq:
        return folly::makeFuture<Message>(Message{.type = Type::PingResp});
      case Type::Disconnect:
        return folly::makeFuture<Message>(Message{.type = Type::None});
      default:
        break;
    }
    return folly::makeFuture<Message>(Message{.type = Type::None});
  }
};

using Pipeline = wangle::Pipeline<folly::IOBufQueue&, Message>;

class PipelineFactory final : public wangle::PipelineFactory<Pipeline> {
public:
  explicit PipelineFactory(size_t threads)
      : service_(
            std::make_shared<folly::CPUThreadPoolExecutor>(threads), std::make_shared<Service>()
        ) {}

  Pipeline::Ptr newPipeline(std::shared_ptr<folly::AsyncTransport> sock) override {
    auto pipeline = Pipeline::create();
    pipeline->addBack(wangle::AsyncSocketHandler(sock));
    pipeline->addBack(wangle::EventBaseHandler());
    pipeline->addBack(Handler());
    pipeline->addBack(wangle::MultiplexServerDispatcher<Message, Message>(&service_));
    pipeline->finalize();
    return pipeline;
  }

private:
  wangle::ExecutorFilter<Message, Message> service_;
};

namespace {
std::shared_ptr<wangle::ServerBootstrap<Pipeline>> server;
}

Server::Server(ServerOptions const& options) : options_(std::make_shared<ServerOptions>(options)) {}

Server::~Server() {}

void Server::start() {
  server = std::make_shared<wangle::ServerBootstrap<Pipeline>>();
  server->childPipeline(std::make_shared<PipelineFactory>(options_->threads));
  server->bind(options_->port);
  server->waitForStop();
  server.reset();
}

void Server::stop() { server->stop(); }
}  // namespace warp::mqtt
