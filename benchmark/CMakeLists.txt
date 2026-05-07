# folly Benchmark is built as part of the folly standalone/FetchContent
# pulled in by moxygen's standalone/CMakeLists.txt — no extra fetch.

add_executable(moqx_benchmark
  bounded_histogram.cpp
  config_loader.cpp
  config_resolver.cpp
  moq_stats_collector.cpp
  moqt_extensions.cpp
  moqt_framer.cpp
  prometheus_format.cpp
  service_matcher.cpp
  stats_registry.cpp
  benchmark_main.cpp
)

target_link_libraries(moqx_benchmark PRIVATE
  moqx_core
  moqx_config_loader
  Folly::folly
  Folly::follybenchmark
)

target_compile_options(moqx_benchmark PRIVATE -Wall -Wextra -Wpedantic)
