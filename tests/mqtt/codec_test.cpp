#include "warp/mqtt/codec.h"

#include <gtest/gtest.h>

class CodecTest : public ::testing::Test {
protected:
  // empty
};

TEST_F(CodecTest, EncodeTest) {
  auto const msg = warp::mqtt::Connect::Builder{}
                       .withLevel(warp::mqtt::Level::V311)
                       .withCleanSession(true)
                       .withKeepAlive(60)
                       .withClient("TestClient")
                       .build();
  auto const data = warp::mqtt::Codec::encode(msg);
  ASSERT_NE(data, nullptr);
  EXPECT_GT(data->computeChainDataLength(), 0u);
}

TEST_F(CodecTest, DecodeTest) {
  auto const exp = warp::mqtt::Publish::Builder{}
                       .withTopic("foo/bar")
                       .withPayload("TEST")
                       .withQos(1)
                       .withPacketId(123)
                       .build();
  auto data = warp::mqtt::Codec::encode(exp);
  ASSERT_NE(data, nullptr);
  EXPECT_GT(data->computeChainDataLength(), 0u);

  folly::IOBufQueue q(folly::IOBufQueue::cacheChainLength());
  q.append(std::move(data));

  auto decoded = warp::mqtt::Codec::decode(q);
  if (!decoded) {
    FAIL() << "Codec::decode() returned std::nullopt";
    return;
  }
  ASSERT_TRUE(decoded.has_value());
  ASSERT_TRUE(std::holds_alternative<warp::mqtt::Publish>(*decoded));

  auto const& msg = std::get<warp::mqtt::Publish>(*decoded);
  EXPECT_EQ(msg.head.topic, exp.head.topic);
  EXPECT_EQ(msg.data.data, exp.data.data);
  EXPECT_EQ(msg.head.qos, exp.head.qos);
  EXPECT_EQ(msg.head.packetId, exp.head.packetId);
}
