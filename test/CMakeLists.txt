find_package(GTest REQUIRED CONFIG)
include(GoogleTest)

# The relay topology tests are Python (test/lib/moq_harness.py). 3.9 is the
# language floor, set by the macOS Command Line Tools' 3.9.6; this only asserts
# an interpreter at least that new exists. It cannot catch a 3.10-only construct
# in the harness — a version arg to find_package is a minimum, and no CI runner
# is old enough to trip over one. That needs a lint pinned to py39.
find_package(Python3 3.9 COMPONENTS Interpreter REQUIRED)

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

# Unit tests share a few binaries, grouped by the libraries they link. A static
# link includes only the objects a binary uses, so a group is about the size of
# its largest member. Under ASAN that is 400-600M for a test that reaches the
# relay stack.
#
# Add a new test file to the group whose libraries it needs, and add any missing
# library to that group's LIBS. Start a new binary only for a test that needs
# its own main or conflicting globals. Put a file-specific define on the source
# with set_source_files_properties. ctest runs each test case in its own process.

# In-process relay through MoqxRelayTestFixture.
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
    MoqxRelayStateTests.cpp
    MoqxRelayRendezvousTests.cpp
    MoqxRelayTestModes.cpp
    MoqxTrackFilterTest.cpp
    MoqxNamespaceTooLargeTest.cpp
    MoqxRelayContextTest.cpp
  LIBS moqx_test_fixture moqx_test_main
)

# Relays and clients over real sockets.
moqx_add_gtest(moqx_relay_integration_test
  SRCS
    MoqxRelayServerTest.cpp
    RelaySessionCloseTest.cpp
    UpstreamProviderTest.cpp
  LIBS
    moqx_core
    moqx_test_utils
    moqx_test_main
    GTest::gmock
    moxygen::moxygen_events_moq_folly_executor_impl
    moxygen::moxygen_moqclient
    moxygen::moxygen_util_quic_connector
    moxygen::moxygen_moq_webtransport_client
)
set_source_files_properties(UpstreamProviderTest.cpp PROPERTIES COMPILE_DEFINITIONS
  "TEST_CERT_PATH=\"${CMAKE_CURRENT_SOURCE_DIR}/test_cert.pem\";TEST_KEY_PATH=\"${CMAKE_CURRENT_SOURCE_DIR}/test_key.pem\""
)

# Relay components without a relay: trees, registries, filters, executor hops,
# auth and the cache.
moqx_add_gtest(moqx_relay_unit_test
  SRCS
    AuthTest.cpp
    AuthTokenIssuerTest.cpp
    CrossExecFilterTest.cpp
    CrossExecForwarderCallbackTest.cpp
    ForwarderRefTest.cpp
    InitialTrackStateTest.cpp
    LocalForwarderRegistryTest.cpp
    MoqxCacheTest.cpp
    NamespaceTreeTest.cpp
    PendingRendezvousTreeTest.cpp
    PropertyRankingBaseTest.cpp
    PropertyRankingSelfExclusionTest.cpp
    PublisherCrossExecFilterTest.cpp
    SafeTrackNameTest.cpp
    ServiceMatcherTest.cpp
    SubscriberCrossExecFilterTest.cpp
    SubscriptionRegistryTest.cpp
    TopNFilterTest.cpp
    TracksTreeTest.cpp
    relay/AuthFiltersTest.cpp
    relay/ExecBoundTest.cpp
    relay/RouteCodecTest.cpp
    relay/SessionParticipantTest.cpp
  LIBS
    moqx_core
    moqx_cache
    moqx_config
    moqx_issuer_lib
    moqx_test_utils
    moqx_test_main
    GTest::gmock
    moxygen::moqtest_utils
    Folly::folly_coro_gtest_helpers
    Folly::folly_executors_cpu_thread_pool_executor
    Folly::folly_executors_manual_executor
    Folly::folly_io_async_scoped_event_base_thread
)
target_include_directories(moqx_relay_unit_test PRIVATE ${PROJECT_SOURCE_DIR}/src)

