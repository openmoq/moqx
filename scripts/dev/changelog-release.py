#!/usr/bin/env python3
"""changelog-release.py — cut a CHANGELOG.md release section from changelog.d/.

Usage:
    scripts/dev/changelog-release.py
    scripts/dev/changelog-release.py <version> [--date YYYY-MM-DD] [--since <tag>]

- Without <version>, prints the entries in changelog.d/ as they will appear.
- With <version>, adds a `[<version>] - <date>` section below [Unreleased] and
  deletes the entry files with `git rm`. See docs/release.md#changelog.
  - The section holds the entries, then the dependencies changed since <tag>.
  - <date> defaults to today; <tag> to the latest v* tag reachable from HEAD,
    other than a prerelease tag such as v1.2.0-rc.1.
- Needs full history and an authenticated `gh`.
- Rendering the entries: see changelog_entries.py.
- The Dependencies block: see changelog_deps.py.
"""

import argparse
import datetime
import re

from changelog_deps import dependencies
from changelog_entries import ENTRIES, entries
from changelog_util import ROOT, die, rel, run

CHANGELOG = ROOT / "CHANGELOG.md"


def release(version, date, since):
    # An untracked file would be released but survive the git rm.
    if run("git", "status", "--porcelain", "--", str(rel(ENTRIES))).stdout:
        die("changelog.d/ has uncommitted changes")
    lines = CHANGELOG.read_text().split("\n")
    if any(line.startswith(f"[{version}]") for line in lines):
        die(f"CHANGELOG.md already has a [{version}] section")
    body, dirs = entries()
    deps = dependencies(since)
    section = "\n\n".join(b for b in (body, deps) if b)

    # A release heading is a line starting "[" whose next line is all "=".
    at = len(lines)
    for i in range(len(lines) - 1):
        is_heading = lines[i].startswith("[") and re.fullmatch("=+", lines[i + 1])
        if is_heading and lines[i] != "[Unreleased]":
            at = i
            break
    heading = f"[{version}] - {date}"
    new = [heading, "=" * len(heading), "", *([section, ""] if section else [])]
    if dirs:
        res = run("git", "rm", "-rq", "--", *(str(rel(d)) for d in dirs))
        if res.returncode:
            die(f"git rm failed: {res.stderr.strip()}")
    CHANGELOG.write_text("\n".join(lines[:at] + new + lines[at:]))


def main():
    parser = argparse.ArgumentParser(description="Cut a CHANGELOG.md release section.")
    parser.add_argument("version", nargs="?", help="e.g. 1.2.3; omit to preview")
    today = datetime.date.today()
    parser.add_argument("--date", type=datetime.date.fromisoformat, default=today)
    parser.add_argument("--since", help="previous release tag")
    args = parser.parse_args()

    if run("git", "rev-parse", "--is-shallow-repository").stdout.strip() != "false":
        die("shallow clone; run git fetch --unshallow")
    if not args.version:
        body, _ = entries()
        if body:
            print(body)
        return
    if not re.fullmatch(r"\d+\.\d+\.\d+(-[0-9A-Za-z.-]+)?", args.version):
        die(f"{args.version}: not a version such as 1.2.3")
    describe = "git describe --tags --abbrev=0 --match v* --exclude v*-* HEAD".split()
    since = args.since or run(*describe).stdout.strip()
    if not since:
        die("no release tag reachable from HEAD; pass --since")
    release(args.version, args.date, since)


if __name__ == "__main__":
    main()
