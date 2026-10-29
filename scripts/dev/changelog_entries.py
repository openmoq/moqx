"""changelog_entries.py — render the entries in changelog.d/.

- File layout and format: see changelog.d/README.md.
- Each entry links the PRs that added its files, looked up with `gh`.
- A file with no PR yet, e.g. uncommitted, renders unlinked with a warning on stderr.
- Entries are ordered by their lowest PR number; unlinked entries go last.
"""

import json
import re
import sys

from changelog_util import ROOT, die, rel, run

REPO = "openmoq/moqx"
PR_URL = f"https://github.com/{REPO}/pull/"
SECTIONS = "headline added changed deprecated removed fixed security".split()
NAME_RE = re.compile(r"([^.]+)(\.[^.]+)?\.md")
LINK = rf"\[#\d+\]\({re.escape(PR_URL)}\d+\)"
LINKS_RE = re.compile(rf"\s+\(({LINK}(?:, {LINK})*)\)$")

ENTRIES = ROOT / "changelog.d"


def pr_of(path):
    """Number of the PR that added `path`, or None.

    Prefers merged PRs, earliest first.
    """
    # An uncommitted file at a released name would match the released add.
    if run("git", "cat-file", "-e", f"HEAD:{rel(path)}").returncode:
        return None
    log = "git log --no-renames --diff-filter=A -1 --format=%H --".split()
    sha = run(*log, str(rel(path))).stdout.strip()
    res = run("gh", "api", f"repos/{REPO}/commits/{sha}/pulls")
    if res.returncode:
        # On failure, gh prints the error body to stdout.
        # 422: GitHub has no such commit, e.g. one not pushed yet.
        if re.search(r'"status": ?"422"', res.stdout):
            return None
        die(f"gh api for {sha} failed: {res.stdout.strip() or res.stderr.strip()}")
    pulls = sorted(json.loads(res.stdout), key=lambda p: p["merged_at"] or "~")
    return pulls[0]["number"] if pulls else None


def read(path):
    lines = [line.rstrip() for line in path.read_text(encoding="utf-8").splitlines()]
    return "\n".join(lines).strip("\n")


def entry(group, files):
    """Returns (sort key, lines) of the entry that `files` make up."""
    bodies = [f for f in files if read(f)]
    if not bodies:
        die(f"{rel(files[0])}: no file holds the entry's text; was it released?")
    if len(bodies) > 1:
        die(f"{rel(bodies[0])} and {rel(bodies[1])} both hold text for one entry")
    body = bodies[0]
    first, *rest = read(body).split("\n")
    if not first.startswith("- "):
        die(f"{rel(body)}: first line must start with '- '")

    prs = set()
    if m := LINKS_RE.search(first):
        # Explicit links replace the PR that added the file.
        prs.update(int(n) for n in re.findall(r"#(\d+)", m.group(1)))
        first = first[: m.start()]
    for f in files:
        if m and f == body:
            continue
        if pr := pr_of(f):
            prs.add(pr)
        else:
            warning = f"warning: {rel(f)}: no PR found; its link is missing"
            print(warning, file=sys.stderr)

    if prs:
        first += " (" + ", ".join(f"[#{n}]({PR_URL}{n})" for n in sorted(prs)) + ")"
    return (min(prs, default=sys.maxsize), group), [first, *rest]


def entries():
    """Returns the rendered entries, and the section directories holding them."""
    groups = {s: {} for s in SECTIONS}
    for path in sorted(ENTRIES.iterdir()):
        if path.name.startswith(".") or path.name == "README.md":
            continue
        if not path.is_dir() or path.name not in SECTIONS:
            die(f"{rel(path)}: not a section directory")
        for f in sorted(path.iterdir()):
            if f.name.startswith("."):
                continue
            m = NAME_RE.fullmatch(f.name)
            if not f.is_file() or not m:
                die(f"{rel(f)}: not a <name>.md or <name>.<anything>.md file")
            groups[path.name].setdefault(m.group(1), []).append(f)

    blocks = []
    for section, found in groups.items():
        if not found:
            continue
        lines = [] if section == "headline" else [f"## {section.capitalize()}", ""]
        for _, e in sorted(entry(g, files) for g, files in found.items()):
            lines += e
        blocks.append("\n".join(lines))
    dirs = [ENTRIES / s for s, found in groups.items() if found]
    return "\n\n".join(blocks), dirs
