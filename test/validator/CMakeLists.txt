add_subdirectory(consensus)

add_executable(test-native-admission-batch test-native-admission-batch.cpp)
target_include_directories(test-native-admission-batch PRIVATE ${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/crypto)
target_link_libraries(test-native-admission-batch PRIVATE td-test-main tdutils ton_validator)
ton_test(test-native-admission-batch)

add_executable(test-native-admission-refresh test-native-admission-refresh.cpp)
target_include_directories(test-native-admission-refresh PRIVATE ${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/crypto)
target_link_libraries(test-native-admission-refresh PRIVATE td-test-main tdutils)
ton_test(test-native-admission-refresh)

add_executable(test-native-work-scratch test-native-work-scratch.cpp)
target_include_directories(test-native-work-scratch PRIVATE ${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/crypto)
target_link_libraries(test-native-work-scratch PRIVATE td-test-main tdutils ton_crypto_core)
ton_test(test-native-work-scratch)

add_executable(test-external-message-routing test-external-message-routing.cpp)
target_include_directories(test-external-message-routing PRIVATE ${CMAKE_SOURCE_DIR} ${CMAKE_BINARY_DIR}
                                                             ${CMAKE_SOURCE_DIR}/common
                                                             ${CMAKE_SOURCE_DIR}/crypto
                                                             ${CMAKE_SOURCE_DIR}/tdactor
                                                             ${CMAKE_SOURCE_DIR}/tddb
                                                             ${CMAKE_SOURCE_DIR}/tdnet
                                                             ${CMAKE_SOURCE_DIR}/tdfec
                                                             ${CMAKE_SOURCE_DIR}/tl
                                                             ${CMAKE_SOURCE_DIR}/tl/generate
                                                             ${CMAKE_SOURCE_DIR}/third-party/rocksdb/include)
target_link_libraries(test-external-message-routing PRIVATE td-test-main tdutils ton_validator)
ton_test(test-external-message-routing)

add_executable(test-collator-external-wait-stats test-collator-external-wait-stats.cpp)
target_include_directories(test-collator-external-wait-stats PRIVATE ${CMAKE_SOURCE_DIR} ${CMAKE_BINARY_DIR}
                                                                    ${CMAKE_SOURCE_DIR}/common
                                                                    ${CMAKE_SOURCE_DIR}/crypto
                                                                    ${CMAKE_SOURCE_DIR}/tdactor
                                                                    ${CMAKE_SOURCE_DIR}/tddb
                                                                    ${CMAKE_SOURCE_DIR}/tdnet
                                                                    ${CMAKE_SOURCE_DIR}/tdfec
                                                                    ${CMAKE_SOURCE_DIR}/tl
                                                                    ${CMAKE_SOURCE_DIR}/tl/generate
                                                                    ${CMAKE_SOURCE_DIR}/third-party/rocksdb/include)
target_link_libraries(test-collator-external-wait-stats PRIVATE td-test-main tdutils ton_validator)
ton_test(test-collator-external-wait-stats)

add_executable(test-ext-message-pool-scheduler test-ext-message-pool-scheduler.cpp)
target_include_directories(test-ext-message-pool-scheduler PRIVATE ${CMAKE_SOURCE_DIR} ${CMAKE_BINARY_DIR}
                                                                   ${CMAKE_SOURCE_DIR}/common
                                                                   ${CMAKE_SOURCE_DIR}/crypto
                                                                   ${CMAKE_SOURCE_DIR}/tdactor
                                                                   ${CMAKE_SOURCE_DIR}/tddb
                                                                   ${CMAKE_SOURCE_DIR}/tdnet
                                                                   ${CMAKE_SOURCE_DIR}/tdfec
                                                                   ${CMAKE_SOURCE_DIR}/tl
                                                                   ${CMAKE_SOURCE_DIR}/tl/generate
                                                                   ${CMAKE_SOURCE_DIR}/third-party/rocksdb/include)
target_link_libraries(test-ext-message-pool-scheduler PRIVATE td-test-main tdutils ton_validator validator)
ton_test(test-ext-message-pool-scheduler)
