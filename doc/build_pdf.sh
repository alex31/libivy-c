#!/bin/sh
# Generate the public C/C++ reference and its PDF from the repository root.
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$repo_dir"
for tool in python3 doxygen pdflatex makeindex make dot; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "Missing documentation tool: $tool" >&2
        exit 1
    fi
done

python3 doc/generate_reference.py
python3 doc/organize_pdf.py doc/doxygen
if ! make -C doc/doxygen/latex; then
    if [ -f doc/doxygen/latex/refman.log ]; then
        cat doc/doxygen/latex/refman.log >&2
    fi
    exit 1
fi
cp doc/doxygen/latex/refman.pdf doc/doxygen/ivy-api.pdf
echo "PDF generated: $repo_dir/doc/doxygen/ivy-api.pdf"
