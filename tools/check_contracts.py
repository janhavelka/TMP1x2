#!/usr/bin/env python3
"""Check release metadata and the framework/ownership boundary."""
import json
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def main():
    errors = []
    for folder in ("include", "src"):
        for path in (ROOT / folder).rglob("*"):
            if path.suffix not in (".h", ".cpp"):
                continue
            text = path.read_text(encoding="utf-8")
            if re.search(r"^\s*#\s*include\s*[<\"](?:Arduino|Wire|esp_|driver/|freertos/)", text, re.M):
                errors.append(f"framework include in {path.relative_to(ROOT)}")
            code = re.sub(r"/\*.*?\*/|//[^\n]*", "", text, flags=re.S)
            if re.search(r"\b(?:delay|malloc|calloc|realloc|printf)\s*\(|\bnew\s+\w|\b(?:Serial|Wire)\s*[.]", code):
                errors.append(f"allocation/logging/framework call in {path.relative_to(ROOT)}")
    metadata = json.loads((ROOT / "library.json").read_text())
    assert metadata["name"] == "TMP1x2"
    result = subprocess.run([sys.executable, str(ROOT / "scripts/generate_version.py"), "check"], cwd=ROOT)
    if result.returncode:
        errors.append("generated release metadata mismatch")
    idf = ROOT / "examples/esp_idf/basic/main/main.cpp"
    if idf.exists() and re.search(r'#include\s*[<"](?:Arduino|Wire)|\b(?:Serial|TwoWire|String)\b', idf.read_text()):
        errors.append("Arduino dependency in native IDF example")
    if errors:
        print("\n".join(errors), file=sys.stderr)
        return 1
    print("Release and framework boundary contracts passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
