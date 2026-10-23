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

struct Subscribe {
  std::uint16_t packetId;
  std::vector<std::uint8_t> qos;
};

struct SubAck {
  std::uint16_t packetId;
  std::vector<std::uint8_t> codes;
};

using Packet = std::variant<Connect, ConnAck, Disconnect, PingReq, PingResp, Subscribe, SubAck>;

std::optional<Packet> decode(folly::IOBufQueue& input) {
  if (input.chainLength() < 2) {
    return std::nullopt;
  }

  folly::io::Cursor cursor(input.front());
  auto const type = cursor.read<std::uint8_t>();
  std::size_t length = 0;
  std::size_t multiplier = 1;
  std::size_t headerSize = 1;
  bool complete = false;

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
      complete = true;
      break;
    }
    multiplier *= 128;
  }

  if (!complete || length > 1024 * 1024) {
    throw std::runtime_error("Invalid MQTT packet length");
  }

  if (input.chainLength() < headerSize + length) {
    return std::nullopt;
  }

  std::unique_ptr<folly::IOBuf> body;
  if (length > 0) {
    cursor.clone(body, length);
  }

  auto readString = [](folly::io::Cursor& c) {
    if (!c.canAdvance(2)) {
      throw std::runtime_error("Invalid MQTT string length");
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
      folly::io::Cursor c(body.get());
      auto const protocol = readString(c);
      if (!c.canAdvance(4)) {
        throw std::runtime_error("Truncated CONNECT");
      }
      auto const version = c.read<std::uint8_t>();
      auto const flags = c.read<std::uint8_t>();
      auto const keepAlive = c.readBE<std::uint16_t>();
      if (protocol != "MQTT" || version != 4) {
        throw std::runtime_error("Unsupported MQTT protocol");
      }
      if (flags & ~std::uint8_t{0x02}) {
        throw std::runtime_error("Unsupported CONNECT flags");
      }
      auto clientId = readString(c);
      if (c.totalLength() != 0 || (clientId.empty() && !(flags & 0x02))) {
        throw std::runtime_error("Invalid CONNECT");
      }
      packet = Connect{std::move(clientId), keepAlive, static_cast<bool>(flags & 0x02)};
      break;
    }
    case 0x20: {
      if (length != 2) {
        throw std::runtime_error("Invalid CONNACK length");
      }
      folly::io::Cursor c(body.get());
      auto const flags = c.read<std::uint8_t>();
      auto const code = c.read<std::uint8_t>();
      if (flags > 1 || code > 5 || (code != 0 && flags != 0)) {
        throw std::runtime_error("Invalid CONNACK");
      }
      packet = ConnAck{static_cast<bool>(flags), code};
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
    case 0x82: {
      if (length < 6) {
        throw std::runtime_error("Invalid SUBSCRIBE length");
      }
      folly::io::Cursor c(body.get());
      auto const packetId = c.readBE<std::uint16_t>();
      if (packetId == 0) {
        throw std::runtime_error("Invalid SUBSCRIBE packet identifier");
      }
      Subscribe subscribe{packetId, {}};
      while (c.totalLength() > 0) {
        auto const topic = readString(c);
        if (topic.empty() || !c.canAdvance(1)) {
          throw std::runtime_error("Invalid SUBSCRIBE topic");
        }
        auto const qos = c.read<std::uint8_t>();
        if (qos > 2) {
          throw std::runtime_error("Invalid SUBSCRIBE QoS");
        }
        subscribe.qos.push_back(qos);
      }
      if (subscribe.qos.empty()) {
        throw std::runtime_error("Empty SUBSCRIBE");
      }
      packet = std::move(subscribe);
      break;
    }
    default:
      throw std::runtime_error("Unsupported MQTT packet");
  }

  input.trimStart(headerSize + length);
  return packet;
}

std::unique_ptr<folly::IOBuf> encode(const Packet& packet) {
  std::vector<std::uint8_t> bytes;
  auto write16 = [&](std::uint16_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value >> 8));
    bytes.push_back(static_cast<std::uint8_t>(value));
  };

  auto writeString = [&](const std::string& value) {
    if (value.size() > 65535) {
      throw std::runtime_error("MQTT string too long");
    }
    write16(static_cast<std::uint16_t>(value.size()));
    bytes.insert(bytes.end(), value.begin(), value.end());
  };

  auto writeLength = [&](std::size_t length) {
    if (length > 268435455) {
      throw std::runtime_error("MQTT packet too large");
    }
    do {
      auto byte = static_cast<std::uint8_t>(length % 128);
      length /= 128;
      if (length) {
        byte |= 0x80;
      }
      bytes.push_back(byte);
    } while (length);
  };

  std::visit(
      [&](const auto& value) {
        using T = std::decay_t<decltype(value)>;

        if constexpr (std::is_same_v<T, Connect>) {
          if (value.clientId.size() > 65535 || (value.clientId.empty() && !value.cleanSession)) {
            throw std::runtime_error("Invalid CONNECT client ID");
          }
          bytes.push_back(0x10);
          writeLength(12 + value.clientId.size());
          writeString("MQTT");
          bytes.push_back(4);
          bytes.push_back(value.cleanSession ? 0x02 : 0x00);
          write16(value.keepAlive);
          writeString(value.clientId);
        } else if constexpr (std::is_same_v<T, ConnAck>) {
          if (value.returnCode > 5 || (value.returnCode != 0 && value.sessionPresent)) {
            throw std::runtime_error("Invalid CONNACK");
          }
          bytes = {0x20, 0x02, static_cast<std::uint8_t>(value.sessionPresent), value.returnCode};
        } else if constexpr (std::is_same_v<T, PingReq>) {
          bytes = {0xc0, 0x00};
        } else if constexpr (std::is_same_v<T, PingResp>) {
          bytes = {0xd0, 0x00};
        } else if constexpr (std::is_same_v<T, Disconnect>) {
          bytes = {0xe0, 0x00};
        } else if constexpr (std::is_same_v<T, SubAck>) {
          if (value.packetId == 0 || value.codes.empty()) {
            throw std::runtime_error("Invalid SUBACK");
          }
          bytes.push_back(0x90);
          writeLength(2 + value.codes.size());
          write16(value.packetId);
          for (auto code : value.codes) {
            if (code > 2 && code != 0x80) {
              throw std::runtime_error("Invalid SUBACK return code");
            }
            bytes.push_back(code);
          }
        } else if constexpr (std::is_same_v<T, Subscribe>) {
          throw std::runtime_error("SUBSCRIBE encoding not implemented");
        }
      },
      packet
  );

  return folly::IOBuf::copyBuffer(bytes.data(), bytes.size());
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
      return std::optional<Packet>{SubAck{subscribe->packetId, std::move(subscribe->qos)}};
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
