#!/usr/bin/env python3
"""Regression checks for the documentation validator; Doxygen is not required."""
import contextlib
import io
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch
import xml.etree.ElementTree as ET

import check_doxygen


class HtmlValidation(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="tmp1x2-doc-links-")
        self.addCleanup(self.temporary.cleanup)
        self.html = Path(self.temporary.name).resolve() / "html"
        self.html.mkdir()

    def write(self, name, content):
        path = self.html / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content, encoding="utf-8")

    def errors(self):
        return check_doxygen.check_html(self.html)[1]

    def test_local_pages_anchors_assets_and_external_links(self):
        self.write("index.html", '<a id="top"></a><a href="#top">Top</a>'
                   '<a href="guide.html?view=all#read">Guide</a>'
                   '<a href="https://example.invalid/unfetched#external">External</a>'
                   '<a href="mailto:maintainer@example.invalid">Mail</a>'
                   '<script src="assets/ui.js"></script><img src="figure%201.svg">')
        self.write("guide.html", '<a name="read"></a><a href="index.html#top">Home</a>')
        self.write("assets/ui.js", "")
        self.write("figure 1.svg", "<svg></svg>")
        self.assertEqual(self.errors(), set())

    def test_missing_anchor_fails(self):
        self.write("index.html", '<a href="guide.html#missing">Guide</a>')
        self.write("guide.html", '<h1 id="read">Guide</h1>')
        self.assertEqual(self.errors(), {"Missing local anchor: index.html: guide.html#missing"})

    def test_missing_file_and_asset_fail(self):
        self.write("index.html", '<a href="guide.html">Guide</a><img src="figure.svg">')
        self.assertEqual(len(self.errors()), 2)
        self.assertTrue(all("Missing local document or asset" in error for error in self.errors()))

    def test_existing_file_outside_documentation_fails(self):
        (self.html.parent / "outside.html").write_text("<h1>Outside</h1>", encoding="utf-8")
        self.write("index.html", '<a href="../outside.html">Nonportable link</a>')
        self.assertEqual(self.errors(), {"Missing local document or asset: index.html: ../outside.html"})

    def test_missing_landing_page_fails(self):
        self.write("guide.html", "<h1>Guide</h1>")
        self.assertIn("Missing HTML landing page", self.errors())


class ToolValidation(unittest.TestCase):
    def test_missing_doxygen_fails_with_installation_instruction(self):
        output = io.StringIO()
        with patch.object(check_doxygen.shutil, "which", return_value=None), contextlib.redirect_stderr(output):
            self.assertEqual(check_doxygen.main(), 1)
        self.assertIn("Install Doxygen", output.getvalue())
        self.assertIn("PATH", output.getvalue())

    def failed_generation(self, result=None, error=None):
        with tempfile.TemporaryDirectory(prefix="tmp1x2-doc-failure-") as temporary:
            root = Path(temporary)
            (root / "Doxyfile").write_text("PROJECT_NAME = test\n", encoding="utf-8")
            previous = root / "build/doxygen/html/index.html"
            previous.parent.mkdir(parents=True)
            previous.write_text("Previous validated documentation", encoding="utf-8")
            output = io.StringIO()
            with (patch.object(check_doxygen, "ROOT", root),
                  patch.object(check_doxygen.shutil, "which", return_value="doxygen"),
                  patch.object(check_doxygen.subprocess, "run", return_value=result, side_effect=error),
                  contextlib.redirect_stderr(output)):
                self.assertEqual(check_doxygen.main(), 1)
            self.assertEqual(previous.read_text(encoding="utf-8"), "Previous validated documentation")
            return output.getvalue()

    def test_failed_generation_preserves_previous_output(self):
        result = subprocess.CompletedProcess(["doxygen", "-"], 1, "", "warning: broken reference")
        self.assertIn("broken reference", self.failed_generation(result=result))

    def test_stalled_generation_is_bounded_and_preserves_previous_output(self):
        error = subprocess.TimeoutExpired(["doxygen", "-"], 120)
        self.assertIn("120-second timeout", self.failed_generation(error=error))


