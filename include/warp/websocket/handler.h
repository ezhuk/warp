#pragma once

#include <folly/io/IOBuf.h>
#include <proxygen/httpserver/RequestHandler.h>

#include <vector>

namespace warp::websocket {
class Frame {
public:
  uint8_t opcode{0};
  bool fin{true};
  std::unique_ptr<folly::IOBuf> data;
};

class Stream {
public:
  enum class Error {};

  folly::Expected<std::vector<Frame>, Error> parse(std::unique_ptr<folly::IOBuf> chain);

  std::unique_ptr<folly::IOBuf> frame(
      std::unique_ptr<folly::IOBuf> data, uint8_t opcode, bool fin = true
  );
};

class Handler : public proxygen::RequestHandler {
public:
  void onRequest(std::unique_ptr<proxygen::HTTPMessage> request) noexcept override;
  void onBody(std::unique_ptr<folly::IOBuf> body) noexcept override;
  void onEOM() noexcept override;
  void onUpgrade(proxygen::UpgradeProtocol) noexcept override;
  void requestComplete() noexcept override { delete this; }
  void onError(proxygen::ProxygenError) noexcept override { delete this; }

protected:
  virtual void onDataFrame(std::unique_ptr<folly::IOBuf> data, bool fin) = 0;
  virtual void onTextFrame(std::unique_ptr<folly::IOBuf>, bool) {}

  void sendData(std::unique_ptr<folly::IOBuf> data, bool fin = true);
  void sendText(std::unique_ptr<folly::IOBuf> data, bool fin = true);
  void sendPong(std::unique_ptr<folly::IOBuf> data);
  void sendClose(uint16_t code = 1000, folly::StringPiece reason = "");

private:
  std::unique_ptr<Stream> stream_;
};
}  // namespace warp::websocket
