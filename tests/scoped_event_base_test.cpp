// scoped_event_base_test.cpp

#include <gtest/gtest.h>
#include <warp/scoped_event_base.h>

#include <csignal>

TEST(ScopedEventBaseTest, HandlesSIGINT) {
  int received = 0;

  warp::ScopedEventBase base([&](int signum) { received = signum; });

  ASSERT_EQ(std::raise(SIGINT), 0);

  base.run();

  EXPECT_EQ(received, SIGINT);
}

TEST(ScopedEventBaseTest, HandlesCustom) {
  int received = 0;

  warp::ScopedEventBase base([&](int signum) { received = signum; }, {SIGUSR1});

  ASSERT_EQ(std::raise(SIGUSR1), 0);

  base.run();

  EXPECT_EQ(received, SIGUSR1);
}