class ApiValidation(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="tmp1x2-doc-api-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.output = self.root / "output"
        self.html = self.output / "html"
        self.xml = self.output / "xml"
        self.html.mkdir(parents=True)
        self.xml.mkdir()
        self.index = ET.Element("doxygenindex")
        self.pages = {}
        # Required type names test that the API index cannot silently disappear.
        for index, name in enumerate(("TMP1x2::TMP1x2", "TMP1x2::Config", "TMP1x2::Status",
                                      "TMP1x2::Sample", "TMP1x2::OperationResult",
                                      "TMP1x2::BusOperations::AlertResponse")):
            refid = "type" + str(index)
            node = self.compound(refid, "class", name, prot="public")
            if index == 0:
                section = ET.SubElement(node, "sectiondef", kind="public-func")
                member = ET.SubElement(section, "memberdef", id=refid + "_1aread", prot="public")
                ET.SubElement(member, "name").text = "readSample"
                section = ET.SubElement(node, "sectiondef", kind="private-attrib")
                member = ET.SubElement(section, "memberdef", id=refid + "_1aconfig", prot="private")
                ET.SubElement(member, "name").text = "_config"
            self.write_compound(refid, node)
            page = check_doxygen.HtmlPage()
            page.feed('<a id="aread"></a>' if index == 0 else "")
            self.pages[self.html / f"{refid}.html"] = page
        node = self.compound("private_type", "struct", "TMP1x2::TMP1x2::ApplyState", prot="private")
        self.write_compound("private_type", node)
        for index, name in enumerate(("README.md", "CHANGELOG.md", "examples/README.md",
                                      "docs/reference/README.md")):
            self.add_file("file" + str(index), name)

    def compound(self, refid, kind, name, **attributes):
        ET.SubElement(self.index, "compound", refid=refid, kind=kind)
        node = ET.Element("compounddef", id=refid, kind=kind, **attributes)
        ET.SubElement(node, "compoundname").text = name
        return node

    def write_compound(self, refid, node):
        root = ET.Element("doxygen")
        root.append(node)
        ET.ElementTree(root).write(self.xml / f"{refid}.xml", encoding="utf-8")

    def add_file(self, refid, name):
        node = self.compound(refid, "file", Path(name).name)
        ET.SubElement(node, "location", file=name)
        self.write_compound(refid, node)

    def errors(self):
        ET.ElementTree(self.index).write(self.xml / "index.xml", encoding="utf-8")
        with patch.object(check_doxygen, "ROOT", self.root):
            return check_doxygen.check_api(self.output, self.pages)

    def test_public_members_visible_and_private_members_hidden(self):
        self.assertEqual(self.errors(), set())

    def test_public_member_hidden_fails(self):
        self.pages[self.html / "type0.html"].anchors.clear()
        self.assertEqual(self.errors(), {"Public member missing in HTML: TMP1x2::TMP1x2::readSample"})

    def test_private_member_visible_fails(self):
        self.pages[self.html / "type0.html"].anchors.add("aconfig")
        self.assertEqual(self.errors(), {"Private member exposed in HTML: TMP1x2::TMP1x2::_config"})

    def test_private_type_visible_fails(self):
        self.pages[self.html / "private_type.html"] = check_doxygen.HtmlPage()
        self.assertEqual(self.errors(), {"Private type exposed in HTML: TMP1x2::TMP1x2::ApplyState"})

    def test_implementation_or_vendor_input_fails(self):
        self.add_file("implementation", "src/TMP1x2.cpp")
        self.add_file("vendor", "docs/reference/ti-linux/drivers/hwmon/tmp102.c")
        errors = self.errors()
        self.assertEqual(len(errors), 2)
        self.assertTrue(all("Unexpected implementation or vendor input" in error for error in errors))


if __name__ == "__main__":
    unittest.main()
