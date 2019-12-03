# Try to find eos folly.
# Once done, this will define
#
# EOS_FOLLY_FOUND            - system has eos-folly
# EOS_FOLLY_INCLUDE_DIRS     - eos-folly include directories
# EOS_FOLLY_LIBRARIES        - eos-folly library library
#
# EOS_FOLLY_ROOT may be defined as a hint for where to look
#
# and the following imported targets
#
# FOLLY::FOLLY

find_path(EOSFOLLY_INCLUDE_DIR
  NAMES folly/folly-config.h
  HINTS /opt/eos-folly/ ${EOSFOLLY_ROOT}
  PATH_SUFFIXES include)

find_library(EOSFOLLY_LIBRARY
  NAMES libfolly.so
  HINTS /opt/eos-folly/ ${EOSFOLLY_ROOT}
  PATH_SUFFIXES lib lib64)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(eosfolly
  REQUIRED_VARS EOSFOLLY_LIBRARY EOSFOLLY_INCLUDE_DIR)

mark_as_advanced(EOSFOLLY_FOUND EOSFOLLY_LIBRARY EOSFOLLY_INCLUDE_DIR)

if(EOSFOLLY_FOUND AND NOT TARGET FOLLY::FOLLY)
  add_library(FOLLY::FOLLY STATIC IMPORTED)
  set_target_properties(FOLLY::FOLLY PROPERTIES
    IMPORTED_LOCATION "${EOSFOLLY_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${EOSFOLLY_INCLUDE_DIR}")
  target_compile_definitions(FOLLY::FOLLY INTERFACE HAVE_FOLLY=1)
  get_filename_component(EOSFOLLY_LINK_DIRECTORY "${EOSFOLLY_LIBRARY}" DIRECTORY)
  target_link_directories(FOLLY::FOLLY INTERFACE
    "${EOSFOLLY_LINK_DIRECTORY}")
endif()

set(EOSFOLLY_LIBRARIES ${EOSFOLLY_LIBRARY})
set(EOSFOLLY_INCLUDE_DIRS ${EOSFOLLY_INCLUDE_DIR})
# This is done to preserve compatibility with qclient
set(FOLLY_INCLUDE_DIRS ${EOSFOLLY_INCLUDE_DIRS})
set(FOLLY_LIBRARIES    ${EOSFOLLY_LIBRARIES} )
set(FOLLY_FOUND TRUE)
unset(EOSFOLLY_LIBRARY)
unset(EOSFOLLY_INCLUDE_DIR)

