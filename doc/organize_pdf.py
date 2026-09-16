#!/usr/bin/env python3
"""Arrange Doxygen's generated reference into separate C and C++ PDF parts.

The XML index supplies each generated input's kind and name. Retain all of the
original LaTeX inputs and labels; only their order and chapter headings change.
This also avoids Doxygen 1.9.x's broken labels for INLINE_GROUPED_CLASSES.
"""

from collections import Counter
from pathlib import Path
import re
import sys
import xml.etree.ElementTree as ET

from reference_order import compound_priority


def place_context_filters(output: Path) -> None:
    """Keep each filter subgroup between ordinary messages and optional events."""
    for group, filters, first_event in [
        ("group__ivy__context__api", "group__ivy__filters", "IvyContextSetBindCallback"),
        ("group__ivy__legacy__api", "group__ivy__legacy__filters", "IvySetBindCallback"),
    ]:
        compound = ET.parse(output / "xml" / (group + ".xml")).getroot()
        event = next(member for member in compound.findall(".//sectiondef/memberdef")
                     if member.findtext("name") == first_event)
        # XML separates compound/member IDs with "_1"; LaTeX uses "_".
        member_id = event.attrib["id"].removeprefix(group + "_1")
        anchor = r"\label{" + group + "_" + member_id + "}"
        include = r"\input{" + filters + "}"
        path = output / "latex" / (group + ".tex")
        text = path.read_text()
        if text.count(include) != 1 or text.count(anchor) != 1:
            raise ValueError(f"Unexpected filter/event layout in {group}")
        text = text.replace(include, "")
        introduction = include + "\n" + r"\doxysubsubsection{Events and optional operations}" + "\n"
        path.write_text(text.replace(anchor, introduction + anchor, 1))


def organize(output: Path) -> None:
    place_context_filters(output)
    reference = output / "latex/refman.tex"
    text = reference.read_text()
    begin = "%--- Begin generated contents ---"
    end = "%--- End generated contents ---"
    before, generated = text.split(begin, 1)
    generated, after = generated.split(end, 1)
    indexes, body = generated.split(r"\chapter{Topic Documentation}", 1)

    compounds = {
        entry.attrib["refid"]: (entry.attrib["kind"], entry.findtext("name"))
        for entry in ET.parse(output / "xml/index.xml").getroot().findall("compound")
    }
    sections = {
        "C API": {"API reference": [], "Data structures": [], "Headers": []},
        "C++23 API": {"API reference": [], "Namespaces": [], "Classes and types": [],
                       "Headers and usage guides": []},
    }
    inputs = re.findall(r"\\input\{([^}]+)\}", body)
    for name in inputs:
        identity = name.removesuffix("_source")
        kind, symbol = compounds[identity]
        if kind == "group":
            family = {"group__ivy__c__api": "C API", "group__ivy__cpp__api": "C++23 API"}[identity]
            section = "API reference"
        elif kind in {"class", "struct", "union", "interface"}:
            family = "C++23 API" if symbol.startswith("ivy::") else "C API"
            section = "Classes and types" if family == "C++23 API" else "Data structures"
        elif kind == "namespace" and (symbol == "ivy" or symbol.startswith("ivy::")):
            family, section = "C++23 API", "Namespaces"
        elif kind == "file" and Path(symbol).suffix in {".h", ".hpp"}:
            family = "C++23 API" if Path(symbol).suffix == ".hpp" else "C API"
            section = "Headers and usage guides" if family == "C++23 API" else "Headers"
        else:
            raise ValueError(f"Unclassified Doxygen input: {name} ({kind}: {symbol})")
        sections[family][section].append(name)

    parts = [indexes]
    for family, chapters in sections.items():
        parts.append(r"\part{" + family + "}\n")
        for title, names in chapters.items():
            if names:
                names.sort(key=lambda name: (
                    compound_priority(*compounds[name.removesuffix("_source")]),
                    name.endswith("_source")))
                parts.append(r"\chapter{" + family + ": " + title + "}\n")
                parts.extend(r"\input{" + name + "}\n" for name in names)
    organized = "".join(parts)
    if Counter(re.findall(r"\\input\{([^}]+)\}", generated)) != Counter(
        re.findall(r"\\input\{([^}]+)\}", organized)
    ):
        raise ValueError("PDF organization changed the set of generated inputs")
    reference.write_text(before + begin + organized + end + after)


if __name__ == "__main__":
    organize(Path(sys.argv[1]))
