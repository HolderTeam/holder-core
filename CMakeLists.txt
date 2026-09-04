cmake_minimum_required(VERSION 3.22)

include(cmake/HolderVersion.cmake)

project(holder VERSION ${HOLDER_CORE_CMAKE_VERSION} LANGUAGES CXX)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(CTest)

option(HOLDER_CORE_WARNINGS_AS_ERRORS "Treat Holder core warnings as errors" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(WIN32)
  add_compile_definitions(
    _WIN32_WINNT=0x0A00
    WINVER=0x0A00
  )
endif()

if(WIN32 OR ANDROID)
  set(HOLDER_CORE_USE_PKG_CONFIG_DEPS OFF)
else()
  set(HOLDER_CORE_USE_PKG_CONFIG_DEPS ON)
endif()

if(HOLDER_CORE_USE_PKG_CONFIG_DEPS)
  find_package(PkgConfig REQUIRED)
endif()

find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)
find_package(nlohmann_json 3.6.0 REQUIRED)
find_package(yaml-cpp REQUIRED)
find_package(spdlog REQUIRED)

if(TARGET yaml-cpp::yaml-cpp)
  set(HOLDER_CORE_YAML_CPP_TARGET yaml-cpp::yaml-cpp)
else()
  set(HOLDER_CORE_YAML_CPP_TARGET yaml-cpp)
endif()

if(WIN32 OR ANDROID)
  find_package(unofficial-sqlite3 CONFIG QUIET)
  if(TARGET unofficial::sqlite3::sqlite3)
    set(HOLDER_CORE_SQLITE3_TARGET unofficial::sqlite3::sqlite3)
  else()
    find_package(SQLite3 REQUIRED)
    set(HOLDER_CORE_SQLITE3_TARGET SQLite::SQLite3)
  endif()

  find_package(unofficial-sodium CONFIG REQUIRED)
  if(TARGET unofficial-sodium::sodium)
    set(HOLDER_CORE_SODIUM_TARGET unofficial-sodium::sodium)
  else()
    message(FATAL_ERROR "unofficial-sodium package did not provide unofficial-sodium::sodium")
  endif()

  find_package(unofficial-libgit2 CONFIG QUIET)
  find_package(libgit2 CONFIG QUIET)
  if(TARGET unofficial::libgit2::libgit2)
    set(HOLDER_CORE_LIBGIT2_TARGET unofficial::libgit2::libgit2)
  elseif(TARGET libgit2::libgit2package)
    set(HOLDER_CORE_LIBGIT2_TARGET libgit2::libgit2package)
  elseif(TARGET libgit2::libgit2)
    set(HOLDER_CORE_LIBGIT2_TARGET libgit2::libgit2)
  elseif(TARGET libgit2package)
    set(HOLDER_CORE_LIBGIT2_TARGET libgit2package)
  elseif(TARGET libgit2)
    set(HOLDER_CORE_LIBGIT2_TARGET libgit2)
  else()
    message(FATAL_ERROR "Could not find a usable libgit2 CMake target")
  endif()

  find_package(md4c CONFIG REQUIRED)
  set(HOLDER_CORE_MD4C_TARGET md4c::md4c)
else()
  pkg_check_modules(SQLITE3 REQUIRED IMPORTED_TARGET sqlite3)
  pkg_check_modules(SODIUM REQUIRED IMPORTED_TARGET libsodium)
  pkg_check_modules(LIBGIT2 REQUIRED IMPORTED_TARGET libgit2)
  pkg_check_modules(MD4C REQUIRED IMPORTED_TARGET md4c)
  set(HOLDER_CORE_SQLITE3_TARGET PkgConfig::SQLITE3)
  set(HOLDER_CORE_SODIUM_TARGET PkgConfig::SODIUM)
  set(HOLDER_CORE_LIBGIT2_TARGET PkgConfig::LIBGIT2)
  set(HOLDER_CORE_MD4C_TARGET PkgConfig::MD4C)
endif()

if(HOLDER_CORE_USE_PKG_CONFIG_DEPS AND NOT APPLE)
  pkg_check_modules(LIBSECRET IMPORTED_TARGET libsecret-1)
else()
  set(LIBSECRET_FOUND 0)
endif()

if(APPLE)
  find_library(HOLDER_CORE_SECURITY_FRAMEWORK Security)
  find_library(HOLDER_CORE_COREFOUNDATION_FRAMEWORK CoreFoundation)
  if(HOLDER_CORE_SECURITY_FRAMEWORK AND HOLDER_CORE_COREFOUNDATION_FRAMEWORK)
    set(HOLDER_CORE_HAVE_MACOS_KEYCHAIN 1)
  else()
    message(WARNING "macOS Keychain frameworks not found; using non-keyring secret fallback")
    set(HOLDER_CORE_HAVE_MACOS_KEYCHAIN 0)
  endif()
else()
  set(HOLDER_CORE_HAVE_MACOS_KEYCHAIN 0)
endif()

if(WIN32)
  set(HOLDER_CORE_HAVE_WINDOWS_CREDENTIAL_MANAGER 1)
else()
  set(HOLDER_CORE_HAVE_WINDOWS_CREDENTIAL_MANAGER 0)
endif()

