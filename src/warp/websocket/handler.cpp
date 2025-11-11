#include "warp/websocket/handler.h"

#include <proxygen/httpserver/ResponseBuilder.h>

namespace warp::websocket {
folly::Expected<std::vector<Frame>, Stream::Error> Stream::parse(
    std::unique_ptr<folly::IOBuf> chain
) {
  std::vector<Frame> out;
  auto fb = chain->clone()->coalesce();
  const uint8_t* p = fb.data();
  size_t n = fb.size();

  while (n >= 2) {
    uint8_t b0 = p[0];
    uint8_t b1 = p[1];
    bool fin = (b0 & 0x80) != 0;
    uint8_t opcode = (b0 & 0x0F);
    bool masked = (b1 & 0x80) != 0;
    uint64_t plen = (b1 & 0x7F);
    size_t off = 2;

    if (plen == 126) {
      if (n < off + 2) break;
      plen = (static_cast<uint64_t>(p[off]) << 8) | p[off + 1];
      off += 2;
    } else if (plen == 127) {
      if (n < off + 8) break;
      plen = 0;
      for (int i = 0; i < 8; ++i) plen = (plen << 8) | p[off + i];
      off += 8;
    }

    const uint8_t* mask = nullptr;
    if (masked) {
      if (n < off + 4) break;
      mask = p + off;
      off += 4;
    }

    if (n < off + plen) break;

    std::unique_ptr<folly::IOBuf> payload;
    if (plen > 0) {
      auto buf = folly::IOBuf::create(plen);
      buf->append(plen);
      if (masked) {
        for (uint64_t i = 0; i < plen; ++i) {
          buf->writableData()[i] = p[off + i] ^ mask[i & 3];
        }
      } else {
        std::memcpy(buf->writableData(), p + off, plen);
      }
      payload = std::move(buf);
    }

    out.push_back(Frame{static_cast<OpCode>(opcode), fin, std::move(payload)});

    size_t frame_size = off + plen;
    p += frame_size;
    n -= frame_size;
  }

  return out;
}

std::unique_ptr<folly::IOBuf> Stream::frame(
    std::unique_ptr<folly::IOBuf> data, uint8_t opcode, bool fin
) {
  size_t len = data ? data->computeChainDataLength() : 0;
  uint8_t hdr[14];
  size_t off = 0;
  hdr[off++] = (fin ? 0x80 : 0x00) | (opcode & 0x0F);
  if (len < 126) {
    hdr[off++] = static_cast<uint8_t>(len);
  } else if (len <= 0xFFFF) {
    hdr[off++] = 126;
    hdr[off++] = static_cast<uint8_t>((len >> 8) & 0xFF);
    hdr[off++] = static_cast<uint8_t>(len & 0xFF);
  } else {
    hdr[off++] = 127;
    for (int i = 7; i >= 0; --i) {
      hdr[off++] = static_cast<uint8_t>((len >> (8 * i)) & 0xFF);
    }
  }
  auto head = folly::IOBuf::copyBuffer(hdr, off);
  if (data) {
    head->appendChain(std::move(data));
  }
  return head;
}

void Handler::onRequest(std::unique_ptr<proxygen::HTTPMessage> request) noexcept {
  if (request->getHeaders().exists(proxygen::HTTP_HEADER_UPGRADE) &&
      request->getHeaders().exists(proxygen::HTTP_HEADER_CONNECTION)) {
    proxygen::ResponseBuilder response(downstream_);
    response.status(101, "Switching Protocols")
        .setEgressWebsocketHeaders()
        .header("Sec-WebSocket-Version", "13")
        .header("Sec-WebSocket-Protocol", "mqtt")
        .send();
  } else {
    proxygen::ResponseBuilder(downstream_).rejectUpgradeRequest();
  }
}

void Handler::onBody(std::unique_ptr<folly::IOBuf> body) noexcept {
  auto frames = stream_->parse(std::move(body));
  if (!frames.hasError()) {
    for (auto& f : *frames) {
      switch (f.opcode) {
        case OpCode::Continuation:
        case OpCode::Binary:
          onDataFrame(std::move(f.data), f.fin);
          break;
        case OpCode::Text:
          onTextFrame(std::move(f.data), f.fin);
          break;
        case OpCode::Ping:
          sendPong(std::move(f.data));
          break;
        case OpCode::Close: {
          uint16_t code = 1000;
          folly::StringPiece reason;
          if (f.data && f.data->length() >= 2) {
            auto d = f.data->data();
            code = (static_cast<uint16_t>(d[0]) << 8) | d[1];
            if (f.data->length() > 2) {
              reason =
                  folly::StringPiece(reinterpret_cast<const char*>(d + 2), f.data->length() - 2);
            }
          }
          sendClose(code);
          break;
        }
        default:
          sendClose(1003);
          return;
      }
    }
  } else {
    sendClose(1002);
  }
}

void Handler::onEOM() noexcept {
  if (!stream_) {
    proxygen::ResponseBuilder(downstream_).sendWithEOM();
  }
}

void Handler::onUpgrade(proxygen::UpgradeProtocol) noexcept {
  stream_ = std::make_unique<Stream>();
}

void Handler::sendData(std::unique_ptr<folly::IOBuf> data, bool fin) {
  auto framed = stream_->frame(std::move(data), 0x2, true);
  proxygen::ResponseBuilder(downstream_).body(std::move(framed)).send();
}

void Handler::sendPong(std::unique_ptr<folly::IOBuf> data) {
  auto framed = stream_->frame(std::move(data), 0xA, true);
  proxygen::ResponseBuilder(downstream_).body(std::move(framed)).send();
}

void Handler::sendClose(uint16_t code, folly::StringPiece reason) {
  std::unique_ptr<folly::IOBuf> payload;
  {
    auto buf = folly::IOBuf::create(2 + reason.size());
    buf->append(2 + reason.size());
    auto* w = buf->writableData();
    w[0] = static_cast<uint8_t>((code >> 8) & 0xFF);
    w[1] = static_cast<uint8_t>(code & 0xFF);
    if (!reason.empty()) {
      std::memcpy(w + 2, reason.data(), reason.size());
    }
    payload = std::move(buf);
  }
  auto framed = stream_->frame(std::move(payload), 0x8, true);
  proxygen::ResponseBuilder(downstream_).body(std::move(framed)).send();
}
}  // namespace warp::websocket
