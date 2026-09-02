add_executable(test-consensus test-consensus.cpp)
target_link_libraries(test-consensus PRIVATE
    adnl
    adnltest
    dht
    overlay
    rldp
    tdactor
    tdutils
    tl_api
    ton_validator
    validator
)

add_test(NAME consensus-max-tps-masterchain-paced
  COMMAND test-consensus --masterchain --n-nodes 1 --duration 1.5 --target-rate-ms 250 --expect-paced)
set_tests_properties(consensus-max-tps-masterchain-paced PROPERTIES
  ENVIRONMENT "TON_SIMPLEX_MAX_TPS=1")

add_test(NAME consensus-max-tps-shardchain-work-driven
  COMMAND test-consensus --n-nodes 1 --duration 0.75 --target-rate-ms 1000 --expect-work-driven)
set_tests_properties(consensus-max-tps-shardchain-work-driven PROPERTIES
  ENVIRONMENT "TON_SIMPLEX_MAX_TPS=1")

add_test(NAME consensus-max-tps-shardchain-idle
  COMMAND test-consensus --n-nodes 1 --duration 4.6 --target-rate-ms 1000 --idle-after-first)
set_tests_properties(consensus-max-tps-shardchain-idle PROPERTIES
  ENVIRONMENT "TON_SIMPLEX_MAX_TPS=1;TON_SIMPLEX_MAX_TPS_CANDIDATE_TIMEOUT_MS=2000;TON_SIMPLEX_MAX_TPS_FINALIZE_RESERVE_MS=250")

add_test(NAME consensus-cancelled-accept-skips-external-tracking
  COMMAND test-consensus --n-nodes 1 --duration 0.75 --target-rate-ms 100 --cancel-block-accepts)

add_executable(test-finalize-metadata test-finalize-metadata.cpp)
target_include_directories(test-finalize-metadata PRIVATE ${CMAKE_SOURCE_DIR} ${CMAKE_BINARY_DIR}
                                                            ${CMAKE_SOURCE_DIR}/common
                                                            ${CMAKE_SOURCE_DIR}/crypto
                                                            ${CMAKE_SOURCE_DIR}/tdactor
                                                            ${CMAKE_SOURCE_DIR}/tddb)
target_link_libraries(test-finalize-metadata PRIVATE td-test-main tdactor tdutils tl_api ton_validator validator)
ton_test(test-finalize-metadata)
