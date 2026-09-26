"""Exercise release metadata changes in an isolated copy, never the checkout."""
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


class VersionSyncTests(unittest.TestCase):
    def test_version_change_updates_all_build_systems(self):
        with tempfile.TemporaryDirectory(prefix="tmp1x2-version-") as directory:
            root = Path(directory)
            for name in ("library.json", "CMakeLists.txt", "idf_component.yml", "Doxyfile",
                         "include/TMP1x2/Version.h", "scripts/generate_version.py"):
                destination = root / name
                destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(ROOT / name, destination)

            metadata = json.loads((root / "library.json").read_text(encoding="utf-8"))
            metadata["version"] = "2.7.13"
            (root / "library.json").write_text(json.dumps(metadata), encoding="utf-8")

            def run(command):
                return subprocess.run(
                    [sys.executable, str(root / "scripts/generate_version.py"), command],
                    cwd=root, capture_output=True, text=True, check=False,
                )

            stale = run("check")
            self.assertEqual(stale.returncode, 1, stale.stdout + stale.stderr)
            self.assertIn("CMakeLists.txt", stale.stdout)
            synced = run("sync")
            self.assertEqual(synced.returncode, 0, synced.stdout + synced.stderr)
            current = run("check")
            self.assertEqual(current.returncode, 0, current.stdout + current.stderr)
            self.assertIn("project(TMP1x2 VERSION 2.7.13 LANGUAGES CXX)",
                          (root / "CMakeLists.txt").read_text(encoding="utf-8"))
            self.assertIn('version: "2.7.13"',
                          (root / "idf_component.yml").read_text(encoding="utf-8"))
            self.assertIn('#define TMP1X2_VERSION_STRING "2.7.13"',
                          (root / "include/TMP1x2/Version.h").read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()
