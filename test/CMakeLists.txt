find_package(GTest REQUIRED CONFIG)
include(GoogleTest)

add_library(moqx_test_utils STATIC
  TestUtils.cpp
)
target_include_directories(moqx_test_utils PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(moqx_test_utils PUBLIC
  Folly::folly_io_iobuf
  Folly::folly_random
)

add_library(moqx_test_main STATIC
  TestMain.cpp
)
target_link_libraries(moqx_test_main PUBLIC
  Folly::folly_init_init
  GTest::gtest
)

add_executable(moqx_relay_test
  MoqxRelayTest.cpp
)
target_link_libraries(moqx_relay_test PRIVATE
  moqx_core
  moqx_test_utils
  moqx_test_main
  GTest::gmock
  moxygen::moxygen_events_moq_folly_executor_impl
)
gtest_discover_tests(moqx_relay_test)

add_executable(moqx_config_test
  config/loader_test.cpp
)
target_link_libraries(moqx_config_test PRIVATE
  moqx_config_loader
  GTest::gtest_main
  GTest::gmock
)
target_compile_definitions(moqx_config_test PRIVATE
  CONFIG_EXAMPLE_PATH="${PROJECT_SOURCE_DIR}/config.example.yaml"
)
if(NOT GFLAGS_SHARED)
  target_link_libraries(moqx_config_test PRIVATE
    "$<LINK_LIBRARY:WHOLE_ARCHIVE,gflags_nothreads_static>"
  )
  set_property(TARGET moqx_config_test PROPERTY
    LINK_LIBRARY_OVERRIDE "WHOLE_ARCHIVE,gflags_nothreads_static"
  )
endif()
gtest_discover_tests(moqx_config_test)

add_executable(moqx_config_resolver_test
  config/config_resolver_test.cpp
)
target_link_libraries(moqx_config_resolver_test PRIVATE
  moqx_config_loader
  GTest::gtest_main
  GTest::gmock
)
if(NOT GFLAGS_SHARED)
  target_link_libraries(moqx_config_resolver_test PRIVATE
    "$<LINK_LIBRARY:WHOLE_ARCHIVE,gflags_nothreads_static>"
  )
  set_property(TARGET moqx_config_resolver_test PROPERTY
    LINK_LIBRARY_OVERRIDE "WHOLE_ARCHIVE,gflags_nothreads_static"
  )
endif()
gtest_discover_tests(moqx_config_resolver_test)

add_executable(moqx_bounded_histogram_test
  stats/BoundedHistogramTest.cpp
)
target_include_directories(moqx_bounded_histogram_test PRIVATE
  ${PROJECT_SOURCE_DIR}/src
)
target_link_libraries(moqx_bounded_histogram_test PRIVATE
  GTest::gtest_main
  GTest::gmock
)
gtest_discover_tests(moqx_bounded_histogram_test)

add_executable(moqx_pico_quic_stats_test
  stats/PicoQuicStatsCollectorTest.cpp
)
target_include_directories(moqx_pico_quic_stats_test PRIVATE
  ${PROJECT_SOURCE_DIR}/src
)
target_link_libraries(moqx_pico_quic_stats_test PRIVATE
  moqx_core
  GTest::gtest_main
  GTest::gmock
)
gtest_discover_tests(moqx_pico_quic_stats_test)

add_executable(moqx_service_matcher_test
  ServiceMatcherTest.cpp
)
target_link_libraries(moqx_service_matcher_test PRIVATE
  moqx_core
  moqx_config
  GTest::gtest_main
  GTest::gmock
)
gtest_discover_tests(moqx_service_matcher_test)

add_executable(moqx_relay_context_test
  MoqxRelayContextTest.cpp
)
target_link_libraries(moqx_relay_context_test PRIVATE
  moqx_core
  moqx_test_utils
  moqx_test_main
  GTest::gmock
  moxygen::moxygen_events_moq_folly_executor_impl
)
gtest_discover_tests(moqx_relay_context_test)

add_test(
  NAME relay_chain
  COMMAND bash ${PROJECT_SOURCE_DIR}/test/test_relay_chain.sh $<TARGET_FILE:moqx>
)
set_tests_properties(relay_chain PROPERTIES
  TIMEOUT 120
  ENVIRONMENT "MOQBIN=${PROJECT_SOURCE_DIR}/.scratch/moxygen-install/bin"
)

add_test(
  NAME admin_info_endpoint
  COMMAND bash ${PROJECT_SOURCE_DIR}/test/test_admin_info.sh $<TARGET_FILE:moqx>
)
add_test(
  NAME admin_tls_endpoint
  COMMAND bash ${PROJECT_SOURCE_DIR}/test/test_admin_tls.sh $<TARGET_FILE:moqx>
)
add_test(
  NAME admin_metrics_endpoint
  COMMAND bash ${PROJECT_SOURCE_DIR}/test/test_admin_metrics.sh $<TARGET_FILE:moqx>
)

add_executable(moqx_upstream_provider_test
  UpstreamProviderTest.cpp
)
target_link_libraries(moqx_upstream_provider_test PRIVATE
  moqx_core
  moqx_test_utils
  moqx_test_main
  moxygen::moqtest_utils
  moxygen::moxygen_events_moq_folly_executor_impl
  moxygen::moxygen_moqclient
  GTest::gmock
)
gtest_discover_tests(moqx_upstream_provider_test)

# TopNFilter unit tests
add_executable(moqx_topn_filter_test
  TopNFilterTest.cpp
)
target_link_libraries(moqx_topn_filter_test PRIVATE
  moqx_core
  GTest::gtest_main
  GTest::gmock
)
gtest_discover_tests(moqx_topn_filter_test)

# PropertyRanking base unit tests (no self-exclusion)
add_executable(moqx_property_ranking_base_test
  PropertyRankingBaseTest.cpp
)
target_link_libraries(moqx_property_ranking_base_test PRIVATE
  moqx_core
  GTest::gtest_main
  GTest::gmock
)
gtest_discover_tests(moqx_property_ranking_base_test)

# TRACK_FILTER integration tests (in-process relay, no live server)
add_executable(moqx_track_filter_test
  MoqxTrackFilterTest.cpp
)
target_link_libraries(moqx_track_filter_test PRIVATE
  moqx_core
  moqx_test_main
  GTest::gmock
  moxygen::moxygen_events_moq_folly_executor_impl
)
gtest_discover_tests(moqx_track_filter_test)
