#include "holder/holder.h"

#ifndef HOLDER_VERSION_STRING
#define HOLDER_VERSION_STRING "0.0.0"
#endif

#ifndef HOLDER_VERSION_MAJOR
#define HOLDER_VERSION_MAJOR 0
#endif

#ifndef HOLDER_VERSION_MINOR
#define HOLDER_VERSION_MINOR 0
#endif

#ifndef HOLDER_VERSION_PATCH
#define HOLDER_VERSION_PATCH 0
#endif

const char* holder_version_string(void) {
  return HOLDER_VERSION_STRING;
}

int holder_version_major(void) {
  return HOLDER_VERSION_MAJOR;
}

int holder_version_minor(void) {
  return HOLDER_VERSION_MINOR;
}

int holder_version_patch(void) {
  return HOLDER_VERSION_PATCH;
}
