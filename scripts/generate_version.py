#!/usr/bin/env python3
"""Synchronize Version.h from library.json and expose build metadata via macros.

Default behavior:
- when run by PlatformIO as an extra script: sync generated headers if needed and
  inject build metadata defines into the compile environment
- when run standalone: sync generated headers if needed

Standalone commands:
  sync
      Regenerate generated headers only if source metadata changed.
  check
      Exit with code 1 when generated headers are out of date.
  bump patch|minor|major
      Update library.json, then regenerate generated headers.
  set X.Y.Z
      Set an explicit semantic version, then regenerate generated headers.
"""

from __future__ import annotations

import json
import re
import subprocess
import sys
from datetime import datetime
from pathlib import Path
from typing import Dict, List, Optional, Tuple

ENV = None
try:
    Import("env")  # type: ignore[name-defined]  # PlatformIO / SCons built-in
    ENV = env  # type: ignore[name-defined]
except Exception:
    ENV = None

SEMVER_RE = re.compile(r"^(\d+)\.(\d+)\.(\d+)$")


def _find_project_root() -> Path:
    if ENV is not None:
        return Path(ENV["PROJECT_DIR"]).resolve()
    return Path(__file__).resolve().parent.parent


def _read_text(path: Path) -> str:
    with open(path, "r", encoding="utf-8") as handle:
        return handle.read()


