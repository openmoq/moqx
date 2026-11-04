"""changelog_deps.py — build the `## Dependencies` block of a release section.

- It lists the pins in cmake/dependencies.cmake that changed since a tag.
- A pin absent at the tag counts as changed.
- moxygen is named by the v* release cmake/print-release-tag.cmake resolves
  MOXYGEN_REV to, else by short sha. Resolving needs network access to github.com.
"""

import shutil
import tempfile
from pathlib import Path

from changelog_util import ROOT, die, run

PINS = "MOXYGEN_REV CATAPULT_REV YAMLCPP_VERSION REFLECTCPP_VERSION".split()


def pin(name, cmake_dir=None):
    """Value of pin `name`, read from cmake_dir/dependencies.cmake if given.

    A pin missing from cmake_dir is None; one missing from the checkout dies.
    """
    script = Path(cmake_dir or ROOT / "cmake") / "print-pin.cmake"
    res = run("cmake", f"-DPIN={name}", "-P", str(script))
    if res.returncode == 0:
        return res.stdout.strip()
    if cmake_dir:
        return None
    die(f"cannot read pin {name}: {res.stderr.strip()}")


def link(name, ref, path):
    return f"- {name} [{ref}](https://github.com/{path})"


def dependencies(since):
    """Returns the block, or "" when no pin changed since tag `since`."""
    old = run("git", "show", f"{since}:cmake/dependencies.cmake")
    if old.returncode:
        die(f"{since} has no cmake/dependencies.cmake to compare against")

    # print-pin.cmake includes dependencies.cmake from its own directory, so the
    # old pins are read from a copy of both files.
    with tempfile.TemporaryDirectory() as tmp:
        (Path(tmp) / "dependencies.cmake").write_text(old.stdout, encoding="utf-8")
        shutil.copy(ROOT / "cmake/print-pin.cmake", tmp)
        changed = {n for n in PINS if pin(n) != pin(n, tmp)}

    lines = []
    if "MOXYGEN_REV" in changed:
        repo, rev = pin("MOXYGEN_REPOSITORY"), pin("MOXYGEN_REV")
        tag = run("cmake", "-P", "cmake/print-release-tag.cmake").stdout.strip()
        if tag.startswith("v"):
            lines.append(link("moxygen", tag, f"{repo}/releases/tag/{tag}"))
        else:
            lines.append(link("moxygen", rev[:7], f"{repo}/commit/{rev}"))
    if "CATAPULT_REV" in changed:
        repo, rev = pin("CATAPULT_REPOSITORY"), pin("CATAPULT_REV")
        lines.append(link("catapult", rev[:7], f"{repo}/commit/{rev}"))
    if "YAMLCPP_VERSION" in changed:
        v = pin("YAMLCPP_VERSION")
        lines.append(link("yaml-cpp", v, f"jbeder/yaml-cpp/releases/tag/{v}"))
    if "REFLECTCPP_VERSION" in changed:
        v = pin("REFLECTCPP_VERSION")
        lines.append(link("reflect-cpp", v, f"getml/reflect-cpp/releases/tag/{v}"))
    return "\n".join(["## Dependencies", "", *lines]) if lines else ""
