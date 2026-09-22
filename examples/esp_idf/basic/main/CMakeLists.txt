idf_component_register(
  SRCS "main.cpp" "../../../common/Tmp1x2Cli.cpp"
  INCLUDE_DIRS "../../../common" "../../../../include"
  REQUIRES TMP1x2 esp_driver_i2c esp_timer esp_driver_gpio
)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
