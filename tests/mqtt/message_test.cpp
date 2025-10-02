#include "warp/mqtt/message.h"

#include <gtest/gtest.h>

namespace {
template <typename T>
std::optional<T> roundtrip(T const& msg) {
  folly::IOBufQueue q(folly::IOBufQueue::cacheChainLength());
  folly::io::QueueAppender a(&q, 128);
  msg.encode(a);

  folly::io::Cursor peek(q.front());
  size_t size = 0;
  auto head = warp::mqtt::readFixedHeader(peek, size);
  if (!head) return std::nullopt;

  auto frame = q.split(size + head->size);
  folly::io::Cursor cur(frame.get());
  cur.skip(size);

  return T::decode(*head, cur);
}
}  // namespace

class MessageTest : public ::testing::Test {
protected:
  // empty
};

TEST_F(MessageTest, ConnectTest) {
  auto const msg = warp::mqtt::Connect::Builder{}
                       .withLevel(warp::mqtt::Level::V311)
                       .withCleanSession(true)
                       .withKeepAlive(60)
                       .withClient("TestClient")
                       .build();
  auto dec = roundtrip(msg);
  if (!dec) {
    FAIL() << "roundtrip() returned std::nullopt";
    return;
  }
  ASSERT_TRUE(dec.has_value());
  EXPECT_EQ(dec->head.level, msg.head.level);
  EXPECT_TRUE((dec->head.flags & 0x02) != 0);
  EXPECT_EQ(dec->head.timeout, msg.head.timeout);
  EXPECT_EQ(dec->data.client, msg.data.client);
}

TEST_F(MessageTest, ConnAckTest) {
  auto const msg = warp::mqtt::ConnAck::Builder{}.withSession(1).withReason(0).build();
  auto dec = roundtrip(msg);
  if (!dec) {
    FAIL() << "roundtrip() returned std::nullopt";
    return;
  }
  ASSERT_TRUE(dec.has_value());
  EXPECT_EQ(dec->head.session, msg.head.session);
  EXPECT_EQ(dec->head.reason, msg.head.reason);
}

TEST_F(MessageTest, PublishTest) {
  auto const msg =
      warp::mqtt::Publish::Builder{}.withTopic("test/foo").withPayload("TEST").withQos(0).build();
  auto dec = roundtrip(msg);
  if (!dec) {
    FAIL() << "roundtrip() returned std::nullopt";
    return;
  }
  ASSERT_TRUE(dec.has_value());
  EXPECT_EQ(dec->head.qos, msg.head.qos);
  EXPECT_EQ(dec->head.topic, msg.head.topic);
  EXPECT_EQ(dec->data.data, msg.data.data);
}

TEST_F(MessageTest, PubAckTest) {
  auto const msg = warp::mqtt::PubAck::Builder{}.withPacketId(7).build();
  auto dec = roundtrip(msg);
  if (!dec) {
    FAIL() << "roundtrip() returned std::nullopt";
    return;
  }
  ASSERT_TRUE(dec.has_value());
  EXPECT_EQ(dec->head.packetId, msg.head.packetId);
}

TEST_F(MessageTest, PubRecTest) {
  auto const msg = warp::mqtt::PubRec::Builder{}.withPacketId(9).build();
  auto dec = roundtrip(msg);
  if (!dec) {
    FAIL() << "roundtrip() returned std::nullopt";
    return;
  }
  ASSERT_TRUE(dec.has_value());
  EXPECT_EQ(dec->head.packetId, msg.head.packetId);
}

TEST_F(MessageTest, PubRelTest) {
  auto const msg = warp::mqtt::PubRel::Builder{}.withPacketId(11).build();
  auto dec = roundtrip(msg);
  if (!dec) {
    FAIL() << "roundtrip() returned std::nullopt";
    return;
  }
  ASSERT_TRUE(dec.has_value());
  EXPECT_EQ(dec->head.packetId, msg.head.packetId);
}

