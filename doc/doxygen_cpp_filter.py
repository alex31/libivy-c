#!/usr/bin/env python3
"""Assemble public API sections for Doxygen, which does not inline class includes.

The compiler uses the original headers. Doxygen reads each section's file-level
guide separately; only its declarations are inserted into the owning ivy.hpp.
"""

from pathlib import Path
import re
import sys


def filter_header(path: Path) -> str:
    source = path.read_text()
    if path.name != "ivy.hpp":
        return source

    def section(match: re.Match) -> str:
        header = (path.parent / match.group(1)).read_text()
        begin = "// IVY_CPP_API_BEGIN\n"
        end = "// IVY_CPP_API_END"
        return header.split(begin, 1)[1].split(end, 1)[0].rstrip()

    source = re.sub(r'^#include "(api/[^"\n]+\.hpp|ivy_bus_private\.hpp)"$', section, source, flags=re.M)
    source = re.sub(r"^#(?:define|undef) IVY_CPP_API_HEADERS\n", "", source, flags=re.M)
    # Doxygen 1.9.x can carry a class's final private access into a reopened
    # namespace. Adjacent namespace blocks are equivalent to a single block.
    return re.sub(r"^} // namespace ivy\s*\nnamespace ivy {", "", source, flags=re.M)


if __name__ == "__main__":
    sys.stdout.write(filter_header(Path(sys.argv[1])))
