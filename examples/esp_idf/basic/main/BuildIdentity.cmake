# Example-only build metadata. Run at build time so an incremental compile cannot
# retain a clean identity after a source edit. Archives must not inherit the
# identity of an unrelated repository containing their extraction directory.
if(NOT DEFINED TMP1X2_SOURCE_DIR OR NOT DEFINED TMP1X2_IDENTITY_HEADER)
  message(FATAL_ERROR "TMP1x2 build identity requires source and output paths")
endif()

set(TMP1X2_COMMIT "unknown")
set(TMP1X2_STATUS "unknown")
if(EXISTS "${TMP1X2_SOURCE_DIR}/.git")
  # .git can be a directory or a linked-worktree file.
  find_package(Git QUIET)
  if(GIT_FOUND)
    execute_process(
      COMMAND "${GIT_EXECUTABLE}" -C "${TMP1X2_SOURCE_DIR}" rev-parse --show-toplevel
      RESULT_VARIABLE TMP1X2_ROOT_RESULT OUTPUT_VARIABLE TMP1X2_GIT_ROOT
      OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET TIMEOUT 5
    )
    get_filename_component(TMP1X2_SOURCE_REAL "${TMP1X2_SOURCE_DIR}" REALPATH)
    get_filename_component(TMP1X2_GIT_ROOT_REAL "${TMP1X2_GIT_ROOT}" REALPATH)
    if(TMP1X2_ROOT_RESULT EQUAL 0 AND TMP1X2_SOURCE_REAL STREQUAL TMP1X2_GIT_ROOT_REAL)
      execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${TMP1X2_SOURCE_DIR}" rev-parse --verify HEAD
        RESULT_VARIABLE TMP1X2_COMMIT_RESULT OUTPUT_VARIABLE TMP1X2_HEAD
        OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET TIMEOUT 5
      )
      execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${TMP1X2_SOURCE_DIR}" status --porcelain --untracked-files=all
        RESULT_VARIABLE TMP1X2_STATUS_RESULT OUTPUT_VARIABLE TMP1X2_CHANGES
        OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET TIMEOUT 5
      )
      if(TMP1X2_COMMIT_RESULT EQUAL 0 AND TMP1X2_HEAD MATCHES "^[0-9a-f]+$")
        set(TMP1X2_COMMIT "${TMP1X2_HEAD}")
        if(TMP1X2_STATUS_RESULT EQUAL 0)
          set(TMP1X2_STATUS "clean")
          if(NOT TMP1X2_CHANGES STREQUAL "")
            set(TMP1X2_STATUS "dirty")
          endif()
        endif()
      endif()
    endif()
  endif()
endif()

string(TIMESTAMP TMP1X2_TIMESTAMP "%Y-%m-%dT%H:%M:%SZ" UTC)
string(SUBSTRING "${TMP1X2_TIMESTAMP}" 0 10 TMP1X2_DATE)
string(SUBSTRING "${TMP1X2_TIMESTAMP}" 11 8 TMP1X2_TIME)
get_filename_component(TMP1X2_OUTPUT_DIR "${TMP1X2_IDENTITY_HEADER}" DIRECTORY)
file(MAKE_DIRECTORY "${TMP1X2_OUTPUT_DIR}")
file(WRITE "${TMP1X2_IDENTITY_HEADER}.tmp"
  "// Generated example build identity; do not edit.\n#pragma once\n"
  "#define TMP1X2_GIT_COMMIT \"${TMP1X2_COMMIT}\"\n"
  "#define TMP1X2_GIT_STATUS \"${TMP1X2_STATUS}\"\n"
  "#define TMP1X2_BUILD_TIMESTAMP \"${TMP1X2_TIMESTAMP}\"\n"
  "#define TMP1X2_BUILD_DATE \"${TMP1X2_DATE}\"\n"
  "#define TMP1X2_BUILD_TIME \"${TMP1X2_TIME}\"\n"
)
# Avoid changing the header mtime if reproducible build metadata is unchanged.
configure_file("${TMP1X2_IDENTITY_HEADER}.tmp" "${TMP1X2_IDENTITY_HEADER}" COPYONLY)
file(REMOVE "${TMP1X2_IDENTITY_HEADER}.tmp")
