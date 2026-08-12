#!/usr/bin/env python3
"""find-affected-tus.py — resolve changed files to the .cpp TUs to lint.

Usage: find-affected-tus.py ROOT < changed-files.txt

Reads changed repo-relative paths (one per line, restricted to src/test/
benchmark by the caller) from stdin. A changed .cpp is itself a TU. A
changed .h is resolved to every TU that transitively includes it.
Over-inclusive by design (a same-named header in a different directory also matches) rather
than risk missing a real dependent by trying to resolve exact include paths.

Prints the affected TUs' repo-relative paths, one per line.
"""
import re
import sys
from pathlib import Path

INCLUDE_RE = re.compile(r'#include\s*"([^"]+)"')


def basename_included(text, basename):
    for m in INCLUDE_RE.finditer(text):
        if Path(m.group(1)).name == basename:
            return True
    return False


def main():
    root = Path(sys.argv[1])
    changed = [line.strip() for line in sys.stdin if line.strip()]

    all_files = [
        p.relative_to(root)
        for d in ("src", "test", "benchmark")
        for p in (root / d).rglob("*")
        if p.suffix in (".cpp", ".h")
    ]
    texts = {p: (root / p).read_text(errors="replace") for p in all_files}

    changed_headers = {Path(f).name for f in changed if f.endswith(".h")}
    changed_cpps = {f for f in changed if f.endswith(".cpp")}

    # Transitive closure: a header included by an already-affected header is
    # affected too, so a change three levels deep in a header chain still
    # reaches the TU at the top.
    affected_header_basenames = set(changed_headers)
    headers = [p for p in all_files if p.suffix == ".h"]
    changed_flag = True
    while changed_flag:
        changed_flag = False
        for h in headers:
            if h.name in affected_header_basenames:
                continue
            if any(basename_included(texts[h], b) for b in affected_header_basenames):
                affected_header_basenames.add(h.name)
                changed_flag = True

    affected_tus = set(changed_cpps)
    for p in all_files:
        if p.suffix != ".cpp":
            continue
        rel = str(p)
        if rel in affected_tus:
            continue
        if any(basename_included(texts[p], b) for b in affected_header_basenames):
            affected_tus.add(rel)

    for rel in sorted(affected_tus):
        print(rel)


if __name__ == "__main__":
    main()