TEST_F(MessageTest, PubCompTest) {
  auto const msg = warp::mqtt::PubComp::Builder{}.withPacketId(13).build();
  auto dec = roundtrip(msg);
  if (!dec) {
    FAIL() << "roundtrip() returned std::nullopt";
    return;
  }
  ASSERT_TRUE(dec.has_value());
  EXPECT_EQ(dec->head.packetId, msg.head.packetId);
}

TEST_F(MessageTest, SubscribeTest) {
  auto const msg = warp::mqtt::Subscribe::Builder{}
                       .withPacketId(21)
                       .addTopic("test/foo", 0)
                       .addTopic("test/bar", 1)
                       .build();
  auto dec = roundtrip(msg);
  if (!dec) {
    FAIL() << "roundtrip() returned std::nullopt";
    return;
  }
  ASSERT_TRUE(dec.has_value());
  EXPECT_EQ(dec->head.packetId, msg.head.packetId);
  ASSERT_EQ(dec->data.topics.size(), msg.data.topics.size());
  EXPECT_EQ(dec->data.topics[0].filter, msg.data.topics[0].filter);
  EXPECT_EQ(dec->data.topics[0].qos, msg.data.topics[0].qos);
  EXPECT_EQ(dec->data.topics[1].filter, msg.data.topics[1].filter);
  EXPECT_EQ(dec->data.topics[1].qos, msg.data.topics[1].qos);
}

TEST_F(MessageTest, SubAckTest) {
  auto const msg = warp::mqtt::SubAck::Builder{}.withPacketId(21).addCode(0).addCode(1).build();
  auto dec = roundtrip(msg);
  if (!dec) {
    FAIL() << "roundtrip() returned std::nullopt";
    return;
  }
  ASSERT_TRUE(dec.has_value());
  EXPECT_EQ(dec->head.packetId, msg.head.packetId);
  ASSERT_EQ(dec->data.codes.size(), msg.data.codes.size());
  EXPECT_EQ(dec->data.codes[0], msg.data.codes[0]);
  EXPECT_EQ(dec->data.codes[1], msg.data.codes[1]);
}

TEST_F(MessageTest, UnsubscribeTest) {
  auto const msg = warp::mqtt::Unsubscribe::Builder{}
                       .withPacketId(33)
                       .addTopic("test/foo")
                       .addTopic("test/bar")
                       .build();
  auto dec = roundtrip(msg);
  if (!dec) {
    FAIL() << "roundtrip() returned std::nullopt";
    return;
  }
  ASSERT_TRUE(dec.has_value());
  EXPECT_EQ(dec->head.packetId, msg.head.packetId);
  ASSERT_EQ(dec->data.topics.size(), 2u);
  EXPECT_EQ(dec->data.topics[0], msg.data.topics[0]);
  EXPECT_EQ(dec->data.topics[1], msg.data.topics[1]);
}

TEST_F(MessageTest, UnsubAckTest) {
  auto const msg = warp::mqtt::UnsubAck::Builder{}.withPacketId(33).build();
  auto dec = roundtrip(msg);
  if (!dec) {
    FAIL() << "roundtrip() returned std::nullopt";
    return;
  }
  ASSERT_TRUE(dec.has_value());
  EXPECT_EQ(dec->head.packetId, msg.head.packetId);
}

TEST_F(MessageTest, PingReqTest) {
  auto const msg = warp::mqtt::PingReq::Builder{}.build();
  auto dec = roundtrip(msg);
  if (!dec) {
    FAIL() << "roundtrip() returned std::nullopt";
    return;
  }
  ASSERT_TRUE(dec.has_value());
}

TEST_F(MessageTest, PingRespTest) {
  auto const msg = warp::mqtt::PingResp::Builder{}.build();
  auto dec = roundtrip(msg);
  if (!dec) {
    FAIL() << "roundtrip() returned std::nullopt";
    return;
  }
  ASSERT_TRUE(dec.has_value());
}

TEST_F(MessageTest, DisconnectTest) {
  auto const msg = warp::mqtt::Disconnect::Builder{}.build();
  auto dec = roundtrip(msg);
  if (!dec) {
    FAIL() << "roundtrip() returned std::nullopt";
    return;
  }
  ASSERT_TRUE(dec.has_value());
}
