# EXTRA_COMPONENT_DIRS registers this library using its checkout directory name.
# Archive extraction and package consumers may name that directory differently.
get_filename_component(TMP1X2_COMPONENT_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
get_filename_component(TMP1X2_COMPONENT_NAME "${TMP1X2_COMPONENT_DIR}" NAME)
idf_component_register(
  SRCS "main.cpp"
       "../../../common/Tmp1x2Cli.cpp"
       "../../../common/Tmp1x2CliDiagnostics.cpp"
       "../../../common/Tmp1x2CliOutput.cpp"
  INCLUDE_DIRS "../../../common" "../../../../include"
  REQUIRES ${TMP1X2_COMPONENT_NAME} esp_driver_i2c esp_timer esp_driver_gpio
)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)

# Refresh example provenance on every build, including incremental source edits.
# The core component remains independent of this example-only generated header.
set(TMP1X2_IDENTITY_DIR "${CMAKE_BINARY_DIR}/tmp1x2_identity")
set(TMP1X2_BUILD_IDENTITY "${TMP1X2_IDENTITY_DIR}/Tmp1x2BuildIdentity.h")
# PlatformIO imports this configuration but runs SCons rather than our custom
# target. Its pre-build script refreshes the same header on every invocation.
execute_process(
  COMMAND "${CMAKE_COMMAND}"
    "-DTMP1X2_SOURCE_DIR=${TMP1X2_COMPONENT_DIR}"
    "-DTMP1X2_IDENTITY_HEADER=${TMP1X2_BUILD_IDENTITY}"
    -P "${CMAKE_CURRENT_LIST_DIR}/BuildIdentity.cmake"
  RESULT_VARIABLE TMP1X2_IDENTITY_RESULT
)
if(NOT TMP1X2_IDENTITY_RESULT EQUAL 0)
  message(FATAL_ERROR "Could not generate TMP1x2 example build identity")
endif()
add_custom_target(tmp1x2_example_identity
  COMMAND "${CMAKE_COMMAND}"
    "-DTMP1X2_SOURCE_DIR=${TMP1X2_COMPONENT_DIR}"
    "-DTMP1X2_IDENTITY_HEADER=${TMP1X2_BUILD_IDENTITY}"
    -P "${CMAKE_CURRENT_LIST_DIR}/BuildIdentity.cmake"
  BYPRODUCTS "${TMP1X2_BUILD_IDENTITY}"
  VERBATIM
)
add_dependencies(${COMPONENT_LIB} tmp1x2_example_identity)
# Only the shared CLI output explicitly includes the header. A forced include
# here would also reach unrelated sources in PlatformIO's merged environment.
target_include_directories(${COMPONENT_LIB} PRIVATE "${TMP1X2_IDENTITY_DIR}")
