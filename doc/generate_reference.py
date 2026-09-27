#!/usr/bin/env python3
"""Apply Ivy's layout overrides to this Doxygen version's complete defaults.

A partial layout can hide unspecified page kinds instead of inheriting defaults.
Merging explicitly supports both older (1.9.x) and newer Doxygen releases.
"""

from pathlib import Path
import subprocess
import tempfile
import xml.etree.ElementTree as ET


def merge_layout(default: ET.Element, override: ET.Element) -> None:
    """Put customized sections first, retaining any remaining default sections."""
    default.attrib.update(override.attrib)
    ordered = []
    for section in override:
        original = default.find(section.tag)
        if original is None:
            raise ValueError(f"Doxygen has no layout section {default.tag}/{section.tag}")
        merge_layout(original, section)
        ordered.append(original)
    default[:] = ordered + [section for section in default if section not in ordered]


def generate() -> None:
    root = Path(__file__).resolve().parent.parent
    with tempfile.TemporaryDirectory(prefix="ivy-doxygen-layout.") as temporary:
        layout = Path(temporary) / "layout.xml"
        subprocess.run(["doxygen", "-l", str(layout)], cwd=root, check=True)
        defaults = ET.parse(layout)
        tree = defaults.getroot()
        overrides = ET.parse(root / "doc/DoxygenLayout.xml").getroot()
        for section in overrides:
            original = tree.find(section.tag)
            if original is None:
                raise ValueError(f"Doxygen has no layout section {section.tag}")
            merge_layout(original, section)
        defaults.write(layout, encoding="utf-8", xml_declaration=True)
        config = (root / "Doxyfile").read_text() + f'\nLAYOUT_FILE = "{layout}"\n'
        subprocess.run(["doxygen", "-"], cwd=root, input=config, text=True, check=True)


if __name__ == "__main__":
    generate()
