find_package(benchmark QUIET)
if(NOT benchmark_FOUND)
  set(BENCHMARK_USE_BUNDLED_GTEST OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_INSTALL_DOCS OFF CACHE BOOL "" FORCE)

  include(FetchContent)
  FetchContent_Declare(
    benchmark
    GIT_REPOSITORY https://github.com/google/benchmark.git
    GIT_TAG v1.9.1
    GIT_SHALLOW ON
  )
  FetchContent_MakeAvailable(benchmark)
endif()

add_executable(moqx_benchmark
  bounded_histogram.cpp
  config_loader.cpp
  config_resolver.cpp
  moq_stats_collector.cpp
  moqt_framer.cpp
  prometheus_format.cpp
  service_matcher.cpp
  stats_registry.cpp
)

target_link_libraries(moqx_benchmark PRIVATE
  moqx_core
  moqx_config_loader
  benchmark::benchmark_main
)

target_compile_options(moqx_benchmark PRIVATE -Wall -Wextra -Wpedantic)
