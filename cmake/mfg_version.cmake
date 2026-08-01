# Regenerate version.h AT BUILD TIME with the commit the DLL is actually being built from.
#
# It used to be configured once, at CMake configure time, which meant a build after new commits
# kept whatever hash the last configure happened to see - on 2026-07-30 a DLL built from 97c3534
# announced itself as 947216c-dirty, which is exactly the kind of thing that sends a bug report
# down the wrong path. Run this with `cmake -P` from a custom target instead.
#
# Expects: SRC_DIR, IN_FILE, OUT_FILE, FULL_VERSION, BUILD_NAME.
#
# The output is only rewritten when the CONTENT changes, so an incremental build does not
# recompile the four translation units that include version.h on every single build.

execute_process(COMMAND git -C "${SRC_DIR}" rev-parse --short HEAD
                OUTPUT_VARIABLE GIT_HASH OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)

if(NOT GIT_HASH)
  set(GIT_HASH "nogit")
else()
  # Uncommitted tracked changes get a "d" PREFIX rather than a "-dirty" suffix: the hash stays
  # the last thing on the line (easy to copy out of a user's log) and the marker is impossible
  # to mistake for part of it.
  execute_process(COMMAND git -C "${SRC_DIR}" diff --quiet HEAD
                  RESULT_VARIABLE _git_dirty ERROR_QUIET)
  if(_git_dirty)
    set(GIT_HASH "d${GIT_HASH}")
  endif()
endif()

configure_file("${IN_FILE}" "${OUT_FILE}.tmp" @ONLY)

set(_write TRUE)
if(EXISTS "${OUT_FILE}")
  file(READ "${OUT_FILE}" _old)
  file(READ "${OUT_FILE}.tmp" _new)
  if(_old STREQUAL _new)
    set(_write FALSE)
  endif()
endif()
if(_write)
  configure_file("${OUT_FILE}.tmp" "${OUT_FILE}" COPYONLY)
  message(STATUS "MapForGoblins build identity: ${FULL_VERSION} [${BUILD_NAME}] (${GIT_HASH})")
endif()