moqx_add_gtest(moqx_admin_stats_test
  SRCS
    CborReaderTest.cpp
    QLogCaptureTest.cpp
    admin/StateResponseTest.cpp
    admin/StateStreamTest.cpp
    admin/TrackMetricsFormatTest.cpp
    stats/BoundedHistogramTest.cpp
    stats/MoQStatsCollectorTest.cpp
    stats/PicoQuicStatsCollectorTest.cpp
    stats/TrackStatsRegistryTest.cpp
    stats/TrackStatsTest.cpp
  LIBS
    moqx_core
    moqx_test_main
    GTest::gmock
    Folly::folly_executors_manual_executor
)
target_include_directories(moqx_admin_stats_test PRIVATE ${PROJECT_SOURCE_DIR}/src)

moqx_add_gtest(moqx_config_test
  SRCS
    config/ConfigResolverTest.cpp
    config/ConfigSerializerTest.cpp
    config/LoaderTest.cpp
    config/Pkcs12Test.cpp
  LIBS moqx_config_loader OpenSSL::Crypto moqx_test_main GTest::gmock
)
set_source_files_properties(config/LoaderTest.cpp PROPERTIES COMPILE_DEFINITIONS
  "CONFIG_EXAMPLE_PATH=\"${PROJECT_SOURCE_DIR}/config.example.yaml\""
)

moqx_add_gtest(moqx_logging_test
  SRCS
    LoggingMultiFlagTest.cpp
    logging/access/AccessLogTemplateTest.cpp
    logging/access/MoqxAccessLogTest.cpp
  LIBS moqx_core moqx_test_main
)

# Registers an integration test once per QUIC stack. The mvfst run keeps the
# plain name; the picoquic run gets a _picoquic suffix and selects the stack
# through MOQ_HARNESS_QUIC_STACK, which the Python harness and the shell tests
# both read. Each draft in MOQT_VERSIONS other than 16 adds a _d<N> variant
# that pins the clients through MOQ_HARNESS_MOQT_VERSION. All variants claim
# the same ports from test/test_ports.sh, so a RESOURCE_LOCK keeps ctest from
# running them at the same time.
function(moqx_add_dual_stack_test name)
  cmake_parse_arguments(ARG "" "TIMEOUT" "COMMAND;ENVIRONMENT;MOQT_VERSIONS" ${ARGN})
  if(NOT ARG_MOQT_VERSIONS)
    set(ARG_MOQT_VERSIONS 16)
  endif()
  foreach(version ${ARG_MOQT_VERSIONS})
    foreach(stack mvfst picoquic)
      set(test_name "${name}")
      if(NOT version STREQUAL "16")
        string(APPEND test_name "_d${version}")
      endif()
      if(NOT stack STREQUAL "mvfst")
        string(APPEND test_name "_${stack}")
      endif()
      add_test(NAME ${test_name} COMMAND ${ARG_COMMAND})
      set(test_env "MOQ_HARNESS_QUIC_STACK=${stack}" "MOQ_HARNESS_MOQT_VERSION=${version}")
      if(ARG_ENVIRONMENT)
        list(APPEND test_env ${ARG_ENVIRONMENT})
      endif()
      set_tests_properties(${test_name} PROPERTIES
        ENVIRONMENT "${test_env}"
        RESOURCE_LOCK "${name}_ports"
      )
      if(ARG_TIMEOUT)
        set_tests_properties(${test_name} PROPERTIES TIMEOUT ${ARG_TIMEOUT})
      endif()
    endforeach()
  endforeach()
endfunction()

moqx_add_dual_stack_test(relay_chain
  COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/test/test_relay_chain.py $<TARGET_FILE:moqx>
  TIMEOUT 120
  MOQT_VERSIONS 16 18
  ENVIRONMENT "MOQBIN=${MOXYGEN_BIN_DIR}"
)

moqx_add_dual_stack_test(relay_hops_cycle
  COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/test/test_relay_hops_cycle.py $<TARGET_FILE:moqx>
  TIMEOUT 90
  MOQT_VERSIONS 16 18
  ENVIRONMENT "MOQBIN=${MOXYGEN_BIN_DIR}"
)

moqx_add_dual_stack_test(relay_pyramid
  COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/test/test_relay_pyramid.py $<TARGET_FILE:moqx>
  TIMEOUT 120
  MOQT_VERSIONS 16 18
  ENVIRONMENT "MOQBIN=${MOXYGEN_BIN_DIR}"
)

