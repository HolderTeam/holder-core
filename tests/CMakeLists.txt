find_package(Catch2 3 QUIET)
if(NOT Catch2_FOUND)
  find_package(Catch2 REQUIRED)
endif()

if(TARGET Catch2::Catch2)
  include(Catch)
endif()

add_executable(holder_core_tests
  AiMessageRepo_test.cpp
  AiMessageFrontMatter_test.cpp
  AiMessagePaths_test.cpp
  AiProviderCredentialRepo_test.cpp
  AiProviderSettingRepo_test.cpp
  AiRunRepo_test.cpp
  AiRunnerRepo_test.cpp
  AiThreadRepo_test.cpp
  CApi_test.cpp
  CardFrontMatter_test.cpp
  CardPaths_test.cpp
  CardRepo_test.cpp
  CardStore_test.cpp
  CardStoreFsFailure_test.cpp
  Cascade_test.cpp
  DefaultProject_test.cpp
  Db_test.cpp
  FsAdapterFailure_test.cpp
  FtsIndexer_test.cpp
  GitAdapterFailure_test.cpp
  GitOps_test.cpp
  GitRepo_test.cpp
  LinkRepo_test.cpp
  Migrations_test.cpp
  PrivacyCryptoService_test.cpp
  PrivacyError_test.cpp
  ProjectPaths_test.cpp
  ProjectPrivacy_test.cpp
  ProjectRepo_test.cpp
  ProjectStore_test.cpp
  ProjectSyncPolicy_test.cpp
  ProjectSyncRepo_test.cpp
  PushResult_test.cpp
  RealFs_test.cpp
  Reindexer_test.cpp
  RemoteProbe_test.cpp
  RepoSyncMetrics_test.cpp
  ResourceRepo_test.cpp
  SecretStore_test.cpp
  StartupRecovery_test.cpp
)

target_link_libraries(holder_core_tests PRIVATE
  Holder::Core
)

target_compile_definitions(holder_core_tests PRIVATE
  EXPECTED_HOLDER_VERSION_STRING="${PROJECT_VERSION}"
  EXPECTED_HOLDER_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
  EXPECTED_HOLDER_VERSION_MINOR=${PROJECT_VERSION_MINOR}
  EXPECTED_HOLDER_VERSION_PATCH=${PROJECT_VERSION_PATCH}
  SCHEMA_SQL_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../schema/schema.sql"
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
