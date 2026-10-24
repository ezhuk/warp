// server.cpp

#include "server.h"

#include <folly/ExceptionWrapper.h>
#include <folly/Unit.h>
#include <folly/futures/Future.h>
#include <folly/io/Cursor.h>
#include <folly/io/IOBuf.h>
#include <folly/io/IOBufQueue.h>
#include <wangle/bootstrap/ServerBootstrap.h>
#include <wangle/channel/AsyncSocketHandler.h>
#include <wangle/channel/EventBaseHandler.h>
#include <wangle/channel/Handler.h>
#include <wangle/channel/Pipeline.h>
#include <wangle/service/ServerDispatcher.h>
#include <wangle/service/Service.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

#include "scoped_event_base.h"

namespace warp {
namespace {
struct Connect {
  std::string clientId;
  std::uint16_t keepAlive{60};
  bool cleanSession{true};
};

struct ConnAck {
  bool sessionPresent{false};
  std::uint8_t returnCode{0};
};

struct Disconnect {};
struct PingReq {};
struct PingResp {};

struct Publish {
  std::string topic;
  std::unique_ptr<folly::IOBuf> payload;
};

struct Subscription {
  std::string topic;
  std::uint8_t qos;
};

struct Subscribe {
  std::uint16_t packetId;
  std::vector<Subscription> topics;
};

struct SubAck {
  std::uint16_t packetId;
  std::vector<std::uint8_t> codes;
};

using Packet =
    std::variant<Connect, ConnAck, Disconnect, PingReq, PingResp, Publish, Subscribe, SubAck>;

std::optional<Packet> decode(folly::IOBufQueue& input) {
  if (input.chainLength() < 2) {
    return std::nullopt;
  }

  folly::io::Cursor cursor(input.front());
  auto const type = cursor.read<std::uint8_t>();

  std::size_t length = 0;
  std::size_t multiplier = 1;
  std::size_t headerSize = 1;

  for (int i = 0; i < 4; ++i) {
    if (!cursor.canAdvance(1)) {
      return std::nullopt;
    }
    auto const byte = cursor.read<std::uint8_t>();
    ++headerSize;
    length += (byte & 0x7f) * multiplier;
    if (!(byte & 0x80)) {
      if (i > 0 && byte == 0) {
        throw std::runtime_error("Nonminimal MQTT Remaining Length");
      }
      break;
    }
    if (i == 3) {
      throw std::runtime_error("Invalid MQTT Remaining Length");
    }
    multiplier *= 128;
  }

  if (length > 1024 * 1024) {
    throw std::runtime_error("MQTT packet too large");
  }

  if (!cursor.canAdvance(length)) {
    return std::nullopt;
  }

  folly::io::Cursor body(cursor, length);

  auto readString = [](folly::io::Cursor& c) {
    if (!c.canAdvance(2)) {
      throw std::runtime_error("Invalid MQTT string");
    }
    auto const size = c.readBE<std::uint16_t>();
    if (!c.canAdvance(size)) {
      throw std::runtime_error("Truncated MQTT string");
    }
    return c.readFixedString(size);
  };

  Packet packet;

  switch (type) {
    case 0x10: {
      auto const protocol = readString(body);
      if (!body.canAdvance(4)) {
        throw std::runtime_error("Truncated CONNECT");
      }
      auto const version = body.read<std::uint8_t>();
      auto const flags = body.read<std::uint8_t>();
      auto const keepAlive = body.readBE<std::uint16_t>();
      if (protocol != "MQTT" || version != 4) {
        throw std::runtime_error("Unsupported MQTT protocol");
      }
      if (flags & ~std::uint8_t{0x02}) {
        throw std::runtime_error("Unsupported CONNECT flags");
      }
      auto clientId = readString(body);
      if (body.totalLength() != 0 || (clientId.empty() && !(flags & 0x02))) {
        throw std::runtime_error("Invalid CONNECT");
      }
      packet = Connect{std::move(clientId), keepAlive, static_cast<bool>(flags & 0x02)};
      break;
    }
    case 0x20: {
      if (length != 2) {
        throw std::runtime_error("Invalid CONNACK length");
      }
      auto const flags = body.read<std::uint8_t>();
      auto const code = body.read<std::uint8_t>();
      if (flags > 1 || code > 5 || (code != 0 && flags != 0)) {
        throw std::runtime_error("Invalid CONNACK");
      }
      packet = ConnAck{static_cast<bool>(flags), code};
      break;
    }
    case 0x30: {
      auto topic = readString(body);
      if (topic.empty()) {
        throw std::runtime_error("Invalid PUBLISH topic");
      }
      std::unique_ptr<folly::IOBuf> payload;
      body.clone(payload, body.totalLength());
      packet = Publish{std::move(topic), std::move(payload)};
      break;
    }
    case 0x82: {
      if (length < 6) {
        throw std::runtime_error("Invalid SUBSCRIBE length");
      }
      auto const packetId = body.readBE<std::uint16_t>();
      if (packetId == 0) {
        throw std::runtime_error("Invalid SUBSCRIBE packet identifier");
      }
      Subscribe subscribe{packetId, {}};
      while (body.totalLength() > 0) {
        auto const topic = readString(body);
        if (topic.empty() || !body.canAdvance(1)) {
          throw std::runtime_error("Invalid SUBSCRIBE topic");
        }
        auto const qos = body.read<std::uint8_t>();
        if (qos > 2) {
          throw std::runtime_error("Invalid SUBSCRIBE QoS");
        }
        subscribe.topics.push_back({std::move(topic), qos});
      }
      if (subscribe.topics.empty()) {
        throw std::runtime_error("Empty SUBSCRIBE");
      }
      packet = std::move(subscribe);
      break;
    }
    case 0xc0:
      if (length != 0) {
        throw std::runtime_error("Invalid PINGREQ");
      }
      packet = PingReq{};
      break;
    case 0xd0:
      if (length != 0) {
        throw std::runtime_error("Invalid PINGRESP");
      }
      packet = PingResp{};
      break;
    case 0xe0:
      if (length != 0) {
        throw std::runtime_error("Invalid DISCONNECT");
      }
      packet = Disconnect{};
      break;
    default:
      throw std::runtime_error("Unsupported MQTT packet");
  }

  input.trimStart(headerSize + length);
  return packet;
}

std::unique_ptr<folly::IOBuf> encode(const Packet& packet) {
  folly::IOBufQueue queue(folly::IOBufQueue::cacheChainLength());
  folly::io::QueueAppender out(&queue, 256);

  auto writeLength = [&](std::size_t length) {
    if (length > 268435455) {
      throw std::runtime_error("MQTT packet too large");
    }
    do {
      auto byte = static_cast<std::uint8_t>(length % 128);
      length /= 128;
      out.write<std::uint8_t>(byte | (length ? 0x80 : 0));
    } while (length);
  };

  auto writeString = [&](const std::string& value) {
    if (value.size() > 65535) {
      throw std::runtime_error("MQTT string too long");
    }
    out.writeBE<std::uint16_t>(value.size());
    out.push(reinterpret_cast<const std::uint8_t*>(value.data()), value.size());
  };

  std::visit(
      [&](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, Connect>) {
          if (value.clientId.empty() && !value.cleanSession) {
            throw std::runtime_error("Invalid CONNECT client ID");
          }
          if (value.clientId.size() > 65535) {
            throw std::runtime_error("MQTT client ID too long");
          }
          out.write<std::uint8_t>(0x10);
          writeLength(12 + value.clientId.size());
          writeString("MQTT");
          out.write<std::uint8_t>(4);
          out.write<std::uint8_t>(value.cleanSession ? 0x02 : 0);
          out.writeBE<std::uint16_t>(value.keepAlive);
          writeString(value.clientId);
        } else if constexpr (std::is_same_v<T, ConnAck>) {
          if (value.returnCode > 5 || (value.returnCode != 0 && value.sessionPresent)) {
            throw std::runtime_error("Invalid CONNACK");
          }
          out.write<std::uint8_t>(0x20);
          out.write<std::uint8_t>(0x02);
          out.write<std::uint8_t>(value.sessionPresent ? 1 : 0);
          out.write<std::uint8_t>(value.returnCode);
        } else if constexpr (std::is_same_v<T, PingReq>) {
          out.write<std::uint8_t>(0xc0);
          out.write<std::uint8_t>(0x00);
        } else if constexpr (std::is_same_v<T, PingResp>) {
          out.write<std::uint8_t>(0xd0);
          out.write<std::uint8_t>(0x00);
        } else if constexpr (std::is_same_v<T, Disconnect>) {
          out.write<std::uint8_t>(0xe0);
          out.write<std::uint8_t>(0x00);
        } else if constexpr (std::is_same_v<T, SubAck>) {
          if (value.packetId == 0 || value.codes.empty()) {
            throw std::runtime_error("Invalid SUBACK");
          }
          out.write<std::uint8_t>(0x90);
          writeLength(2 + value.codes.size());
          out.writeBE<std::uint16_t>(value.packetId);
          for (auto code : value.codes) {
            if (code > 2 && code != 0x80) {
              throw std::runtime_error("Invalid SUBACK return code");
            }
            out.write<std::uint8_t>(code);
          }
        } else if constexpr (std::is_same_v<T, Publish>) {
          if (value.topic.empty() || value.topic.size() > 65535) {
            throw std::runtime_error("Invalid PUBLISH topic");
          }
          auto const payloadSize = value.payload ? value.payload->computeChainDataLength() : 0;
          out.write<std::uint8_t>(0x30);
          writeLength(2 + value.topic.size() + payloadSize);
          writeString(value.topic);
          if (value.payload) {
            out.push(folly::io::Cursor(value.payload.get()), payloadSize);
          }
        } else if constexpr (std::is_same_v<T, Subscribe>) {
          throw std::runtime_error("SUBSCRIBE encoding not implemented");
        }
      },
      packet
  );

