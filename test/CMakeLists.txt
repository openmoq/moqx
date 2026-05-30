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

add_library(moqx_test_fixture STATIC
  MoqxRelayTestFixture.cpp
)
target_link_libraries(moqx_test_fixture PUBLIC
  moqx_core
  moqx_test_utils
  GTest::gmock
  moxygen::moxygen_events_moq_folly_executor_impl
)

add_executable(moqx_relay_test
  MoqxRelayPublishTests.cpp
  MoqxRelaySubscribeTests.cpp
  MoqxRelaySubNsTests.cpp
  MoqxRelayDataPlaneTests.cpp
  MoqxRelayFetchTests.cpp
  MoqxRelayTrackStatusTests.cpp
  MoqxRelayNGRTests.cpp
  MoqxRelayPeerTests.cpp
)
target_link_libraries(moqx_relay_test PRIVATE
  moqx_test_fixture
  moqx_test_main
)
gtest_discover_tests(moqx_relay_test)

add_executable(moqx_namespace_tree_test
  NamespaceTreeTest.cpp
)
target_link_libraries(moqx_namespace_tree_test PRIVATE
  moqx_core
  moqx_test_main
  GTest::gmock
)
gtest_discover_tests(moqx_namespace_tree_test)

add_executable(moqx_config_test
  config/LoaderTest.cpp
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
  # Force gflags's DEFINE_FLAG-registered .o files into the binary, even
  # though the test code doesn't reference gflags symbols directly. Cross-
  # platform raw linker flags — works with cmake 3.22, unlike the
  # $<LINK_LIBRARY:WHOLE_ARCHIVE,...> genex which needs cmake 3.24+.
  if(APPLE)
    target_link_libraries(moqx_config_test PRIVATE
      "-Wl,-force_load,$<TARGET_FILE:gflags_nothreads_static>"
    )
  else()
    target_link_libraries(moqx_config_test PRIVATE
      "-Wl,--whole-archive"
      gflags_nothreads_static
      "-Wl,--no-whole-archive"
    )
  endif()
endif()
gtest_discover_tests(moqx_config_test)

add_executable(moqx_config_resolver_test
  config/ConfigResolverTest.cpp
)
target_link_libraries(moqx_config_resolver_test PRIVATE
  moqx_config_loader
  GTest::gtest_main
  GTest::gmock
)
if(NOT GFLAGS_SHARED)
  if(APPLE)
    target_link_libraries(moqx_config_resolver_test PRIVATE
      "-Wl,-force_load,$<TARGET_FILE:gflags_nothreads_static>"
    )
  else()
    target_link_libraries(moqx_config_resolver_test PRIVATE
      "-Wl,--whole-archive"
      gflags_nothreads_static
      "-Wl,--no-whole-archive"
    )
  endif()
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

add_executable(moqx_auth_test
  AuthTest.cpp
)
target_link_libraries(moqx_auth_test PRIVATE
  moqx_core
  GTest::gtest_main
)
gtest_discover_tests(moqx_auth_test)

# CborReader unit tests (header-only decoder, tested in isolation)
add_executable(moqx_cbor_reader_test
  CborReaderTest.cpp
)
target_include_directories(moqx_cbor_reader_test PRIVATE
  ${PROJECT_SOURCE_DIR}/src
)
target_link_libraries(moqx_cbor_reader_test PRIVATE
  GTest::gtest_main
  GTest::gmock
)
gtest_discover_tests(moqx_cbor_reader_test)

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
add_test(
  NAME admin_cache_purge_endpoint
  COMMAND bash ${PROJECT_SOURCE_DIR}/test/test_admin_cache_purge.sh $<TARGET_FILE:moqx>
)
add_test(
  NAME admin_cache_purge_concurrency_test
  COMMAND bash ${PROJECT_SOURCE_DIR}/test/test_admin_cache_purge_race.sh $<TARGET_FILE:moqx>
)

add_executable(moqx_upstream_provider_test
  UpstreamProviderTest.cpp
)
target_link_libraries(moqx_upstream_provider_test PRIVATE
  moqx_core
  moqx_test_utils
  moqx_test_main
  moxygen::moxygen_events_moq_folly_executor_impl
  moxygen::moxygen_moqclient
  GTest::gmock
)
gtest_discover_tests(moqx_upstream_provider_test)

# SubscriptionRegistry unit tests
add_executable(moqx_subscription_registry_test
  SubscriptionRegistryTest.cpp
)
target_link_libraries(moqx_subscription_registry_test PRIVATE
  moqx_core
  moqx_test_main
  GTest::gmock
)
gtest_discover_tests(moqx_subscription_registry_test)

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

# PropertyRanking self-exclusion / waterline unit tests
add_executable(moqx_property_ranking_self_exclusion_test
  PropertyRankingSelfExclusionTest.cpp
)
target_link_libraries(moqx_property_ranking_self_exclusion_test PRIVATE
  moqx_core
  GTest::gtest_main
  GTest::gmock
)
gtest_discover_tests(moqx_property_ranking_self_exclusion_test)

# TRACK_FILTER integration tests (in-process relay, no live server)
add_executable(moqx_track_filter_test
  MoqxTrackFilterTest.cpp
)
target_link_libraries(moqx_track_filter_test PRIVATE
  moqx_test_fixture
  moqx_test_main
)
gtest_discover_tests(moqx_track_filter_test)

# TrackFilterLoadTest: requires a live relay process.
# Run manually: ./track_filter_load_test --relay_url=https://localhost:9668/moq-relay
# Not registered with gtest_discover_tests — excluded from automated ctest.
add_executable(track_filter_load_test
  TrackFilterLoadTest.cpp
)
target_link_libraries(track_filter_load_test PRIVATE
  moqx_core
  Folly::folly_init_init
  moxygen::moxygen_events_moq_folly_executor_impl
  moxygen::moxygen_moqclient
  moxygen::moxygen_moq_webtransport_client
  moxygen::moxygen_relay_moq_relay_client
  moxygen::moxygen_relay_moq_forwarder
  moxygen::moxygen_moq_relay_session
  moxygen::moxygen_util_insecure_verifier_dangerous_do_not_use_in_production
)

# CrossExecFilter unit tests
add_executable(moqx_cross_exec_filter_test
  CrossExecFilterTest.cpp
)
target_link_libraries(moqx_cross_exec_filter_test PRIVATE
  moqx_core
  GTest::gtest_main
  GTest::gmock
  moxygen::moqtest_utils
  Folly::folly_executors_manual_executor
)
gtest_discover_tests(moqx_cross_exec_filter_test)

# PublisherCrossExecFilter unit tests
add_executable(moqx_publisher_cross_exec_filter_test
  PublisherCrossExecFilterTest.cpp
)
target_link_libraries(moqx_publisher_cross_exec_filter_test PRIVATE
  moqx_core
  GTest::gtest_main
  GTest::gmock
  moxygen::moqtest_utils
  Folly::folly_executors_manual_executor
  Folly::folly_executors_cpu_thread_pool_executor
)
gtest_discover_tests(moqx_publisher_cross_exec_filter_test)

# SubscriberCrossExecFilter unit tests
add_executable(moqx_subscriber_cross_exec_filter_test
  SubscriberCrossExecFilterTest.cpp
)
target_link_libraries(moqx_subscriber_cross_exec_filter_test PRIVATE
  moqx_core
  GTest::gtest_main
  GTest::gmock
  moxygen::moqtest_utils
  Folly::folly_executors_manual_executor
  Folly::folly_executors_cpu_thread_pool_executor
)
gtest_discover_tests(moqx_subscriber_cross_exec_filter_test)

# --- MoqxCache tests ---

add_executable(moqx_cache_test
  MoqxCacheTest.cpp
)

target_link_libraries(moqx_cache_test PRIVATE
  moqx_cache
  moqx_test_utils
  moqx_test_main
  moxygen::moqtest_utils
  GTest::gmock
  Folly::folly_coro_gtest_helpers
  Folly::folly_io_async_scoped_event_base_thread
)

target_compile_options(moqx_cache_test PRIVATE
  -Wall -Wextra -Wpedantic
)

gtest_discover_tests(moqx_cache_test)
