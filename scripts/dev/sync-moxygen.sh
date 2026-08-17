#!/usr/bin/env bash
# sync-moxygen.sh — move the local moxygen checkout to MOXYGEN_REV.
#
# Usage: sync-moxygen.sh [--strict] [--dir DIR]
#
#   --strict    refuse loudly (non-zero exit) when the checkout cannot be moved.
#               The default reports the reason and exits 0.
#   --dir DIR   the moxygen checkout, else $MOQX_MOXYGEN_DIR. Required: which tree
#               you develop moxygen in is a fact about your machine, so this script
#               is told rather than guessing. Guessing from the build's own state
#               (deps/moxygen, the CMake cache) can land on a CPM clone, which is
#               CPM's to manage and must not be moved.
#
# The pin used to live in a git submodule, so a checkout moved moxygen with it. It is
# a line of cmake/dependencies.cmake now and nothing tracks it, so anything wanting
# that behaviour back — scripts/configure.sh --sync-moxygen-dir, or a VCS post-checkout
# hook — calls this.
#
# It never stashes, resets, or checks out over local work: that tree is where moxygen
# changes are made, and no pin is worth destroying it for. It refuses only what
# detaching would actually cost you — uncommitted changes, an operation mid-flight, or
# a commit no ref but HEAD names. Being on a branch is fine: the branch ref survives
# the detach and the message below says how to return.
#
# The default warns and exits 0 so that a caller running on every checkout does not
# turn routine navigation into an error; --strict is for callers that must stop.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

strict=0
dir="${MOQX_MOXYGEN_DIR:-}"
while (($#)); do
  case "$1" in
    --strict) strict=1; shift ;;
    --dir)    (($# >= 2)) || { echo "sync-moxygen.sh: --dir needs a directory" >&2; exit 1; }
              dir="$2"; shift 2 ;;
    -h|--help) awk 'NR>1 && /^#/ {sub(/^# ?/,""); print; next} NR>1 {exit}' "${BASH_SOURCE[0]}"; exit 0 ;;
    *)        echo "sync-moxygen.sh: unknown option '$1' (see --help)" >&2; exit 1 ;;
  esac
done

refuse() {
  echo "sync-moxygen.sh: $*" >&2
  ((strict)) && exit 1
  exit 0
}

if [[ -z "$dir" ]]; then
  refuse "no moxygen checkout given — pass --dir, or set MOQX_MOXYGEN_DIR"
fi
dir="$(cd "$dir" 2>/dev/null && pwd)" || refuse "moxygen checkout not found: $dir"

# print-pin.cmake is the only supported reader; cmake/dependencies.cmake says so.
pin="$(cmake -DPIN=MOXYGEN_REV -P cmake/print-pin.cmake)" \
  || refuse "could not read MOXYGEN_REV via cmake/print-pin.cmake"
[[ "$pin" =~ ^[0-9a-f]{40}$ ]] || refuse "MOXYGEN_REV is not a sha ('$pin')"

head="$(git -C "$dir" rev-parse HEAD 2>/dev/null)" || refuse "$dir is not a git checkout"
# Silent on the hook path: this is the common case on nearly every goto.
if [[ "$head" == "$pin" ]]; then
  if ((strict)); then
    echo "sync-moxygen.sh: $dir already at MOXYGEN_REV ($pin)"
  fi
  exit 0
fi

# Captured separately: [[ -n "$(git status …)" ]] throws the exit code away, so a
# corrupt index or a failing hook would yield empty output and read as "clean".
status_out="$(git -C "$dir" status --porcelain --untracked-files=all)" \
  || refuse "git status failed in $dir"
if [[ -n "$status_out" ]]; then
  refuse "$dir is dirty — commit, stash, or discard before it can move to $pin"
fi

git_dir="$(git -C "$dir" rev-parse --absolute-git-dir)"
for marker in rebase-merge rebase-apply MERGE_HEAD CHERRY_PICK_HEAD REVERT_HEAD BISECT_LOG sequencer; do
  if [[ -e "$git_dir/$marker" ]]; then
    refuse "$marker in progress in $dir"
  fi
done

# What detaching can actually destroy is a commit reachable only from HEAD: nothing
# else names it afterwards and the reflog is the only way back. Any ref containing
# HEAD — a branch, a tag, a remote — keeps it, and `git checkout` returns to it, so
# being on a branch is not itself a reason to refuse.
if [[ -z "$(git -C "$dir" for-each-ref --contains HEAD --count=1)" ]]; then
  refuse "HEAD in $dir (${head:0:12}) is on no branch, tag, or remote — moving it would leave it reflog-only"
fi

# A pin can name a commit that is no branch tip (a merge parent, a force-pushed
# rev), which a plain fetch will not bring down.
git -C "$dir" rev-parse -q --verify "${pin}^{commit}" >/dev/null \
  || git -C "$dir" fetch --quiet origin "$pin" \
  || refuse "$pin is not fetchable in $dir"
# Named before the checkout, and reported after: detaching off a branch is silent
# otherwise, and this is the one line that says how to get back to it.
was="$(git -C "$dir" symbolic-ref -q --short HEAD)" || was="${head:0:12}"
git -C "$dir" checkout --quiet --detach "$pin" || refuse "could not check out $pin in $dir"
echo "sync-moxygen.sh: moved $dir $was -> ${pin:0:12} (back with: git -C $dir checkout $was)"
