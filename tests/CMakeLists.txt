find_package(GTest CONFIG REQUIRED)

add_executable(warp_tests
  mqtt/codec_test.cpp
  warp_test.cpp
)

target_link_libraries(warp_tests PRIVATE
  GTest::gtest_main
  warp::warp
)

include(GoogleTest)
gtest_discover_tests(warp_tests)