moqx_add_dual_stack_test(relay_cluster_diamond
  COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/test/test_relay_cluster_diamond.py $<TARGET_FILE:moqx>
  TIMEOUT 120
  MOQT_VERSIONS 16 18
  ENVIRONMENT "MOQBIN=${MOXYGEN_BIN_DIR}"
)

moqx_add_dual_stack_test(relay_upstreams
  COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/test/test_relay_upstreams.py $<TARGET_FILE:moqx>
  TIMEOUT 90
  MOQT_VERSIONS 16 18
  ENVIRONMENT "MOQBIN=${MOXYGEN_BIN_DIR}"
)

moqx_add_dual_stack_test(relay_sighup
  COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/test/test_relay_sighup.py $<TARGET_FILE:moqx>
  TIMEOUT 60
  ENVIRONMENT "MOQBIN=${MOXYGEN_BIN_DIR}"
)

# QMUX is a TCP transport, not a QUIC stack, so this one sweeps drafts only.
foreach(version 16 18)
  set(test_name qmux_relay)
  if(NOT version STREQUAL "16")
    string(APPEND test_name "_d${version}")
  endif()
  add_test(
    NAME ${test_name}
    COMMAND bash ${PROJECT_SOURCE_DIR}/test/test_qmux_relay.sh $<TARGET_FILE:moqx>
  )
  set_tests_properties(${test_name} PROPERTIES
    TIMEOUT 60
    SKIP_RETURN_CODE 77
    ENVIRONMENT "MOQBIN=${MOXYGEN_BIN_DIR};MOQ_HARNESS_MOQT_VERSION=${version}"
    RESOURCE_LOCK qmux_relay_ports
  )
endforeach()

# Validates config files only; binds no port and runs no stack.
add_test(
  NAME validate_config
  COMMAND bash ${PROJECT_SOURCE_DIR}/test/test_validate_config.sh $<TARGET_FILE:moqx>
)

moqx_add_dual_stack_test(admin_info_endpoint
  COMMAND bash ${PROJECT_SOURCE_DIR}/test/test_admin_info.sh $<TARGET_FILE:moqx>
)
moqx_add_dual_stack_test(admin_tls_endpoint
  COMMAND bash ${PROJECT_SOURCE_DIR}/test/test_admin_tls.sh $<TARGET_FILE:moqx>
)
moqx_add_dual_stack_test(admin_metrics_endpoint
  COMMAND bash ${PROJECT_SOURCE_DIR}/test/test_admin_metrics.sh $<TARGET_FILE:moqx>
)
moqx_add_dual_stack_test(admin_track_metrics_endpoint
  COMMAND bash ${PROJECT_SOURCE_DIR}/test/test_admin_track_metrics.sh $<TARGET_FILE:moqx>
  MOQT_VERSIONS 16 18
  ENVIRONMENT "MOQBIN=${MOXYGEN_BIN_DIR}"
)
moqx_add_dual_stack_test(admin_cache_purge_endpoint
  COMMAND bash ${PROJECT_SOURCE_DIR}/test/test_admin_cache_purge.sh $<TARGET_FILE:moqx>
)
moqx_add_dual_stack_test(admin_config_endpoint
  COMMAND bash ${PROJECT_SOURCE_DIR}/test/test_admin_config.sh $<TARGET_FILE:moqx>
)
moqx_add_dual_stack_test(admin_state_endpoint
  COMMAND bash ${PROJECT_SOURCE_DIR}/test/test_admin_state.sh $<TARGET_FILE:moqx>
)
moqx_add_dual_stack_test(admin_connection_logs_endpoint
  COMMAND bash ${PROJECT_SOURCE_DIR}/test/test_admin_connection_logs.sh $<TARGET_FILE:moqx>
)
moqx_add_dual_stack_test(admin_cache_purge_concurrency_test
  COMMAND bash ${PROJECT_SOURCE_DIR}/test/test_admin_cache_purge_race.sh $<TARGET_FILE:moqx>
  MOQT_VERSIONS 16 18
  ENVIRONMENT "MOQBIN=${MOXYGEN_BIN_DIR}"
)

# TrackFilterLoadTest: requires a live relay process.
# Run manually: ./track_filter_load_test --relay_url=https://localhost:9668/moq-relay
# Build it on demand (`ninja track_filter_load_test`); the default build and
# ctest skip it.
add_executable(track_filter_load_test EXCLUDE_FROM_ALL
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
