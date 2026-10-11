#!/usr/bin/env python3
"""Check release metadata and the framework/ownership boundary."""
import json
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
SOURCE_SUFFIXES = {".h", ".hpp", ".hh", ".hxx", ".c", ".cc", ".cpp", ".cxx"}
FRAMEWORK_INCLUDE = re.compile(
    r'^\s*#\s*include\s*[<"](?:Arduino(?:\.h|/)|Wire(?:\.h|/)|'
    r'esp_|driver/|freertos/|FreeRTOS\.h|sdkconfig\.h)', re.M)


def code_only(text):
    # Diagnostic strings and comments are not C++ framework dependencies.
    return re.sub(r"""/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*'""",
                  "", text, flags=re.S)


def main():
    errors = []
    # The shared example command processor must stay portable too: platform
    # adaptation belongs only in the framework-specific application entrypoints.
    for folder in ("include", "src", "examples/common"):
        for path in (ROOT / folder).rglob("*"):
            if path.suffix not in SOURCE_SUFFIXES:
                continue
            text = path.read_text(encoding="utf-8")
            if FRAMEWORK_INCLUDE.search(text):
                errors.append(f"framework include in {path.relative_to(ROOT)}")
            code = code_only(text)
            if re.search(r"\b(?:delay|malloc|calloc|realloc|printf)\s*\(|\bnew\s+\w|\b(?:Serial|Wire)\s*[.]", code):
                errors.append(f"allocation/logging/framework call in {path.relative_to(ROOT)}")
    metadata = json.loads((ROOT / "library.json").read_text())
    assert metadata["name"] == "TMP1x2"
    result = subprocess.run([sys.executable, str(ROOT / "scripts/generate_version.py"), "check"], cwd=ROOT)
    if result.returncode:
        errors.append("generated release metadata mismatch")
    idf = ROOT / "examples/esp_idf/basic/main/main.cpp"
    if idf.exists():
        text = idf.read_text(encoding="utf-8")
        if (re.search(r'#\s*include\s*[<"](?:Arduino|Wire)', text) or
                re.search(r'\b(?:Serial|TwoWire|String)\b', code_only(text))):
            errors.append("Arduino dependency in native IDF example")
    if errors:
        print("\n".join(errors), file=sys.stderr)
        return 1
    print("Release and framework boundary contracts passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
