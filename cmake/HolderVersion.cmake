file(READ "${CMAKE_CURRENT_LIST_DIR}/../VERSION" HOLDER_CORE_VERSION)
string(STRIP "${HOLDER_CORE_VERSION}" HOLDER_CORE_VERSION)

if(NOT HOLDER_CORE_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
  message(FATAL_ERROR "VERSION must contain a semantic version like 0.1.7")
endif()