  return queue.move();
}

using Pipeline = wangle::Pipeline<folly::IOBufQueue&, std::optional<Packet>>;

class PacketHandler final
    : public wangle::Handler<
          folly::IOBufQueue&, Packet, std::optional<Packet>, std::unique_ptr<folly::IOBuf>> {
public:
  void read(Context* ctx, folly::IOBufQueue& data) override {
    try {
      while (active_) {
        auto packet = decode(data);
        if (!packet) {
          break;
        }
        ctx->fireRead(std::move(*packet));
      }
    } catch (std::exception const&) {
      ctx->fireClose();
    }
  }

  void readEOF(Context* ctx) override {
    active_ = false;
    ctx->fireClose();
  }

  void readException(Context* ctx, folly::exception_wrapper) override {
    active_ = false;
    ctx->fireClose();
  }

  folly::Future<folly::Unit> write(Context* ctx, std::optional<Packet> packet) override {
    return packet ? ctx->fireWrite(encode(*packet)) : folly::makeFuture();
  }

  void transportActive(Context* ctx) override {
    active_ = true;
    ctx->fireTransportActive();
  }

  void transportInactive(Context* ctx) override {
    active_ = false;
    ctx->fireTransportInactive();
  }

private:
  bool active_{true};
};

class Service final : public wangle::Service<Packet, std::optional<Packet>> {
public:
  folly::Future<std::optional<Packet>> operator()(Packet packet) override {
    if (std::holds_alternative<Connect>(packet)) {
      return std::optional<Packet>{ConnAck{false, 0}};
    }

    if (std::holds_alternative<PingReq>(packet)) {
      return std::optional<Packet>{PingResp{}};
    }

    if (auto* subscribe = std::get_if<Subscribe>(&packet)) {
      SubAck ack{subscribe->packetId, {}};
      for (auto const& topic : subscribe->topics) {
        ack.codes.push_back(0);
      }
      return std::optional<Packet>{std::move(ack)};
    }

    return std::optional<Packet>{};
  }
};

class PipelineFactory final : public wangle::PipelineFactory<Pipeline> {
public:
  Pipeline::Ptr newPipeline(std::shared_ptr<folly::AsyncTransport> socket) override {
    auto pipeline = Pipeline::create();
    pipeline->addBack(wangle::AsyncSocketHandler(socket));
    pipeline->addBack(wangle::EventBaseHandler());
    pipeline->addBack(PacketHandler());
    pipeline->addBack(wangle::MultiplexServerDispatcher<Packet, std::optional<Packet>>(&service_));
    pipeline->finalize();
    return pipeline;
  }

private:
  Service service_;
};

std::unique_ptr<wangle::ServerBootstrap<Pipeline>> server;
}  // namespace

Server::Server(ServerOptions options) : options_(std::move(options)) {
  // empty
}

Server::~Server() {
  stop();
  join();
}

void Server::start() {
  server = std::make_unique<wangle::ServerBootstrap<Pipeline>>();
  server->childPipeline(std::make_shared<PipelineFactory>());
  server->bind(1883);
}

void Server::stop() {
  if (server) {
    server->stop();
  }
}

void Server::join() {
  if (server) {
    server->join();
  }
}

void Server::run() {
  warp::ScopedEventBase base([this](int) { stop(); }, options_.signals);
  start();
  base.run();
  join();
}
}  // namespace warp