add_library(holder STATIC
  src/capi/holder.cpp
  src/capi/version.cpp
  src/ai/AiMessageFrontMatter.cpp
  src/ai/AiMessagePaths.cpp
  src/ai/AiMessageRepo.cpp
  src/ai/AiProviderCredentialRecovery.cpp
  src/ai/AiProviderCredentialRepo.cpp
  src/ai/AiProviderSettingRepo.cpp
  src/ai/AiRunRepo.cpp
  src/ai/AiRunnerRepo.cpp
  src/ai/AiThreadRepo.cpp
  src/ai/AiThreadManifest.cpp
  src/card/CardFrontMatter.cpp
  src/card/CardPaths.cpp
  src/card/CardRepo.cpp
  src/card/CardStore.cpp
  src/card/LinkKindCatalog.cpp
  src/card/LinkRepo.cpp
  src/card/MilestoneRepo.cpp
  src/card/TagExtractor.cpp
  src/card/TagRepo.cpp
  src/git/EcdsaDerSigningCredentialProvider.cpp
  src/git/GitOps.cpp
  src/git/GitRepo.cpp
  src/git/RepoSyncMetrics.cpp
  src/git/SshAgentAndFileCredentialProvider.cpp
  src/history/CardHistory.cpp
  src/index/FtsIndexer.cpp
  src/index/Reindexer.cpp
  src/platform/Db.cpp
  src/platform/DatabaseRebuild.cpp
  src/platform/Fs.cpp
  src/platform/Migrations.cpp
  src/platform/Tx.cpp
  src/privacy/CryptoService.cpp
  src/privacy/PlatformKeyring.cpp
  src/privacy/ProjectPrivacy.cpp
  src/privacy/SecretStore.cpp
  src/project/DefaultProject.cpp
  src/project/ProjectPaths.cpp
  src/project/ProjectManifest.cpp
  src/project/ProjectRepo.cpp
  src/project/ProjectStore.cpp
  src/project/ProjectSyncRepo.cpp
  src/project/Rebuilder.cpp
  src/project/StartupRecovery.cpp
  src/resource/LocationRepo.cpp
  src/resource/LocationStore.cpp
  src/resource/MetadataMapping.cpp
  src/resource/AssetEnvelope.cpp
  src/resource/AssetImportService.cpp
  src/resource/LocalDirectoryProvider.cpp
  src/resource/LocationBindingStore.cpp
  src/resource/ResourceManifest.cpp
  src/resource/ResourcePaths.cpp
  src/resource/ResourceRepo.cpp
  src/resource/ResourceStore.cpp
  src/sync/ProjectSyncPolicy.cpp
  src/sync/PullConflictResolution.cpp
)
add_library(Holder::Core ALIAS holder)
set_target_properties(holder PROPERTIES
  EXPORT_NAME Core
  POSITION_INDEPENDENT_CODE ON
)

target_include_directories(holder PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/holder>
)

target_compile_definitions(holder PUBLIC
  CARD_SERVER_HAVE_LIBGIT2=1
  HOLDER_HAVE_MACOS_KEYCHAIN=${HOLDER_CORE_HAVE_MACOS_KEYCHAIN}
  HOLDER_HAVE_WINDOWS_CREDENTIAL_MANAGER=${HOLDER_CORE_HAVE_WINDOWS_CREDENTIAL_MANAGER}
)

target_compile_definitions(holder PRIVATE
  HOLDER_VERSION_STRING="${HOLDER_CORE_VERSION}"
  HOLDER_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
  HOLDER_VERSION_MINOR=${PROJECT_VERSION_MINOR}
  HOLDER_VERSION_PATCH=${PROJECT_VERSION_PATCH}
)

if(LIBSECRET_FOUND)
  target_link_libraries(holder PUBLIC PkgConfig::LIBSECRET)
  target_compile_definitions(holder PUBLIC HOLDER_HAVE_LIBSECRET=1)
else()
  target_compile_definitions(holder PUBLIC HOLDER_HAVE_LIBSECRET=0)
endif()

target_link_libraries(holder PUBLIC
  OpenSSL::SSL
  OpenSSL::Crypto
  ${HOLDER_CORE_SQLITE3_TARGET}
  ${HOLDER_CORE_SODIUM_TARGET}
  ${HOLDER_CORE_LIBGIT2_TARGET}
  ${HOLDER_CORE_MD4C_TARGET}
  Threads::Threads
  nlohmann_json::nlohmann_json
  spdlog::spdlog
  ${HOLDER_CORE_YAML_CPP_TARGET}
)

if(HOLDER_CORE_HAVE_MACOS_KEYCHAIN)
  target_link_libraries(holder PUBLIC
    "${HOLDER_CORE_SECURITY_FRAMEWORK}"
    "${HOLDER_CORE_COREFOUNDATION_FRAMEWORK}"
  )
endif()

if(HOLDER_CORE_HAVE_WINDOWS_CREDENTIAL_MANAGER)
  target_link_libraries(holder PUBLIC Advapi32)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(holder PRIVATE
    -Wall -Wextra -Wpedantic
    -Wshadow -Wconversion -Wsign-conversion
    -Wno-unknown-pragmas
  )
  if(HOLDER_CORE_WARNINGS_AS_ERRORS)
    target_compile_options(holder PRIVATE -Werror)
  endif()
endif()

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()

install(TARGETS holder
  EXPORT holderTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(DIRECTORY src/
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/holder
  FILES_MATCHING PATTERN "*.h"
)
install(DIRECTORY include/
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
  FILES_MATCHING PATTERN "*.h"
)
install(EXPORT holderTargets
  NAMESPACE Holder::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/holder
)

configure_package_config_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/holderConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/holderConfig.cmake"
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/holder
)
write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/holderConfigVersion.cmake"
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion
)
install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/holderConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/holderConfigVersion.cmake"
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/holder
)
