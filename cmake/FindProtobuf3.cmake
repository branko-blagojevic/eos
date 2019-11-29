# Try to find PROTOBUF3
# Once done, this will define
#
# PROTOBUF_FOUND               - system has PROTOBUF
# PROTOBUF_INCLUDE_DIRs        - Protobuf include directories
# PROTOBUF_LIBRARIES           - libraries needed to use Protobuf
#
# and the following imported targets
#
# PROTOBUF::PROTOBUF
#
# PROTOBUF_ROOT may be defined as a hint for where to look

find_program(PROTOBUF_PROTOC_EXECUTABLE
  NAMES protoc3
  HINTS /opt/eos/bin /usr/bin/ /bin/ ${PROTOBUF_ROOT} NO_DEFAULT_PATH
  DOC "Version 3 of The Google Protocol Buffers Compiler")

find_path(PROTOBUF_INCLUDE_DIR
  NAMES google/protobuf/message.h
  HINTS /opt/eos/include/protobuf3 /usr/include/protobuf3
        /usr/include ${PROTOBUF_ROOT} NO_DEFAULT_PATH)

find_library(PROTOBUF_LIBRARY
  NAME protobuf
  PATHS /opt/eos/lib64/protobuf3 /usr/lib64/protobuf3 /usr/lib/protobuf3
        /usr/lib64 /usr/lib/x86_64-linux-gnu ${PROTOBUF_ROOT} NO_DEFAULT_PATH)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Protobuf3
  REQUIRED_VARS PROTOBUF_INCLUDE_DIR PROTOBUF_LIBRARY)
mark_as_advanced(PROOBUF3_FOUND PROTOBUF_INCLUDE_DIR PROTOBUF_LIBRARY)

if (PROTOBUF_PROTOC_EXECUTABLE)
  message(STATUS "Found protoc: ${PROTOBUF_PROTOC_EXECUTABLE}")
else()
  message(STATUS "Could NOT find protoc (missing: PROTOBUF_PROTOC_EXECUTABLE)")
endif()

if (PROTOBUF3_FOUND AND NOT TARGET PROTOBUF::PROTOBUF)
  add_library(PROTOBUF::PROTOBUF UNKNOWN IMPORTED)
  set_target_properties(PROTOBUF::PROTOBUF PROPERTIES
    IMPORTED_LOCATION "${PROTOBUF_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${PROTOBUF_INCLUDE_DIR}")
endif ()

# Include Protobuf package from the generation commands
find_package(Protobuf)

set(PROTOBUF_INCLUDE_DIRS ${PROTOBUF_INCLUDE_DIR})
set(PROTOBUF_LIBRARIES ${PROTOBUF_LIBRARY})
#unset(PROTOBUF_INCLUDE_DIR)
#unset(PROTOBUF_LIBRARY)
