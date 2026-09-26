#!/usr/bin/env python3
"""Check IDF example dependency naming; this is not an SDK build or link test."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which("cmake"), "CMake is required for configure contracts")
class IdfComponentContract(unittest.TestCase):
    def test_checkout_directory_names(self):
        for checkout_name in ("TMP1x2", "TMP1x2-1.0.0"):
            with self.subTest(checkout=checkout_name), tempfile.TemporaryDirectory(
                prefix="tmp1x2-idf-contract-"
            ) as temporary:
                workspace = Path(temporary)
                example = workspace / checkout_name / "examples/esp_idf/basic/main"
                example.mkdir(parents=True)
                shutil.copyfile(
                    ROOT / "examples/esp_idf/basic/main/CMakeLists.txt",
                    example / "CMakeLists.txt",
                )
                # Validate the registration call during CMake configuration,
                # without claiming that fake registration establishes IDF linking.
                (workspace / "CMakeLists.txt").write_text(
                    '''cmake_minimum_required(VERSION 3.16)
project(tmp1x2_idf_contract NONE)
function(idf_component_register)
  cmake_parse_arguments(COMP "" "" "SRCS;INCLUDE_DIRS;REQUIRES" ${ARGN})
  set(expected "${CHECKOUT_NAME};esp_driver_i2c;esp_timer;esp_driver_gpio")
  if(NOT "${COMP_REQUIRES}" STREQUAL "${expected}")
    message(FATAL_ERROR "Component dependencies '${COMP_REQUIRES}' != '${expected}'")
  endif()
  set(COMPONENT_LIB contract_component PARENT_SCOPE)
endfunction()
function(target_compile_features)
  if(NOT "${ARGV}" STREQUAL "contract_component;PUBLIC;cxx_std_17")
    message(FATAL_ERROR "Unexpected C++ standard contract: ${ARGV}")
  endif()
endfunction()
add_subdirectory("${CHECKOUT_NAME}/examples/esp_idf/basic/main" main)
''',
                    encoding="utf-8",
                )
                result = subprocess.run(
                    ["cmake", "-S", str(workspace), "-B", str(workspace / "build"),
                     "-DCHECKOUT_NAME=" + checkout_name],
                    capture_output=True,
                    text=True,
                    check=False,
                )
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
