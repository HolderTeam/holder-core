file(READ "${CMAKE_CURRENT_LIST_DIR}/../VERSION" HOLDER_CORE_VERSION)
string(STRIP "${HOLDER_CORE_VERSION}" HOLDER_CORE_VERSION)

if(HOLDER_CORE_VERSION MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)(-[0-9A-Za-z-]+(\\.[0-9A-Za-z-]+)*)?(\\+[0-9A-Za-z-]+(\\.[0-9A-Za-z-]+)*)?$")
  # CMake's project(VERSION) accepts only numeric components. Keep the full
  # SemVer for Holder's public version string and expose its numeric core to
  # CMake's project/package version machinery.
  set(HOLDER_CORE_CMAKE_VERSION
      "${CMAKE_MATCH_1}.${CMAKE_MATCH_2}.${CMAKE_MATCH_3}")
else()
  message(FATAL_ERROR
    "VERSION must contain a semantic version like 0.1.7 or 2.0.0-dev")
endif()
