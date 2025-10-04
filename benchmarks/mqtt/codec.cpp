#include "warp/mqtt/codec.h"

#include <benchmark/benchmark.h>

class CodecTest : public benchmark::Fixture {
public:
  void SetUp(const ::benchmark::State&) override {
    connect_ = warp::mqtt::Connect::Builder{}
                   .withLevel(warp::mqtt::Level::V311)
                   .withCleanSession(true)
                   .withKeepAlive(60)
                   .withClient("CLIENT")
                   .build();
  }

  void TearDown(const ::benchmark::State&) override {
    // empty
  }

protected:
  warp::mqtt::Connect connect_;
};

BENCHMARK_F(CodecTest, EncodeTest)(benchmark::State& state) {
  for (auto _ : state) {
    auto data = warp::mqtt::Codec::encode(connect_);
    benchmark::DoNotOptimize(data);
    benchmark::ClobberMemory();
  }
}
