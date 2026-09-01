add_library(td-test-main test-td-main.cpp)
target_link_libraries(td-test-main PRIVATE tdutils)

add_executable(test-overlay-traffic-yield-policy test-overlay-traffic-yield-policy.cpp)
target_include_directories(test-overlay-traffic-yield-policy PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(test-overlay-traffic-yield-policy PRIVATE td-test-main tdutils)
ton_test(test-overlay-traffic-yield-policy)

add_subdirectory(tdactor)
add_subdirectory(tdutils)
add_subdirectory(tonlib)
add_subdirectory(validator)
