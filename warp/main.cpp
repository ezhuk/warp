// main.cpp

#include <folly/init/Init.h>
#include <folly/logging/Init.h>
#include <folly/logging/xlog.h>
#include <folly/portability/GFlags.h>

#include <string>

#include "scoped_event_base.h"

FOLLY_GFLAGS_DEFINE_int32(mqtt_port, 1883, "MQTT port to listen on");
FOLLY_GFLAGS_DEFINE_int32(http_port, 8080, "HTTP port to listen on");
FOLLY_GFLAGS_DEFINE_string(host, "0.0.0.0", "Host IP address to bind to");

FOLLY_INIT_LOGGING_CONFIG("INFO; default=stream:stream=stdout");

int main(int argc, char* argv[]) {
  folly::gflags::SetUsageMessage("\nUsage: " + std::string(argv[0]) + " [OPTIONS]\n");
  folly::gflags::SetVersionString(WARP_VERSION);

  auto _ = folly::Init(&argc, &argv);
  XLOGF(INFO, "STARTED v{} {}", WARP_VERSION, FLAGS_mqtt_port);

  warp::ScopedEventBase base([](int signum) { XLOGF(INFO, "Received signal {}", signum); });
  base.run();

  XLOG(INFO, "STOPPED");
  return 0;
}