def _write_text(path: Path, content: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(content)


def _load_library_json(path: Path) -> Dict[str, object]:
    with open(path, "r", encoding="utf-8") as handle:
        return json.load(handle)


def _write_library_json(path: Path, data: Dict[str, object]) -> None:
    with open(path, "w", encoding="utf-8", newline="\n") as handle:
        json.dump(data, handle, indent=2)
        handle.write("\n")


def _parse_semver(version: str) -> Tuple[int, int, int]:
    match = SEMVER_RE.match(version)
    if not match:
        raise ValueError(f"Invalid semantic version: {version}")
    return tuple(int(part) for part in match.groups())


def _bump_semver(version: str, part: str) -> str:
    major, minor, patch = _parse_semver(version)
    if part == "major":
        return f"{major + 1}.0.0"
    if part == "minor":
        return f"{major}.{minor + 1}.0"
    if part == "patch":
        return f"{major}.{minor}.{patch + 1}"
    raise ValueError(f"Unknown bump target: {part}")


def _resolve_namespace_dir(project_root: Path) -> Path:
    include_root = project_root / "include"
    if not include_root.exists():
        raise FileNotFoundError(f"Missing include directory: {include_root}")

    candidates = [path for path in include_root.iterdir() if path.is_dir()]
    version_candidates = [path for path in candidates if (path / "Version.h").exists()]

    if len(version_candidates) == 1:
        return version_candidates[0]
    if len(candidates) == 1:
        return candidates[0]
    if len(version_candidates) > 1:
        raise RuntimeError(
            f"Multiple Version.h candidates under {include_root}: "
            + ", ".join(str(path) for path in version_candidates)
        )
    raise RuntimeError(
        f"Unable to resolve namespace directory under {include_root}; found: "
        + ", ".join(path.name for path in candidates)
    )


def _macro_prefix(namespace: str) -> str:
    return "".join(ch if ch.isalnum() else "_" for ch in namespace).upper()


def _get_git_info(project_root: Path) -> Tuple[str, str]:
    try:
        commit_result = subprocess.run(
            ["git", "rev-parse", "--short", "HEAD"],
            cwd=project_root,
            capture_output=True,
            text=True,
            timeout=2,
            check=False,
        )
        commit = commit_result.stdout.strip() if commit_result.returncode == 0 else "unknown"

        status_result = subprocess.run(
            ["git", "status", "--porcelain", "--untracked-files=no"],
            cwd=project_root,
            capture_output=True,
            text=True,
            timeout=2,
            check=False,
        )
        git_status = "dirty" if status_result.stdout.strip() else "clean"
        return commit or "unknown", git_status
    except Exception:
        return "unknown", "unknown"


def _cpp_string_literal(value: str) -> str:
    escaped = value.replace("\\", "\\\\").replace('"', '\\"')
    return f'\\"{escaped}\\"'


def _append_build_metadata_defines(namespace: str, project_root: Path) -> None:
    if ENV is None:
        return

    now = datetime.now()
    build_date = now.strftime("%Y-%m-%d")
    build_time = now.strftime("%H:%M:%S")
    build_timestamp = f"{build_date} {build_time}"
    git_commit, git_status = _get_git_info(project_root)
    prefix = _macro_prefix(namespace)

    ENV.Append(
        CPPDEFINES=[
            (f"{prefix}_BUILD_DATE", _cpp_string_literal(build_date)),
            (f"{prefix}_BUILD_TIME", _cpp_string_literal(build_time)),
            (f"{prefix}_BUILD_TIMESTAMP", _cpp_string_literal(build_timestamp)),
            (f"{prefix}_GIT_COMMIT", _cpp_string_literal(git_commit)),
            (f"{prefix}_GIT_STATUS", _cpp_string_literal(git_status)),
        ]
    )


def _render_version_header(namespace: str, version: str) -> str:
    major, minor, patch = _parse_semver(version)
    version_code = major * 10000 + minor * 100 + patch
    prefix = _macro_prefix(namespace)

    return f'''/**
 * @file Version.h
 * @brief Version information for {namespace}.
 *
 * This file is AUTO-GENERATED by scripts/generate_version.py from library.json.
 * DO NOT EDIT MANUALLY. Update version metadata via library.json or
 * scripts/generate_version.py bump/set commands.
 */

#pragma once

#include <stdint.h>

#ifndef {prefix}_VERSION_STRING
#define {prefix}_VERSION_STRING "{version}"
#endif

#ifndef {prefix}_BUILD_DATE
#define {prefix}_BUILD_DATE "unknown"
#endif

#ifndef {prefix}_BUILD_TIME
#define {prefix}_BUILD_TIME "unknown"
#endif

#ifndef {prefix}_BUILD_TIMESTAMP
#define {prefix}_BUILD_TIMESTAMP "unknown"
#endif

#ifndef {prefix}_GIT_COMMIT
#define {prefix}_GIT_COMMIT "unknown"
#endif

#ifndef {prefix}_GIT_STATUS
#define {prefix}_GIT_STATUS "unknown"
#endif

#ifndef {prefix}_VERSION_FULL
#define {prefix}_VERSION_FULL {prefix}_VERSION_STRING " (" {prefix}_GIT_COMMIT ", " {prefix}_BUILD_TIMESTAMP ", " {prefix}_GIT_STATUS ")"
#endif

namespace {namespace} {{

/// @brief Major version (breaking changes).
static constexpr uint16_t VERSION_MAJOR = {major};

/// @brief Minor version (backward-compatible features).
static constexpr uint16_t VERSION_MINOR = {minor};

/// @brief Patch version (backward-compatible fixes).
static constexpr uint16_t VERSION_PATCH = {patch};

/// @brief Full semantic version string.
static constexpr const char* VERSION = {prefix}_VERSION_STRING;

/// @brief Encoded version for numeric comparison: MAJOR*10000 + MINOR*100 + PATCH.
static constexpr uint32_t VERSION_CODE = {version_code};

/// @brief Backward-compatible alias used by older repositories.
static constexpr int VERSION_INT = {version_code};

/// @brief Build date string.
static constexpr const char* BUILD_DATE = {prefix}_BUILD_DATE;

/// @brief Build time string.
static constexpr const char* BUILD_TIME = {prefix}_BUILD_TIME;

/// @brief Build timestamp string.
static constexpr const char* BUILD_TIMESTAMP = {prefix}_BUILD_TIMESTAMP;

/// @brief Git commit string.
static constexpr const char* GIT_COMMIT = {prefix}_GIT_COMMIT;

/// @brief Git working tree status string.
static constexpr const char* GIT_STATUS = {prefix}_GIT_STATUS;

/// @brief Full version string with build metadata.
static constexpr const char* VERSION_FULL = {prefix}_VERSION_FULL;

}}  // namespace {namespace}
'''


def _expected_outputs(project_root: Path) -> Dict[Path, str]:
    library_json = project_root / "library.json"
    library_data = _load_library_json(library_json)
    version = str(library_data.get("version", "0.0.0"))
    namespace_dir = _resolve_namespace_dir(project_root)
    namespace = namespace_dir.name

    outputs = {
        namespace_dir / "Version.h": _render_version_header(namespace, version),
    }

    def replace_required(path: Path, pattern: str, replacement: str) -> str:
        updated, count = re.subn(
            pattern,
            replacement,
            _read_text(path),
            count=1,
            flags=re.MULTILINE,
        )
        if count != 1:
            raise RuntimeError(f"Version metadata pattern missing from {path}")
        return updated

    idf_component = project_root / "idf_component.yml"
    if idf_component.exists():
        outputs[idf_component] = replace_required(
            idf_component,
            r'^version:\s*"[^"]+"\s*$',
            f'version: "{version}"',
        )

    doxyfile = project_root / "Doxyfile"
    if doxyfile.exists():
        outputs[doxyfile] = replace_required(
            doxyfile,
            r'^PROJECT_NUMBER\s*=\s*"[^"]+"\s*$',
            f'PROJECT_NUMBER         = "{version}"',
        )

    security = project_root / "SECURITY.md"
    if security.exists():
        major, minor, _ = _parse_semver(version)
        outputs[security] = replace_required(
            security,
            r'^\|\s*\d+\.\d+\.x\s*\|\s*:white_check_mark:\s*\|$',
            f'| {major}.{minor}.x   | :white_check_mark: |',
        )

    return outputs


def _sync_outputs(project_root: Path, check_only: bool, quiet: bool) -> bool:
    outputs = _expected_outputs(project_root)
    clean = True

    for path, expected in outputs.items():
        current = _read_text(path) if path.exists() else None
        if current == expected:
            if not quiet:
                print(f"Up to date: {path}")
            continue

        clean = False
        if check_only:
            print(f"Out of date: {path}")
            continue

        _write_text(path, expected)
        print(f"Updated: {path}")

    return clean


def _set_version(project_root: Path, new_version: str) -> str:
    _parse_semver(new_version)
    library_json = project_root / "library.json"
    data = _load_library_json(library_json)
    data["version"] = new_version
    _write_library_json(library_json, data)
    return new_version


def _usage() -> str:
    return (
        "Usage:\n"
        "  scripts/generate_version.py sync\n"
        "  scripts/generate_version.py check\n"
        "  scripts/generate_version.py bump patch|minor|major\n"
        "  scripts/generate_version.py set X.Y.Z"
    )


def main(args: List[str]) -> int:
    project_root = _find_project_root()
    namespace = _resolve_namespace_dir(project_root).name
    _append_build_metadata_defines(namespace, project_root)

    if not args:
        _sync_outputs(project_root, check_only=False, quiet=True)
        return 0

    command = args[0]
    if command == "sync":
        _sync_outputs(project_root, check_only=False, quiet=False)
        return 0
    if command == "check":
        return 0 if _sync_outputs(project_root, check_only=True, quiet=False) else 1
    if command == "bump":
        if len(args) != 2:
            print(_usage(), file=sys.stderr)
            return 2
        current_version = str(_load_library_json(project_root / "library.json").get("version", "0.0.0"))
        new_version = _bump_semver(current_version, args[1])
        _set_version(project_root, new_version)
        _sync_outputs(project_root, check_only=False, quiet=False)
        print(f"Version bumped: {current_version} -> {new_version}")
        return 0
    if command == "set":
        if len(args) != 2:
            print(_usage(), file=sys.stderr)
            return 2
        new_version = _set_version(project_root, args[1])
        _sync_outputs(project_root, check_only=False, quiet=False)
        print(f"Version set: {new_version}")
        return 0

    print(_usage(), file=sys.stderr)
    return 2


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))

result = main([])
if result != 0:
    raise SystemExit(result)
