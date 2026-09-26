# EXTRA_COMPONENT_DIRS registers this library using its checkout directory name.
# Archive extraction and package consumers may name that directory differently.
get_filename_component(TMP1X2_COMPONENT_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
get_filename_component(TMP1X2_COMPONENT_NAME "${TMP1X2_COMPONENT_DIR}" NAME)
idf_component_register(
  SRCS "main.cpp" "../../../common/Tmp1x2Cli.cpp"
  INCLUDE_DIRS "../../../common" "../../../../include"
  REQUIRES ${TMP1X2_COMPONENT_NAME} esp_driver_i2c esp_timer esp_driver_gpio
)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
