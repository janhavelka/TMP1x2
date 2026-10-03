#!/usr/bin/env python3
"""Build and check HTML API docs. Requires Doxygen on PATH; uses no Python packages.

Run from any directory: python tools/check_doxygen.py
Successful output replaces build/doxygen; open build/doxygen/html/index.html.
Checks generated local links, public API visibility, and documentation inputs.
External links are not fetched. This is not a source-code or hardware test.
"""
from html.parser import HTMLParser
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
from urllib.parse import unquote, urlsplit
import xml.etree.ElementTree as ET


ROOT = Path(__file__).resolve().parents[1]


class HtmlPage(HTMLParser):
    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.links = []
        self.anchors = set()

    def handle_starttag(self, tag, attrs):
        values = dict(attrs)
        for name in ("href", "src"):
            if values.get(name):
                self.links.append(values[name])
        if values.get("id"):
            self.anchors.add(values["id"])
        if tag == "a" and values.get("name"):
            self.anchors.add(values["name"])


def check_html(html):
    pages = {}
    for path in html.rglob("*.html"):
        page = HtmlPage()
        page.feed(path.read_text(encoding="utf-8"))
        pages[path.resolve()] = page
    errors = set()
    for path, page in pages.items():
        for link in page.links:
            url = urlsplit(link)
            if url.scheme or url.netloc:
                continue
            target = (path.parent / unquote(url.path)).resolve() if url.path else path
            label = f"{path.relative_to(html)}: {link}"
            if not target.is_relative_to(html) or not target.is_file():
                errors.add(f"Missing local document or asset: {label}")
            elif url.fragment and target in pages and unquote(url.fragment) not in pages[target].anchors:
                errors.add(f"Missing local anchor: {label}")
    if (html / "index.html") not in pages:
        errors.add("Missing HTML landing page")
    return pages, errors


def check_api(output, pages):
    html = output / "html"
    xml = output / "xml"
    errors = set()
    files = set()
    public_types = set()
    for entry in ET.parse(xml / "index.xml").getroot().findall("compound"):
        refid = entry.get("refid")
        compound = ET.parse(xml / f"{refid}.xml").getroot().find("compounddef")
        name = compound.findtext("compoundname")
        kind = compound.get("kind")
        if kind == "file":
            files.add(compound.find("location").get("file").replace("\\", "/"))
        if kind not in {"class", "struct", "namespace"}:
            continue
        path = html / f"{refid}.html"
        if compound.get("prot") == "private":
            if path in pages:
                errors.add(f"Private type exposed in HTML: {name}")
            continue
        if kind in {"class", "struct"}:
            public_types.add(name)
        if path not in pages:
            errors.add(f"Public API page missing: {name}")
            continue
        for member in compound.findall("sectiondef/memberdef"):
            member_id = member.get("id")
            anchor = member_id.removeprefix(refid + "_1")
            visible = anchor in pages[path].anchors
            member_name = member.findtext("name")
            if member.get("prot") == "private" and visible:
                errors.add(f"Private member exposed in HTML: {name}::{member_name}")
            elif member.get("prot") == "public" and not visible:
                errors.add(f"Public member missing in HTML: {name}::{member_name}")
    required_types = {"TMP1x2::TMP1x2", "TMP1x2::Config", "TMP1x2::Status", "TMP1x2::Sample",
                      "TMP1x2::OperationResult", "TMP1x2::BusOperations::AlertResponse"}
    for missing in sorted(required_types - public_types):
        errors.add(f"Required public type missing: {missing}")
    inputs = [*ROOT.glob("include/TMP1x2/*.h"), *ROOT.glob("docs/*.md"),
              ROOT / "README.md", ROOT / "CHANGELOG.md", ROOT / "examples/README.md",
              ROOT / "docs/reference/README.md"]
    expected = {path.relative_to(ROOT).as_posix() for path in inputs}
    for missing in sorted(expected - files):
        errors.add(f"Documentation input missing: {missing}")
    for unexpected in sorted(files - expected):
        errors.add(f"Unexpected implementation or vendor input: {unexpected}")
    return errors


def main():
    executable = shutil.which("doxygen")
    if executable is None:
        print("Doxygen is required. Install Doxygen and add its executable to PATH "
              "(Ubuntu: sudo apt-get install doxygen).", file=sys.stderr)
        return 1
    build = ROOT / "build"
    build.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="doxygen-check-", dir=build) as temporary:
        workspace = Path(temporary).resolve()
        output = workspace / "generated"
        # XML is temporary validation data. Doxygen includes private symbols in
        # XML even when HTML extraction excludes them, so do not publish it.
        configuration = (ROOT / "Doxyfile").read_text(encoding="utf-8")
        configuration += (f'\nOUTPUT_DIRECTORY = "{output.as_posix()}"\n'
                          'GENERATE_XML = YES\nWARN_LOGFILE =\nWARN_AS_ERROR = YES\n')
        try:
            result = subprocess.run([executable, "-"], input=configuration, cwd=ROOT,
                                    text=True, capture_output=True, encoding="utf-8", errors="replace",
                                    timeout=120)
        except subprocess.TimeoutExpired:
            print("Doxygen generation exceeded the 120-second timeout.", file=sys.stderr)
            return 1
        if result.returncode or result.stderr.strip():
            print(result.stdout + result.stderr, file=sys.stderr)
            return 1
        pages, errors = check_html(output / "html")
        errors.update(check_api(output, pages))
        if errors:
            print("\n".join(sorted(errors)), file=sys.stderr)
            return 1
        destination = build / "doxygen"
        # Restrict cleanup to this generated directory, including on Windows.
        if destination.is_symlink() or destination.resolve().parent != build.resolve():
            raise ValueError(f"Refusing to replace documentation outside {build}")
        if destination.exists():
            shutil.rmtree(destination)
        destination.mkdir()
        shutil.move(str(output / "html"), str(destination / "html"))
        print(f"Doxygen checks passed: {len(pages)} HTML pages, public API, local links and assets")
        print(f"Open {destination / 'html/index.html'}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, ET.ParseError) as error:
        print(f"Doxygen check failed: {error}", file=sys.stderr)
        raise SystemExit(1)
