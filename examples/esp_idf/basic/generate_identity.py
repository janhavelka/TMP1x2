"""Refresh native example identity before each PlatformIO/SCons build."""
from pathlib import Path
import shutil
import subprocess


def refresh_identity(project_dir, build_dir, cmake_package=None):
    project = Path(project_dir).resolve()
    cmake = None
    if cmake_package:
        candidates = [Path(cmake_package) / "bin" / name for name in ("cmake", "cmake.exe")]
        cmake = next((str(path) for path in candidates if path.is_file()), None)
    cmake = cmake or shutil.which("cmake")
    if not cmake:
        raise RuntimeError("CMake is required for native TMP1x2 example build identity")
    subprocess.run([
        cmake, "-DTMP1X2_SOURCE_DIR=" + str(project.parents[2]),
        "-DTMP1X2_IDENTITY_HEADER=" + str(Path(build_dir) / "tmp1x2_identity/Tmp1x2BuildIdentity.h"),
        "-P", str(project / "main/BuildIdentity.cmake"),
    ], check=True, timeout=30)


if "Import" in globals():
    Import("env")
    refresh_identity(env.subst("$PROJECT_DIR"), env.subst("$BUILD_DIR"),
                     env.PioPlatform().get_package_dir("tool-cmake"))
