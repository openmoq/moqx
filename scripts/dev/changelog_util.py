"""changelog_util.py — repo root, die, rel and run for the changelog scripts."""

import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def die(msg):
    sys.exit(f"error: {msg}")


def rel(path):
    return path.relative_to(ROOT)


def run(*cmd):
    try:
        return subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    except FileNotFoundError:
        die(f"{cmd[0]} not found")
