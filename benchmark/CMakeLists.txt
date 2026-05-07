# folly Benchmark is built as part of the folly standalone/FetchContent
# pulled in by moxygen's standalone/CMakeLists.txt — no extra fetch.

add_executable(moqx_benchmark
  BoundedHistogram.cpp
  MoQStatsCollector.cpp
  MoQTExtensions.cpp
  MoQFramer.cpp
  PrometheusFormat.cpp
  ServiceMatcher.cpp
  StatsRegistry.cpp
  BenchmarkMain.cpp
)

target_link_libraries(moqx_benchmark PRIVATE
  moqx_core
  moqx_config_loader
  Folly::folly
  Folly::follybenchmark
)

target_compile_options(moqx_benchmark PRIVATE -Wall -Wextra -Wpedantic)
