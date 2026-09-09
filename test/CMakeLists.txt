add_library(td-test-main test-td-main.cpp)
target_link_libraries(td-test-main PRIVATE tdutils)

add_executable(test-native-parent-metadata test-native-parent-metadata.cpp)
target_include_directories(test-native-parent-metadata PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(test-native-parent-metadata PRIVATE td-test-main ton_crypto)
ton_test(test-native-parent-metadata)

add_executable(bench-native-parent-metadata EXCLUDE_FROM_ALL bench-native-parent-metadata.cpp)
target_include_directories(bench-native-parent-metadata PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(bench-native-parent-metadata PRIVATE ton_crypto)

add_executable(test-overlay-traffic-yield-policy test-overlay-traffic-yield-policy.cpp)
target_include_directories(test-overlay-traffic-yield-policy PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(test-overlay-traffic-yield-policy PRIVATE td-test-main tdutils)
ton_test(test-overlay-traffic-yield-policy)

add_executable(test-keyring-prepared-signing test-keyring-prepared-signing.cpp)
target_include_directories(test-keyring-prepared-signing PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(test-keyring-prepared-signing PRIVATE td-test-main keyring keys tdactor)
ton_test(test-keyring-prepared-signing)

add_executable(test-overlay-local-signature test-overlay-local-signature.cpp)
target_include_directories(test-overlay-local-signature PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(test-overlay-local-signature PRIVATE td-test-main overlay keyring keys tdactor tdfec)
ton_test(test-overlay-local-signature)

add_subdirectory(tdactor)
add_subdirectory(tdutils)
add_subdirectory(tonlib)
add_subdirectory(validator)
