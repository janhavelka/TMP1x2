#!/usr/bin/env python3
"""Compile native IDF example against SDK headers from a PlatformIO compilation DB.

This checks API/header compatibility only, NOT native IDF CMake or firmware linking.
First run `scripts/pio.cmd run -e esp32s3dev -t compiledb` (or the S2 environment).
Arduino include paths and preprocessor defines are deliberately discarded.
"""
import json
from pathlib import Path
import shlex
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def main():
    database = json.loads((ROOT / "compile_commands.json").read_text())
    entry = next(item for item in database if "01_basic_bringup_cli" in item["file"])
    args = entry.get("arguments") or shlex.split(entry["command"], posix=sys.platform != "win32")
    includes = [arg.strip('"') for arg in args if arg.startswith("-I") and
                ("framework-arduinoespressif32-libs" in arg or "framework-espidf" in arg)]
    includes = ["-isystem" + arg[2:] for arg in includes]
    target = "esp32s2" if "esp32s2" in args[0] else "esp32s3"
    command = [args[0], "-std=c++17", "-mlongcalls", "-fsyntax-only", "-Wall", "-Wextra", "-Werror",
               "-DESP_PLATFORM", "-Iinclude", "-Iexamples/common", *includes,
               "examples/esp_idf/basic/main/main.cpp", "examples/common/Tmp1x2Cli.cpp", "src/TMP1x2.cpp"]
    response = ROOT / "build" / f"idf-sdk-{target}.rsp"
    response.parent.mkdir(exist_ok=True)
    response.write_text("\n".join('"' + arg.replace("\\", "/").replace('"', '\\"') + '"'
                                  for arg in command[1:]), encoding="utf-8")
    result = subprocess.run([command[0], "@" + str(response)], cwd=ROOT)
    if result.returncode == 0:
        print(f"Native IDF example SDK header compilation passed ({target}); no IDF link/CMake claim")
    return result.returncode


if __name__ == "__main__":
    raise SystemExit(main())
