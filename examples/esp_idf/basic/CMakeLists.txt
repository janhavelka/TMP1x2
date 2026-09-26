cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../..")
# Retain the package version in archives and checkouts without a Git tag.
file(READ "${CMAKE_CURRENT_LIST_DIR}/../../../library.json" TMP1X2_MANIFEST)
string(REGEX MATCH "\"version\"[ \t\r\n]*:[ \t\r\n]*\"([^\"]+)\"" TMP1X2_VERSION_MATCH "${TMP1X2_MANIFEST}")
set(PROJECT_VER "${CMAKE_MATCH_1}")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(tmp1x2_basic_cli)
