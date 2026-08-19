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
  ENVIRONMENT "TON_SIMPLEX_MAX_TPS=1;TON_SIMPLEX_MAX_TPS_CANDIDATE_TIMEOUT_MS=2000")
