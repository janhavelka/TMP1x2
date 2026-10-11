#!/usr/bin/env python3
"""Check example registration/build provenance; this is not an IDF SDK link test."""
from pathlib import Path
import configparser
import re
import runpy
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
EXAMPLE_MAIN = Path("examples/esp_idf/basic/main")


@unittest.skipUnless(shutil.which("cmake"), "CMake is required for build contracts")
class IdfComponentContract(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="tmp1x2-idf-contract-")
        self.addCleanup(temporary.cleanup)
        self.workspace = Path(temporary.name)

    def run_command(self, *args, cwd=None):
        result = subprocess.run(args, cwd=cwd, capture_output=True, text=True,
                                check=False, timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result.stdout.strip()

    def prepare(self, checkout_name="TMP1x2"):
        checkout = self.workspace / checkout_name
        example = checkout / EXAMPLE_MAIN
        example.mkdir(parents=True)
        for name in ("CMakeLists.txt", "BuildIdentity.cmake"):
            shutil.copyfile(ROOT / EXAMPLE_MAIN / name, example / name)
        (checkout / "tracked.txt").write_text("baseline\n", encoding="utf-8")
        (checkout / ".gitignore").write_text("ignored.txt\n", encoding="utf-8")
        return checkout

    def configure(self, checkout):
        # A real build target exercises generated-header ordering and refresh.
        # Registration/includes are checked without simulating an SDK compiler.
        (self.workspace / "CMakeLists.txt").write_text(
            '''cmake_minimum_required(VERSION 3.16)
project(tmp1x2_idf_contract NONE)
function(idf_component_register)
  cmake_parse_arguments(COMP "" "" "SRCS;INCLUDE_DIRS;REQUIRES" ${ARGN})
  set(expected "${CHECKOUT_NAME};esp_driver_i2c;esp_timer;esp_driver_gpio;esp_driver_uart;esp_driver_usb_serial_jtag")
  if(NOT "${COMP_REQUIRES}" STREQUAL "${expected}")
    message(FATAL_ERROR "Component dependencies '${COMP_REQUIRES}' != '${expected}'")
  endif()
  add_custom_target(contract_component ALL)
  set(COMPONENT_LIB contract_component PARENT_SCOPE)
endfunction()
function(target_compile_features)
  if(NOT "${ARGV}" STREQUAL "contract_component;PUBLIC;cxx_std_17")
    message(FATAL_ERROR "Unexpected C++ standard contract: ${ARGV}")
  endif()
endfunction()
function(target_include_directories)
  set(expected "contract_component;PRIVATE;${CMAKE_BINARY_DIR}/tmp1x2_identity")
  if(NOT "${ARGV}" STREQUAL "${expected}")
    message(FATAL_ERROR "Unexpected identity include directory: ${ARGV}")
  endif()
endfunction()
add_subdirectory("${CHECKOUT_NAME}/examples/esp_idf/basic/main" main)
''', encoding="utf-8")
        generator = ("-G", "Ninja") if shutil.which("ninja") else ()
        self.run_command("cmake", "-S", str(self.workspace), "-B",
                         str(self.workspace / "build"), *generator,
                         "-DCHECKOUT_NAME=" + checkout.name)

    def build_identity(self):
        self.run_command("cmake", "--build", str(self.workspace / "build"),
                         "--target", "contract_component")
        return self.read_identity()

    def read_identity(self):
        header = self.workspace / "build/tmp1x2_identity/Tmp1x2BuildIdentity.h"
        self.assertTrue(header.exists(), "component dependency must generate the header")
        values = dict(re.findall(r'^#define TMP1X2_(\w+) "([^"]+)"$',
                                 header.read_text(encoding="utf-8"), re.MULTILINE))
        self.assertRegex(values["BUILD_TIMESTAMP"],
                         r"^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z$")
        self.assertEqual(values["BUILD_DATE"], values["BUILD_TIMESTAMP"][:10])
        self.assertEqual(values["BUILD_TIME"], values["BUILD_TIMESTAMP"][11:19])
        return values

    @unittest.skipUnless(shutil.which("git"), "Git is required for provenance tests")
    def test_platformio_refresh_without_cmake_target_or_reconfigure(self):
        checkout = self.prepare()
        commit = self.initialize_git(checkout)
        self.configure(checkout)
        config = configparser.ConfigParser()
        config.read(ROOT / EXAMPLE_MAIN.parent / "platformio.ini", encoding="utf-8")
        self.assertEqual(config["env"]["extra_scripts"], "pre:generate_identity.py")
        refresh = runpy.run_path(
            str(ROOT / EXAMPLE_MAIN.parent / "generate_identity.py"))["refresh_identity"]
        project = checkout / "examples/esp_idf/basic"
        refresh(project, self.workspace / "build")
        self.assertEqual(self.read_identity()["GIT_COMMIT"], commit)
        self.assertEqual(self.read_identity()["GIT_STATUS"], "clean")
        (checkout / "tracked.txt").write_text("SCons incremental edit\n", encoding="utf-8")
        refresh(project, self.workspace / "build")
        self.assertEqual(self.read_identity()["GIT_STATUS"], "dirty")

    def git(self, checkout, *args):
        return self.run_command("git", "-C", str(checkout), *args)

    def initialize_git(self, checkout):
        self.git(checkout, "init", "--quiet")
        self.git(checkout, "add", ".")
        self.git(checkout, "-c", "user.name=TMP1x2 contract",
                 "-c", "user.email=contract@example.invalid",
                 "-c", "commit.gpgsign=false", "commit", "--quiet", "-m", "fixture")
        return self.git(checkout, "rev-parse", "HEAD")

    def test_checkout_directory_names_and_archive_identity(self):
        for checkout_name in ("TMP1x2", "TMP1x2-1.0.0"):
            with self.subTest(checkout=checkout_name):
                checkout = self.prepare(checkout_name)
                self.configure(checkout)
                values = self.build_identity()
                self.assertEqual(values["GIT_COMMIT"], "unknown")
                self.assertEqual(values["GIT_STATUS"], "unknown")

    @unittest.skipUnless(shutil.which("git"), "Git is required for provenance tests")
    def test_clean_dirty_and_new_commit_refresh_without_reconfigure(self):
        checkout = self.prepare()
        original_commit = self.initialize_git(checkout)
        self.configure(checkout)
        self.assertEqual(self.build_identity()["GIT_COMMIT"], original_commit)
        self.assertEqual(self.build_identity()["GIT_STATUS"], "clean")
        (checkout / "tracked.txt").write_text("changed\n", encoding="utf-8")
        self.assertEqual(self.build_identity()["GIT_STATUS"], "dirty")
        new_commit = self.initialize_git(checkout)
        self.assertNotEqual(new_commit, original_commit)
        values = self.build_identity()
        self.assertEqual(values["GIT_COMMIT"], new_commit)
        self.assertEqual(values["GIT_STATUS"], "clean")
        (checkout / "untracked.txt").write_text("new source\n", encoding="utf-8")
        self.assertEqual(self.build_identity()["GIT_STATUS"], "dirty")
        (checkout / "untracked.txt").unlink()
        (checkout / "ignored.txt").write_text("build output\n", encoding="utf-8")
        self.assertEqual(self.build_identity()["GIT_STATUS"], "clean")

    @unittest.skipUnless(shutil.which("git"), "Git is required for provenance tests")
    def test_archive_inside_unrelated_git_does_not_inherit_identity(self):
        checkout = self.prepare()
        self.initialize_git(self.workspace)
        self.configure(checkout)
        values = self.build_identity()
        self.assertEqual(values["GIT_COMMIT"], "unknown")
        self.assertEqual(values["GIT_STATUS"], "unknown")

    @unittest.skipUnless(shutil.which("git"), "Git is required for provenance tests")
    def test_linked_worktree_git_file(self):
        original = self.prepare("original")
        commit = self.initialize_git(original)
        checkout = self.workspace / "linked"
        self.git(original, "worktree", "add", "--detach", str(checkout), "HEAD")
        self.assertTrue((checkout / ".git").is_file())
        self.configure(checkout)
        values = self.build_identity()
        self.assertEqual(values["GIT_COMMIT"], commit)
        self.assertEqual(values["GIT_STATUS"], "clean")
        (checkout / "tracked.txt").write_text("linked change\n", encoding="utf-8")
        self.assertEqual(self.build_identity()["GIT_STATUS"], "dirty")


if __name__ == "__main__":
    unittest.main()
