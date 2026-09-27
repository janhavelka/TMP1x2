set(TMP1X2_SOURCES
  src/TMP1x2.cpp
  src/TMP1x2Configuration.cpp
  src/TMP1x2Measurement.cpp
  src/TMP1x2Operations.cpp
  src/BusOperations.cpp
)

if(COMMAND idf_component_register)
  idf_component_register(SRCS ${TMP1X2_SOURCES} INCLUDE_DIRS "include")
  target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
else()
  cmake_minimum_required(VERSION 3.16)
  project(TMP1x2 VERSION 1.0.0 LANGUAGES CXX)
  add_library(TMP1x2 ${TMP1X2_SOURCES})
  target_include_directories(TMP1x2 PUBLIC include)
  target_compile_features(TMP1x2 PUBLIC cxx_std_17)
  option(TMP1X2_BUILD_TESTS "Build native regression tests" OFF)
  if(TMP1X2_BUILD_TESTS)
    enable_testing()
    add_executable(tmp1x2_tests test/test_core.cpp)
    target_link_libraries(tmp1x2_tests PRIVATE TMP1x2)
    add_test(NAME core COMMAND tmp1x2_tests)
    add_executable(tmp1x2_cli_tests test/cli/test_cli.cpp
      examples/common/Tmp1x2Cli.cpp
      examples/common/Tmp1x2CliDiagnostics.cpp
      examples/common/Tmp1x2CliOutput.cpp
    )
    target_include_directories(tmp1x2_cli_tests PRIVATE examples/common)
    target_link_libraries(tmp1x2_cli_tests PRIVATE TMP1x2)
    add_test(NAME cli COMMAND tmp1x2_cli_tests)
    add_executable(tmp1x2_job_tests test/jobs/test_jobs.cpp)
    target_link_libraries(tmp1x2_job_tests PRIVATE TMP1x2)
    add_test(NAME jobs COMMAND tmp1x2_job_tests)
    add_executable(tmp1x2_variant_tests test/variants/test_variants.cpp)
    target_link_libraries(tmp1x2_variant_tests PRIVATE TMP1x2)
    add_test(NAME variants COMMAND tmp1x2_variant_tests)
    add_executable(tmp1x2_helper_tests test/helpers/test_helpers.cpp)
    target_link_libraries(tmp1x2_helper_tests PRIVATE TMP1x2)
    add_test(NAME helpers COMMAND tmp1x2_helper_tests)
    add_executable(tmp1x2_bus_tests test/bus/test_bus.cpp)
    target_link_libraries(tmp1x2_bus_tests PRIVATE TMP1x2)
    add_test(NAME bus COMMAND tmp1x2_bus_tests)
    add_executable(tmp1x2_core_no_arduino test/core_no_arduino/compile_main.cpp)
    target_link_libraries(tmp1x2_core_no_arduino PRIVATE TMP1x2)
    add_test(NAME core_no_arduino COMMAND tmp1x2_core_no_arduino)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME version_sync
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_version_sync.py")
    add_test(NAME idf_component_contract
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_idf_component_contract.py")
  endif()
endif()
