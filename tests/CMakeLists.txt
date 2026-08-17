find_package(Catch2 3 QUIET)
if(NOT Catch2_FOUND)
  find_package(Catch2 REQUIRED)
endif()

if(TARGET Catch2::Catch2)
  include(Catch)
endif()

add_executable(holder_core_tests
  AiMessageFrontMatter_test.cpp
  AiMessagePaths_test.cpp
  CardFrontMatter_test.cpp
  CardPaths_test.cpp
  PrivacyCryptoService_test.cpp
  PrivacyError_test.cpp
  PushResult_test.cpp
  RemoteProbe_test.cpp
)

target_link_libraries(holder_core_tests PRIVATE
  Holder::Core
)

if(TARGET Catch2::Catch2)
  target_sources(holder_core_tests PRIVATE catch_main.cpp)
  target_link_libraries(holder_core_tests PRIVATE Catch2::Catch2)
elseif(TARGET Catch2::Catch2WithMain)
  target_link_libraries(holder_core_tests PRIVATE Catch2::Catch2WithMain)
endif()

set(HOLDER_CORE_TEST_ENVIRONMENT
  "HOLDER_TEST_KEYSTORE_DIR=${CMAKE_CURRENT_BINARY_DIR}/test-keystore"
)

if(COMMAND catch_discover_tests)
  catch_discover_tests(holder_core_tests
    PROPERTIES
      ENVIRONMENT "${HOLDER_CORE_TEST_ENVIRONMENT}"
  )
else()
  add_test(NAME holder_core_tests COMMAND holder_core_tests)
  set_tests_properties(holder_core_tests PROPERTIES ENVIRONMENT "${HOLDER_CORE_TEST_ENVIRONMENT}")
endif()
