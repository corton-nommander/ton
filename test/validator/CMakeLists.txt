add_subdirectory(consensus)

add_executable(test-native-admission-batch test-native-admission-batch.cpp)
target_include_directories(test-native-admission-batch PRIVATE ${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/crypto)
target_link_libraries(test-native-admission-batch PRIVATE td-test-main tdutils ton_validator)
ton_test(test-native-admission-batch)
