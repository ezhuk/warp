find_package(benchmark CONFIG REQUIRED)

add_executable(warp_bench
  mqtt/codec.cpp
  main.cpp
)

target_link_libraries(warp_bench PRIVATE
  benchmark::benchmark
  warp::warp
)
