find_package(GTest REQUIRED CONFIG)
include(GoogleTest)

# Discover tests at ctest startup (serial) instead of POST_BUILD (parallel).
# Parallel POST_BUILD discovery races on a shared JSON file under cmake 4.4.0,
# intermittently failing macOS builds with "JSON failed parsing".
set(CMAKE_GTEST_DISCOVER_TESTS_DISCOVERY_MODE PRE_TEST)

# moqx_add_gtest(<name> SRCS <sources...> LIBS <link targets...>)
# Per-test extras (compile definitions, include dirs) go on <name> after the call.
# moqx_warnings comes with it: test code is first-party code.
function(moqx_add_gtest name)
  cmake_parse_arguments(ARG "" "" "SRCS;LIBS" ${ARGN})
  add_executable(${name} ${ARG_SRCS})
  target_link_libraries(${name} PRIVATE ${ARG_LIBS} moqx_warnings)
  gtest_discover_tests(${name})
endfunction()

add_library(moqx_test_utils STATIC
  TestUtils.cpp
)
target_include_directories(moqx_test_utils PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(moqx_test_utils
  PUBLIC
    Folly::folly_io_iobuf
    Folly::folly_random
  PRIVATE
    moqx_warnings
)

add_library(moqx_test_main STATIC
  TestMain.cpp
)
target_link_libraries(moqx_test_main
  PUBLIC
    Folly::folly_init_init
    GTest::gtest
  PRIVATE
    moqx_warnings
)

add_library(moqx_test_fixture STATIC
  MoqxRelayTestFixture.cpp
)
target_link_libraries(moqx_test_fixture
  PUBLIC
    moqx_core
    moqx_test_utils
    GTest::gmock
    moxygen::moxygen_events_moq_folly_executor_impl
  PRIVATE
    moqx_warnings
)

moqx_add_gtest(moqx_relay_test
  SRCS
    MoqxRelayPublishTests.cpp
    MoqxRelaySubscribeTests.cpp
    MoqxRelaySubNsTests.cpp
    MoqxRelayDataPlaneTests.cpp
    MoqxRelayFetchTests.cpp
    MoqxRelayTrackStatusTests.cpp
    MoqxRelayNGRTests.cpp
    MoqxRelayPeerTests.cpp
    MoqxRelayTracksTests.cpp
    MoqxRelayTrackStatsTests.cpp
    MoqxRelayTestModes.cpp
  LIBS moqx_test_fixture moqx_test_main
)

moqx_add_gtest(moqx_namespace_tree_test
  SRCS NamespaceTreeTest.cpp
  LIBS moqx_core moqx_test_main GTest::gmock
)

moqx_add_gtest(moqx_config_test
  SRCS config/LoaderTest.cpp
  LIBS moqx_config_loader GTest::gtest_main GTest::gmock
)
target_compile_definitions(moqx_config_test PRIVATE
  CONFIG_EXAMPLE_PATH="${PROJECT_SOURCE_DIR}/config.example.yaml"
)

moqx_add_gtest(moqx_config_resolver_test
  SRCS config/ConfigResolverTest.cpp
  LIBS moqx_config_loader OpenSSL::Crypto GTest::gtest_main GTest::gmock
)

moqx_add_gtest(moqx_config_serializer_test
  SRCS config/ConfigSerializerTest.cpp
  LIBS moqx_config_loader GTest::gtest_main GTest::gmock
)

moqx_add_gtest(moqx_pkcs12_test
  SRCS config/Pkcs12Test.cpp
  LIBS moqx_config_loader OpenSSL::Crypto GTest::gtest_main GTest::gmock
)

# Header-only, tested in isolation — needs the src/ headers without moqx_core.
moqx_add_gtest(moqx_bounded_histogram_test
  SRCS stats/BoundedHistogramTest.cpp
  LIBS GTest::gtest_main GTest::gmock
)
target_include_directories(moqx_bounded_histogram_test PRIVATE
  ${PROJECT_SOURCE_DIR}/src
)

moqx_add_gtest(moqx_track_stats_test
  SRCS stats/TrackStatsTest.cpp
  LIBS moqx_core GTest::gtest_main GTest::gmock
)
target_include_directories(moqx_track_stats_test PRIVATE
  ${PROJECT_SOURCE_DIR}/src
)

moqx_add_gtest(moqx_track_metrics_format_test
  SRCS admin/TrackMetricsFormatTest.cpp
  LIBS moqx_core GTest::gtest_main GTest::gmock
)
target_include_directories(moqx_track_metrics_format_test PRIVATE
  ${PROJECT_SOURCE_DIR}/src
)

moqx_add_gtest(moqx_track_stats_registry_test
  SRCS stats/TrackStatsRegistryTest.cpp
  LIBS moqx_core GTest::gtest_main GTest::gmock
)
target_include_directories(moqx_track_stats_registry_test PRIVATE
  ${PROJECT_SOURCE_DIR}/src
)

moqx_add_gtest(moqx_logging_multi_flag_test
  SRCS LoggingMultiFlagTest.cpp
  LIBS moqx_core GTest::gtest_main GTest::gmock
)

moqx_add_gtest(moqx_pico_quic_stats_test
  SRCS stats/PicoQuicStatsCollectorTest.cpp
  LIBS moqx_core GTest::gtest_main GTest::gmock
)

moqx_add_gtest(moqx_moq_stats_collector_test
  SRCS stats/MoQStatsCollectorTest.cpp
  LIBS moqx_core GTest::gtest_main GTest::gmock
)

moqx_add_gtest(moqx_service_matcher_test
  SRCS ServiceMatcherTest.cpp
  LIBS moqx_core moqx_config GTest::gtest_main GTest::gmock
)

moqx_add_gtest(moqx_auth_test
  SRCS AuthTest.cpp
  LIBS moqx_core moqx_issuer_lib GTest::gtest_main
)

moqx_add_gtest(moqx_auth_token_issuer_test
  SRCS AuthTokenIssuerTest.cpp
  LIBS moqx_core moqx_issuer_lib GTest::gtest_main
)

moqx_add_gtest(moqx_auth_filters_test
  SRCS relay/AuthFiltersTest.cpp
  LIBS
    moqx_core
    moqx_issuer_lib
    GTest::gtest_main
    GTest::gmock
    moxygen::moqtest_utils
)

# Header-only decoder, tested in isolation — src/ headers without moqx_core.
moqx_add_gtest(moqx_cbor_reader_test
  SRCS CborReaderTest.cpp
  LIBS GTest::gtest_main GTest::gmock
)
target_include_directories(moqx_cbor_reader_test PRIVATE
  ${PROJECT_SOURCE_DIR}/src
)

moqx_add_gtest(moqx_safe_track_name_test
  SRCS SafeTrackNameTest.cpp
  LIBS moqx_core GTest::gtest_main
)
target_include_directories(moqx_safe_track_name_test PRIVATE
  ${PROJECT_SOURCE_DIR}/src
)

moqx_add_gtest(moqx_relay_context_test
  SRCS MoqxRelayContextTest.cpp
  LIBS
    moqx_core
    moqx_test_utils
    moqx_test_main
    GTest::gmock
    moxygen::moxygen_events_moq_folly_executor_impl
)

add_test(
  NAME relay_chain
  COMMAND bash ${PROJECT_SOURCE_DIR}/test/test_relay_chain.sh $<TARGET_FILE:moqx>
)
set_tests_properties(relay_chain PROPERTIES
  TIMEOUT 120
  ENVIRONMENT "MOQBIN=${MOXYGEN_BIN_DIR}"
)

add_test(
  NAME relay_hops_cycle
  COMMAND bash ${PROJECT_SOURCE_DIR}/test/test_relay_hops_cycle.sh $<TARGET_FILE:moqx>
)
set_tests_properties(relay_hops_cycle PROPERTIES
  TIMEOUT 90
  ENVIRONMENT "MOQBIN=${MOXYGEN_BIN_DIR}"
)

add_test(
  NAME qmux_relay
  COMMAND bash ${PROJECT_SOURCE_DIR}/test/test_qmux_relay.sh $<TARGET_FILE:moqx>
)
set_tests_properties(qmux_relay PROPERTIES
  TIMEOUT 60
  SKIP_RETURN_CODE 77
  ENVIRONMENT "MOQBIN=${MOXYGEN_BIN_DIR}"
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
  NAME admin_track_metrics_endpoint
  COMMAND bash ${PROJECT_SOURCE_DIR}/test/test_admin_track_metrics.sh $<TARGET_FILE:moqx>
)
set_tests_properties(admin_track_metrics_endpoint PROPERTIES
  ENVIRONMENT "MOQBIN=${MOXYGEN_BIN_DIR}"
)
add_test(
  NAME admin_cache_purge_endpoint
  COMMAND bash ${PROJECT_SOURCE_DIR}/test/test_admin_cache_purge.sh $<TARGET_FILE:moqx>
)
add_test(
  NAME admin_config_endpoint
  COMMAND bash ${PROJECT_SOURCE_DIR}/test/test_admin_config.sh $<TARGET_FILE:moqx>
)
add_test(
  NAME admin_cache_purge_concurrency_test
  COMMAND bash ${PROJECT_SOURCE_DIR}/test/test_admin_cache_purge_race.sh $<TARGET_FILE:moqx>
)
set_tests_properties(admin_cache_purge_concurrency_test PROPERTIES
  ENVIRONMENT "MOQBIN=${MOXYGEN_BIN_DIR}"
)

moqx_add_gtest(moqx_upstream_provider_test
  SRCS UpstreamProviderTest.cpp
  LIBS
    moqx_core
    moqx_test_utils
    moqx_test_main
    moxygen::moxygen_events_moq_folly_executor_impl
    moxygen::moxygen_moqclient
    GTest::gmock
)

moqx_add_gtest(moqx_subscription_registry_test
  SRCS SubscriptionRegistryTest.cpp
  LIBS moqx_core moqx_test_main GTest::gmock
)

moqx_add_gtest(moqx_initial_track_state_test
  SRCS InitialTrackStateTest.cpp
  LIBS moqx_core moqx_test_main GTest::gmock
)

moqx_add_gtest(moqx_local_forwarder_registry_test
  SRCS LocalForwarderRegistryTest.cpp
  LIBS moqx_core moqx_test_main GTest::gmock
)

moqx_add_gtest(moqx_topn_filter_test
  SRCS TopNFilterTest.cpp
  LIBS moqx_core GTest::gtest_main GTest::gmock
)

# PropertyRanking base (no self-exclusion) vs. self-exclusion/waterline split.
moqx_add_gtest(moqx_property_ranking_base_test
  SRCS PropertyRankingBaseTest.cpp
  LIBS moqx_core GTest::gtest_main GTest::gmock
)

moqx_add_gtest(moqx_property_ranking_self_exclusion_test
  SRCS PropertyRankingSelfExclusionTest.cpp
  LIBS moqx_core GTest::gtest_main GTest::gmock
)

# TRACK_FILTER integration tests (in-process relay, no live server)
moqx_add_gtest(moqx_track_filter_test
  SRCS MoqxTrackFilterTest.cpp
  LIBS moqx_test_fixture moqx_test_main
)

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

moqx_add_gtest(moqx_cross_exec_filter_test
  SRCS CrossExecFilterTest.cpp
  LIBS
    moqx_core
    GTest::gtest_main
    GTest::gmock
    moxygen::moqtest_utils
    Folly::folly_executors_manual_executor
)

moqx_add_gtest(moqx_publisher_cross_exec_filter_test
  SRCS PublisherCrossExecFilterTest.cpp
  LIBS
    moqx_core
    GTest::gtest_main
    GTest::gmock
    moxygen::moqtest_utils
    Folly::folly_executors_manual_executor
    Folly::folly_executors_cpu_thread_pool_executor
)

moqx_add_gtest(moqx_subscriber_cross_exec_filter_test
  SRCS SubscriberCrossExecFilterTest.cpp
  LIBS
    moqx_core
    GTest::gtest_main
    GTest::gmock
    moxygen::moqtest_utils
    Folly::folly_executors_manual_executor
    Folly::folly_executors_cpu_thread_pool_executor
)

moqx_add_gtest(moqx_cache_test
  SRCS MoqxCacheTest.cpp
  LIBS
    moqx_cache
    moqx_test_utils
    moqx_test_main
    moxygen::moqtest_utils
    GTest::gmock
    Folly::folly_coro_gtest_helpers
    Folly::folly_io_async_scoped_event_base_thread
)
