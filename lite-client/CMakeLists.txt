add_library(lite-client-common STATIC lite-client-common.cpp lite-client-common.h ext-client.cpp ext-client.h
  query-utils.hpp query-utils.cpp)
target_link_libraries(lite-client-common PUBLIC tdactor adnllite tl_api tl_lite_api tl-lite-utils ton_crypto)

add_executable(lite-client lite-client.cpp lite-client.h ext-client.h ext-client.cpp)
target_link_libraries(lite-client tdutils tdactor adnllite tl_api tl_lite_api tl-lite-utils terminal lite-client-common git)

add_executable(native-load-generator native-load-generator.cpp)
target_link_libraries(native-load-generator tdutils tdactor adnllite tl_api tl_lite_api tl-lite-utils lite-client-common git)

if (NOT CMAKE_CROSSCOMPILING)
  add_executable(test-lite-client-query-utils ${CMAKE_SOURCE_DIR}/test/test-td-main.cpp query-utils-test.cpp)
  target_link_libraries(test-lite-client-query-utils PRIVATE lite-client-common ton_crypto)
  ton_test(test-lite-client-query-utils)
endif()

install(TARGETS lite-client native-load-generator RUNTIME DESTINATION bin)
