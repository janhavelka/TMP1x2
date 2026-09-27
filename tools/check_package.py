#!/usr/bin/env python3
"""Inspect a PlatformIO release archive and build an isolated CMake consumer.

Create the archive with scripts/pio.cmd pkg pack -o build/TMP1x2.tar.gz .
Then run this script with the archive path (and --generator Ninja if needed).
"""
import argparse
import json
from pathlib import Path, PurePosixPath, PureWindowsPath
import subprocess
import tarfile
import tempfile


REQUIRED = {
    "library.json", "idf_component.yml", "CMakeLists.txt", "LICENSE", "README.md",
    "src/TMP1x2.cpp", "src/TMP1x2Configuration.cpp", "src/TMP1x2Measurement.cpp",
    "src/TMP1x2Operations.cpp", "include/TMP1x2/TMP1x2.h", "include/TMP1x2/Config.h",
    "include/TMP1x2/Status.h", "include/TMP1x2/CommandTable.h", "include/TMP1x2/Version.h",
    "include/TMP1x2/BusOperations.h", "src/BusOperations.cpp",
    "examples/common/Tmp1x2Cli.h", "examples/common/Tmp1x2Cli.cpp",
    "examples/common/Tmp1x2CliDiagnostics.cpp", "examples/common/Tmp1x2CliOutput.cpp",
    "examples/common/BoardConfig.h", "examples/01_basic_bringup_cli/main.cpp",
    "examples/esp_idf/basic/CMakeLists.txt", "examples/esp_idf/basic/main/CMakeLists.txt",
    "examples/esp_idf/basic/main/main.cpp", "examples/esp_idf/basic/platformio.ini",
    "examples/esp_idf/basic/sdkconfig.defaults",
}


def forbidden(path):
    parts = path.parts
    return (
        any(part in {".git", ".github", ".pio", "managed_components", "__pycache__"}
            or part.startswith("build") for part in parts)
        or parts[0] in {"test", "tools", "scripts"}
        or path.name in {"sdkconfig", "sdkconfig.old", "dependencies.lock"}
        or path.name.startswith("sdkconfig.esp32")
        or path.suffix in {".log", ".pyc", ".elf", ".bin", ".map"}
        or (len(parts) > 2 and parts[:2] == ("docs", "reference") and path.name != "README.md")
    )


def run(command):
    result = subprocess.run(command, capture_output=True, text=True, check=False)
    if result.returncode:
        raise RuntimeError(result.stdout + result.stderr)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archive", type=Path)
    parser.add_argument("--generator", help="Optional CMake generator")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="tmp1x2-package-") as temporary:
        workspace = Path(temporary)
        library = workspace / "renamed-library"
        files = set()
        with tarfile.open(args.archive) as archive:
            for member in archive.getmembers():
                path = PurePosixPath(member.name)
                if (path.is_absolute() or PureWindowsPath(member.name).drive
                        or "\\" in member.name or ":" in member.name
                        or ".." in path.parts or not path.parts):
                    raise ValueError(f"Unsafe package path: {member.name}")
                if forbidden(path):
                    raise ValueError(f"Unexpected package artifact: {member.name}")
                if member.isdir():
                    continue
                if not member.isfile():
                    raise ValueError(f"Unexpected package artifact: {member.name}")
                if path.as_posix() in files:
                    raise ValueError(f"Duplicate package path: {member.name}")
                files.add(path.as_posix())
                target = library.joinpath(*path.parts)
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(archive.extractfile(member).read())
        missing = REQUIRED - files
        if missing:
            raise ValueError(f"Required package files missing: {sorted(missing)}")
        metadata = json.loads((library / "library.json").read_text(encoding="utf-8"))
        if metadata["name"] != "TMP1x2":
            raise ValueError("Wrong library in archive")
        (workspace / "main.cpp").write_text(
            '''#include <TMP1x2/CommandTable.h>
#include <TMP1x2/Config.h>
#include <TMP1x2/Status.h>
#include <TMP1x2/Version.h>
#include <TMP1x2/TMP1x2.h>
#include <TMP1x2/BusOperations.h>
int main() {
  TMP1x2::TMP1x2 sensor;
  TMP1x2::BusOperations::AlertResponse response;
  const auto decoded = TMP1x2::BusOperations::decodeAlertResponse(0x90, response);
  TMP1x2::Sample sample;
  const auto temperature = TMP1x2::TMP1x2::decodeTemperature(0x1900, sample);
  uint16_t configuration = 0;
  const auto encoded = TMP1x2::TMP1x2::encodeConfiguration(TMP1x2::Config{}, configuration);
  TMP1x2::OperationToken token = 123;
  const auto operation = sensor.startInitialize(0, 100, token);
  sensor.end();
  return sensor.isBound() || !decoded.ok() || response.address != 0x48 ||
      !temperature.ok() || sample.celsius != 25 || !encoded.ok() || configuration != 0x6080 ||
      !operation.is(TMP1x2::Err::NOT_BOUND) || token != 123;
}
''', encoding="utf-8")
        (workspace / "CMakeLists.txt").write_text(
            '''cmake_minimum_required(VERSION 3.16)
project(tmp1x2_package_consumer LANGUAGES CXX)
add_subdirectory(renamed-library)
add_executable(package_consumer main.cpp)
target_link_libraries(package_consumer PRIVATE TMP1x2)
enable_testing()
add_test(NAME package_consumer COMMAND package_consumer)
''', encoding="utf-8")
        build = workspace / "build"
        configure = ["cmake", "-S", str(workspace), "-B", str(build)]
        if args.generator:
            configure += ["-G", args.generator]
        run(configure)
        run(["cmake", "--build", str(build), "--config", "Release"])
        run(["ctest", "--test-dir", str(build), "-C", "Release", "--output-on-failure"])
        print(f"Package check passed: TMP1x2 {metadata['version']}, {len(files)} files; "
              "isolated renamed CMake consumer compiled, linked and ran")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
