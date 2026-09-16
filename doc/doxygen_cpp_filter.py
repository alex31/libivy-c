#!/usr/bin/env python3
"""Assemble and order public API declarations for Doxygen.

The compiler uses the original headers. Doxygen reads each C++ section's
file-level guide separately; its declarations are inserted into ivy.hpp.
C and C++ operations are presented in reading order, rather than alphabetically.
"""

from pathlib import Path
import re
import sys

from reference_order import CPP_OBJECT_METHODS, order_c_header, order_cpp_bus, order_cpp_class


def filter_header(path: Path) -> str:
    source = path.read_text()
    if path.name == "ivy.h":
        return order_c_header(source)
    if path.name == "ivy_thread.hpp":
        return order_cpp_class(source, "LoopThread", [
            ("Operations", CPP_OBJECT_METHODS["LoopThread"]),
            ("Construction and ownership", ["LoopThread", "~LoopThread", "operator="])],
            prefix="LoopThread: ")
    if path.parent.name == "api":
        # Declarations are already inserted into ivy.hpp below. Parsing them a
        # second time from the section headers duplicates API documentation.
        return source.split("#if !defined(IVY_CPP_API_HEADERS)", 1)[0]
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
    source = re.sub(r"^} // namespace ivy\s*\nnamespace ivy {", "", source, flags=re.M)
    return order_cpp_bus(source)


if __name__ == "__main__":
    sys.stdout.write(filter_header(Path(sys.argv[1])))
