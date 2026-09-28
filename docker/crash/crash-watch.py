#!/usr/bin/env python3
"""Relay crash watcher for the CI relay host.

Follows Docker events for the relay container. When the relay dies on a crash
signal or the OOM killer, the watcher keeps the core dump and the run's log on
this host, decodes the stack with gdb inside the image that crashed, and
records the crash in one GitHub issue per unique stack. A repeat updates that
issue's occurrence table, the first crash on a new build also adds a comment,
and a crash on an issue closed as completed reopens it. New issues, new builds
and reopens are also posted to Slack when SLACK_WEBHOOK_URL is set.

Core dumps never leave this host: the repository is public and a core holds
the relay's TLS key.

usage:
  crash-watch.py [watch]          follow Docker events (the systemd service)
  crash-watch.py decode <bundle>  decode a bundle and print its stack
  crash-watch.py gdb <bundle>     open gdb on a bundle's core
"""

import contextlib
import hashlib
import json
import os
import queue
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time
import traceback
import urllib.error
import urllib.request
from pathlib import Path

CONTAINER = os.environ.get("CRASH_CONTAINER", "moqx")
CRASH_DIR = Path(os.environ.get("CRASH_DIR", "/var/lib/moqx-crash"))
HOST = os.environ.get("CRASH_HOST") or socket.getfqdn()
GH_REPO = os.environ.get("GH_REPO", "openmoq/moqx")
GH_TOKEN = os.environ.get("GH_TOKEN", "")
SLACK_WEBHOOK_URL = os.environ.get("SLACK_WEBHOOK_URL", "")
LABEL = os.environ.get("CRASH_ISSUE_LABEL", "crash")
DRY_RUN = os.environ.get("CRASH_DRY_RUN") == "1"
# Caps that keep a crash loop from flooding the issue tracker or the disk.
MAX_NEW_ISSUES_PER_DAY = int(os.environ.get("CRASH_MAX_NEW_ISSUES_PER_DAY", "5"))
MAX_GDB_RUNS_PER_HOUR = int(os.environ.get("CRASH_MAX_GDB_RUNS_PER_HOUR", "6"))
KEEP_BUNDLES = int(os.environ.get("CRASH_KEEP_BUNDLES", "20"))  # per signature
KEEP_CORES = int(os.environ.get("CRASH_KEEP_CORES", "3"))  # per signature
MAX_BUNDLES = int(os.environ.get("CRASH_MAX_BUNDLES", "500"))  # all signatures
MAX_CORE_BYTES = int(float(os.environ.get("CRASH_MAX_CORE_GB", "20")) * 2**30)
GDB_MEMORY = os.environ.get("CRASH_GDB_MEMORY", "8g")

BUNDLES = CRASH_DIR / "bundles"
STATE = CRASH_DIR / "state.json"
CORE_MOUNT = "/var/coredumps"
BINARY = "/usr/local/bin/moqx"
SELF = "/usr/local/libexec/moqx-crash/crash-watch.py"

SIGNALS = {
    4: "SIGILL",
    5: "SIGTRAP",
    6: "SIGABRT",
    7: "SIGBUS",
    8: "SIGFPE",
    9: "SIGKILL",
    11: "SIGSEGV",
    31: "SIGSYS",
}
CRASH_SIGNALS = {4, 5, 6, 7, 8, 11, 31}

# Frames that say how the process died rather than where: signal delivery,
# abort/assert/terminate machinery, logging's fatal path, unresolved frames.
SKIP_FRAME = re.compile(
    r"<signal handler called>|\?\?|\(unknown\)|__restore_rt"
    r"|(__GI_)?(raise|abort|__assert_fail(_base)?)|_?_?pthread_kill\w*"
    r"|__(mem|str|wmem|wcs)\w*|__libc_\w+"
    r"|std::(__)?terminate|__cxxabiv1::.*|__cxa_\w+"
    r"|__gnu_cxx::__verbose_terminate_handler"
    r"|google::.*|folly::(Log\w*|symbolizer)\b.*"
    r"|folly::(detail::)?(terminate_with|assertionFailure|throw_exception)\w*"
)
OPERATORS = (
    "operator<=>",
    "operator<<=",
    "operator>>=",
    "operator<<",
    "operator>>",
    "operator<=",
    "operator>=",
    "operator->*",
    "operator->",
    "operator<",
    "operator>",
)

RUN_START = re.compile(r"^.*\bmoqx \S+ starting$", re.M)
FOLLY_HEADER = re.compile(r"^\*\*\* Signal \d+ \((\w+)\).*stack trace: \*\*\*$", re.M)
FOLLY_FRAME = re.compile(r"^\s*@ [0-9a-f]+ (.+)$")
FOLLY_LOC = re.compile(r"^\s{8,}(\S+:\d+)$")
# Symbolizer-less ("safe mode") frame: module(symbol+offset)[address].
FOLLY_RAW = re.compile(r"^(\S+\([^)]*\))\[0x[0-9a-f]+\]$")
FATAL_LINE = re.compile(r"^F\d{4} \d\d:\d\d:\d\d.*$", re.M)
SHUTDOWN = re.compile(r"Received signal (\d+), shutting down")
GDB_FRAME = re.compile(r"^#\d+\s+(?:0x[0-9a-f]+ in )?(.+)$")

GDB_COMMANDS = [
    "set pagination off",
    "set width 0",
    "set print thread-events off",
    "echo @@siginfo\\n",
    "print $_siginfo.si_signo",
    "print $_siginfo.si_code",
    "print $_siginfo._sifields._sigfault.si_addr",
    "echo @@crash\\n",
    "bt 64",
    "echo @@threads\\n",
    "thread apply all bt 32",
]
GDB_ARGS = ["-batch", "-nx", "-q", "-iex", "set auto-load off"]
GDB_ARGS += [arg for cmd in GDB_COMMANDS for arg in ("-ex", cmd)]

MARKER = "<!-- moqx-crash-signature: {} -->"
SITE_MARKER = "<!-- moqx-crash-site: {} -->"
# Crash sites in relay code group call paths into one issue; a library function
# (folly, std) is too generic for that.
RELAY_CODE = re.compile(r"(moxygen|openmoq)::")
OCC_START, OCC_END = "<!-- occurrences -->", "<!-- /occurrences -->"
OCC_HEADER = ["Build", "Count", "First seen (UTC)", "Last seen (UTC)", "Latest bundle"]
HELP = (
    "Core dumps stay on the relay host because they hold the relay's TLS key. "
    f"Each bundle under `{BUNDLES}/` has `core.zst` and `container.log`, plus "
    "`gdb.txt` (every thread) when gdb decoded it. To open a core in gdb, run "
    f"`sudo {SELF} gdb <bundle>` on the host.\n\n"
    "To merge this issue into another crash issue, copy its "
    "`moqx-crash-signature` line (view the source of this description) into that "
    "issue's description, then close this one as a duplicate."
)


def log(msg):
    print(msg, flush=True)


def run(cmd, timeout=300):
    """Runs cmd with stdout and stderr merged into .stdout."""
    try:
        return subprocess.run(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            errors="replace",
            timeout=timeout,
        )
    except subprocess.TimeoutExpired:
        return subprocess.CompletedProcess(cmd, -1, f"timed out after {timeout}s")


def read_text(path):
    try:
        return path.read_text(errors="replace")
    except OSError:
        return ""


def load_state():
    try:
        return json.loads(STATE.read_text())
    except (OSError, ValueError):
        return {}


def save_state(state):
    tmp = STATE.with_suffix(".tmp")
    tmp.write_text(json.dumps(state, indent=1))
    tmp.replace(STATE)


def recent(state, key, window):
    """state[key] (epoch list) trimmed to the last `window` seconds."""
    now = time.time()
    state[key] = [t for t in state.get(key, []) if now - t < window]
    return state[key]


# ── stack signature ──────────────────────────────────────────────────────────


def base_name(fn):
    """fn without argument values, parameter list or trailing qualifiers.

    gdb prints `name (args)`, and for templates the name carries its own
    parameter list, so trailing groups are stripped until a bare name is left.
    """
    s = re.sub(r'"(?:[^"\\]|\\.)*"', '""', fn)
    while True:
        s = re.sub(r"(\s+(const|volatile)|\s*&&?)+$", "", s.rstrip())
        if not s.endswith(")"):
            return s
        depth = 0
        for i in range(len(s) - 1, -1, -1):
            depth += {")": 1, "(": -1}.get(s[i], 0)
            if depth == 0:
                break
        head = s[:i].rstrip()
        if not head or head.endswith("operator"):
            return s
        s = head


def drop_templates(s, keep=""):
    """s with each outermost template argument list replaced by keep."""
    for i, op in enumerate(OPERATORS):
        s = s.replace(op, f"\0{i}\0")
    out, depth = [], 0
    for c in s:
        if c == "<":
            out.append(keep if depth == 0 else "")
            depth += 1
        elif c == ">":
            depth = max(depth - 1, 0)
        elif depth == 0:
            out.append(c)
    s = "".join(out)
    for i, op in enumerate(OPERATORS):
        s = s.replace(f"\0{i}\0", op)
    return s


def sig_name(fn):
    """Name that stays stable across builds: no clones, templates, lambda ids."""
    s = re.sub(r"\s*\[clone [^\]]*\]", "", base_name(fn))
    s = re.sub(r"\{lambda\([^{}]*\)#\d+\}|<lambda\([^<>]*\)>", "{lambda}", s)
    s = drop_templates(s.replace("(anonymous namespace)::", ""))
    head, _, tail = s.strip().rpartition(" ")
    if head and "operator" not in head and re.fullmatch(r"[\w:*& ]+", head):
        s = tail  # demangled return type
    return s.strip()


def signature(frames):
    """(10-hex signature, top frame names) or (None, []) without usable frames."""
    names = (sig_name(f["fn"]) for f in frames)
    top = [n for n in names if n and not SKIP_FRAME.fullmatch(n)][:3]
    if not top:
        return None, []
    return hashlib.sha1("\n".join(top).encode()).hexdigest()[:10], top


# ── decoding ─────────────────────────────────────────────────────────────────


def last_run(log_text):
    """The part of the container log written by the run that crashed."""
    starts = [m.end() for m in RUN_START.finditer(log_text)]
    return log_text[starts[-1] :] if starts else log_text


def folly_trace(run_log):
    """The first fatal-signal trace folly printed in the run.

    Returns (signal, sent, lines, frames): whether another process sent the
    signal, the trace's frame lines without addresses, and its named frames
    (empty when folly had no symbolizer).
    """
    m = FOLLY_HEADER.search(run_log)
    if not m:
        return None, False, [], []
    lines, frames = [], []
    for line in run_log[m.end() :].splitlines():
        if fm := FOLLY_FRAME.match(line):
            frames.append({"fn": fm.group(1).strip(), "loc": ""})
            lines.append(frames[-1]["fn"])
        elif fm := FOLLY_RAW.match(line):
            lines.append(fm.group(1))
        elif frames and (lm := FOLLY_LOC.match(line)):
            frames[-1]["loc"] = lm.group(1)
        elif lines and line.strip():
            break
    return m.group(1), "(maybe from PID" in m.group(0), lines, frames


def section(text, name):
    m = re.search(rf"^@@{name}$(.*?)(?=^@@|\Z)", text, re.S | re.M)
    return m.group(1) if m else ""


def gdb_report(text):
    """(signal, si_code, fault address, crashing-thread frames) from gdb output."""
    sig = re.search(r"Program terminated with signal (SIG\w+)", text)
    info = section(text, "siginfo")
    code = re.search(r"^\$2 = (-?\d+)", info, re.M)
    addr = re.search(r"^\$3 = \(void \*\) (0x[0-9a-f]+)", info, re.M)
    frames = []
    for line in section(text, "crash").splitlines():
        if m := GDB_FRAME.match(line):
            fn, loc = m.group(1), ""
            if lm := re.search(r" (?:at|from) (\S+)$", fn):
                fn, loc = fn[: lm.start()], lm.group(1)
            frames.append({"fn": fn, "loc": loc})
    return (
        sig and sig.group(1),
        code and int(code.group(1)),
        addr and addr.group(1),
        frames,
    )


def gdb_image(image_id):
    """Tag of the crashed image with gdb added, built once per image."""
    tag = "moqx-crash-gdb:" + image_id.removeprefix("sha256:")[:12]
    if run(["docker", "image", "inspect", tag]).returncode == 0:
        return tag
    builder = tag.replace(":", "-")
    run(["docker", "rm", "-f", builder])
    install = (
        "apt-get update -qq && apt-get install -y -qq --no-install-recommends gdb"
        " && rm -rf /var/lib/apt/lists/*"
    )
    r = run(
        ["docker", "run", "--name", builder, "-e", "DEBIAN_FRONTEND=noninteractive"]
        + ["--entrypoint", "sh", image_id, "-c", install],
        timeout=900,
    )
    if r.returncode == 0:
        r = run(
            ["docker", "commit", "--change", 'ENTRYPOINT ["gdb"]']
            + ["--change", "LABEL moqx.crash-gdb=1", builder, tag]
        )
    run(["docker", "rm", "-f", builder])
    if r.returncode != 0:
        log(f"no gdb image for {image_id}: {r.stdout[-500:]}")
        return None
    run(
        ["docker", "image", "prune", "-af", "--filter", "label=moqx.crash-gdb=1"]
        + ["--filter", "until=720h"]
    )
    return tag


def gdb_decode(image_id, core):
    """gdb's report on core, run inside the image that crashed."""
    image = gdb_image(image_id)
    if not image:
        return None
    # Named so a timed-out run can be removed: killing the CLI leaves it running.
    name = f"moqx-crash-gdb-{core.parent.name}"
    try:
        return run(
            ["docker", "run", "--rm", "--name", name, "--network", "none"]
            + ["--memory", GDB_MEMORY, "-v", f"{core.parent}:/crash:ro", image]
            + [*GDB_ARGS, BINARY, f"/crash/{core.name}"],
            timeout=1800,
        ).stdout
    finally:
        run(["docker", "rm", "-f", name])


def short_path(loc):
    return loc.removeprefix("/opt/moxygen/include/").removeprefix("./")


def git_ref(version):
    """Commit or tag a relay version string names, if any."""
    if sha := re.search(r"-g([0-9a-f]{7,40})$", version):
        return sha.group(1)
    return version if re.fullmatch(r"v\d[\w.+-]*", version) else None


def source_links(frames, version, limit=3):
    """Links to the relay's own frames at the commit that crashed."""
    ref, links = git_ref(version), []
    for f in frames:
        m = re.fullmatch(r"(?:\./)?(src/\S+):(\d+)", f["loc"])
        if ref and m and len(links) < limit:
            url = f"https://github.com/{GH_REPO}/blob/{ref}/{m.group(1)}#L{m.group(2)}"
            links.append(f"[{m.group(1)}:{m.group(2)}]({url})")
    return links


def render(frames, limit=40):
    out = []
    for i, f in enumerate(frames[:limit]):
        name = base_name(f["fn"])
        if not name.startswith("<"):  # keep "<signal handler called>"
            name = drop_templates(name, "<…>")
        line = f"#{i:<3}{name}"
        if f["loc"]:
            line += f"  {short_path(f['loc'])}"
        out.append(line[:300])
    if len(frames) > limit:
        out.append(f"... {len(frames) - limit} more frames")
    return "\n".join(out) or "(no frames)"


@contextlib.contextmanager
def raw_core(bundle):
    """Path to the bundle's uncompressed core, decompressing to a temp dir."""
    core = bundle / "core"
    if core.exists():
        yield core
        return
    packed = bundle / "core.zst"
    if not packed.exists():
        sys.exit(f"{bundle.name}: its core was pruned")
    with tempfile.TemporaryDirectory(dir=CRASH_DIR) as tmp:
        core = Path(tmp) / "core"
        subprocess.run(["zstd", "-q", "-d", str(packed), "-o", str(core)], check=True)
        yield core


# ── capture ──────────────────────────────────────────────────────────────────


def core_dir(cid, attrs):
    """Host path of the container's /var/coredumps volume."""
    r = run(["docker", "inspect", "--format", "{{json .Mounts}}", cid])
    if r.returncode == 0:
        for mount in json.loads(r.stdout):
            if mount.get("Destination") == CORE_MOUNT:
                return Path(mount["Source"])
    project = attrs.get("com.docker.compose.project", "moqx")
    r = run(
        ["docker", "volume", "inspect", "--format", "{{.Mountpoint}}"]
        + [f"{project}_moqx-coredumps"]
    )
    return Path(r.stdout.strip()) if r.returncode == 0 else None


def pick_core(cores, died, run_start=None):
    """The core this exit wrote, or None.

    The kernel finishes writing a core just before the process exits, so a
    core from outside a few seconds of the exit, or from before the crashed
    run started, belongs to another crash.
    """
    lower = max(died - 5, run_start or 0)
    found = [p for p in cores.glob("core.*") if lower <= p.stat().st_mtime <= died + 5]
    return max(found, key=lambda p: p.stat().st_mtime, default=None)


def capture(ev, signo, oom, run_start=None):
    """Moves the crash's core and log into a new bundle before anything else runs."""
    attrs, cid = ev["Actor"]["Attributes"], ev["Actor"]["ID"]
    died_ns = int(ev.get("timeNano") or int(ev["time"]) * 10**9)
    died = died_ns / 10**9
    stamp = time.strftime("%Y%m%dT%H%M%SZ", time.gmtime(died))
    bundle = BUNDLES / f"{stamp}-{cid[:12]}"
    bundle.mkdir(parents=True, exist_ok=True)
    # Up to the exit exactly: the restarted relay logs within the same second.
    until = f"{died_ns // 10**9}.{died_ns % 10**9:09d}"
    logs = run(["docker", "logs", "--until", until, "--tail", "5000", cid])
    (bundle / "container.log").write_text(logs.stdout)

    core_name = None
    cores = core_dir(cid, attrs)
    if cores and cores.is_dir() and (core := pick_core(cores, died, run_start)):
        shutil.move(str(core), str(bundle / "core"))
        core_name = core.name

    image_id = (
        attrs.get("com.docker.compose.image")
        or run(["docker", "inspect", "--format", "{{.Image}}", cid]).stdout.strip()
    )
    meta = {
        "container": attrs.get("name", CONTAINER),
        "container_id": cid,
        "image": attrs.get("image", ""),
        "image_id": image_id,
        "version": attrs.get("org.opencontainers.image.version", "unknown"),
        "exit_code": int(attrs.get("exitCode", 0)),
        "signal": SIGNALS.get(signo, f"signal {signo}"),
        "oom": oom,
        "died": died,
        "host": HOST,
        "core": core_name,
    }
    (bundle / "meta.json").write_text(json.dumps(meta, indent=1))
    log(f"{bundle.name}: captured (core {core_name or 'missing'})")
    return bundle


# ── GitHub ───────────────────────────────────────────────────────────────────


def gh(method, path, body=None):
    if DRY_RUN and method != "GET":
        log(f"dry run: {method} {path}\n{json.dumps(body, indent=1)}")
        return {"number": 0}
    headers = {
        "Accept": "application/vnd.github+json",
        "Content-Type": "application/json",
        "X-GitHub-Api-Version": "2022-11-28",
    }
    if GH_TOKEN:
        headers["Authorization"] = f"Bearer {GH_TOKEN}"
    data = None if body is None else json.dumps(body).encode()
    req = urllib.request.Request(
        f"https://api.github.com/{path}", data=data, method=method, headers=headers
    )
    with urllib.request.urlopen(req, timeout=30) as resp:
        return json.loads(resp.read() or b"null")


def slack(emoji, what, meta, number, title, url=None):
    """Posts one line about an issue to Slack. A failure is only logged."""
    if not SLACK_WEBHOOK_URL and not DRY_RUN:
        return
    url = url or f"https://github.com/{GH_REPO}/issues/{number}"
    title = title.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")
    text = (
        f"{emoji} *{GH_REPO}* relay crashed on `{meta['host']}` (`{meta['version']}`): "
        f"{what} <{url}|#{number} {title}>"
    )
    if DRY_RUN:
        log(f"dry run: slack: {text}")
        return
    data = json.dumps({"text": text}).encode()
    req = urllib.request.Request(
        SLACK_WEBHOOK_URL, data=data, headers={"Content-Type": "application/json"}
    )
    try:
        urllib.request.urlopen(req, timeout=10).close()
    except OSError as e:
        log(f"Slack post failed: {e!r}")


def find_issue(sig, site, state):
    """(issue, exact) for sig, else an open issue with the same crash site.

    Issues closed as duplicates are skipped, so a merged signature follows its
    marker to the issue that was kept.
    """
    marker = MARKER.format(sig)
    site_marker = SITE_MARKER.format(site) if site else None

    def live(issue):
        return issue.get("state_reason") != "duplicate"

    number = state.get("issues", {}).get(sig)
    if number:
        with contextlib.suppress(urllib.error.HTTPError):
            issue = gh("GET", f"repos/{GH_REPO}/issues/{number}")
            if marker in (issue.get("body") or "") and live(issue):
                return issue, True
    same_site = None
    for page in range(1, 11):
        issues = gh(
            "GET",
            f"repos/{GH_REPO}/issues?labels={LABEL}&state=all&per_page=100&page={page}",
        )
        for issue in filter(live, issues):
            body = issue.get("body") or ""
            if marker in body:
                return issue, True
            if site_marker and site_marker in body and issue["state"] == "open":
                same_site = same_site or issue
        if len(issues) < 100:
            break
    return same_site, False


def ensure_label():
    try:
        gh(
            "POST",
            f"repos/{GH_REPO}/labels",
            {"name": LABEL, "color": "b60205", "description": "Relay crash"},
        )
    except urllib.error.HTTPError as e:
        if e.code != 422:  # 422: the label exists
            raise


def build_cell(version):
    """The Build column: the relay version, linked to its commit or tag."""
    ref = git_ref(version)
    return (
        f"[{version}](https://github.com/{GH_REPO}/tree/{ref})"
        if ref
        else f"`{version}`"
    )


def table(rows):
    lines = [
        OCC_START,
        "| " + " | ".join(OCC_HEADER) + " |",
        "|---" * len(OCC_HEADER) + "|",
    ]
    lines += ["| " + " | ".join(r) + " |" for r in rows]
    return "\n".join([*lines, OCC_END])


def bump(body, row):
    """body with row merged into its occurrence table; True if row's build is new."""
    m = re.search(re.escape(OCC_START) + "(.*?)" + re.escape(OCC_END), body, re.S)
    rows = []
    for line in m.group(1).strip().splitlines()[2:] if m else []:
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        if len(cells) == len(OCC_HEADER) and cells[1].isdigit():
            rows.append(cells)
    same = next((r for r in rows if r[0] == row[0]), None)
    if same:
        same[1], same[3], same[4] = str(int(same[1]) + 1), row[3], row[4]
    else:
        rows.append(row)
    rows = sorted(rows, key=lambda r: r[3], reverse=True)[:15]
    new = table(rows)
    body = body[: m.start()] + new + body[m.end() :] if m else f"{body}\n\n{new}"
    return body, same is None


def describe(meta, frames):
    if meta["oom"]:
        text = f"The relay on `{meta['host']}` was killed by the OOM killer."
    else:
        text = f"The relay on `{meta['host']}` crashed with **{meta['signal']}**"
        if meta.get("fault"):
            text += f" at address `{meta['fault']}`"
        text += "."
    if meta.get("sent"):
        text += " Another process sent the signal; it was not a fault."
    if meta.get("shutdown_signal"):
        text += f" It was shutting down after signal {meta['shutdown_signal']}."
    if links := source_links(frames, meta["version"]):
        text += f"\n\nRelay source: {', '.join(links)}"
    if meta.get("fatal"):
        text += f"\n\nFatal log line:\n```\n{meta['fatal']}\n```"
    return text


def issue_title(meta, top, frames):
    if meta["oom"]:
        return "Relay killed by the OOM killer"
    if not top:
        return f"Relay crash: {meta['signal']}, no usable stack"
    site = top[0]
    if "::" not in site:  # e.g. a lambda's bare operator(): add its source line
        locs = (
            f["loc"]
            for f in frames
            if re.search(r":\d+$", f["loc"]) and sig_name(f["fn"]) == site
        )
        if loc := next(locs, None):
            site += f" at {short_path(loc)}"
    title = f"Relay crash: {meta['signal']} in {site}"
    return title if len(title) <= 120 else title[:117] + "..."


def report(sig, top, frames, meta, bundle, state):
    """Files or updates the issue for sig. Returns its number, or None."""
    if not GH_TOKEN and not DRY_RUN:
        log(f"{bundle.name}: GitHub reporting off (no GH_TOKEN), kept on the host")
        return None
    stack = render(frames)
    when = time.strftime("%Y-%m-%d %H:%M", time.gmtime(meta["died"]))
    row = [build_cell(meta["version"]), "1", when, when, f"`{bundle.name}`"]
    site = None
    if top and RELAY_CODE.match(top[0]):
        site = hashlib.sha1(top[0].encode()).hexdigest()[:10]
    issue, exact = find_issue(sig, site, state)
    if issue is None:
        created = recent(state, "created", 86400)
        if len(created) >= MAX_NEW_ISSUES_PER_DAY:
            log(
                f"{bundle.name}: {MAX_NEW_ISSUES_PER_DAY} new issues in 24h, kept on the host"
            )
            return None
        chain = " → ".join(f"`{t}`" for t in top) or "no usable frames"
        markers = MARKER.format(sig) + (f"\n{SITE_MARKER.format(site)}" if site else "")
        body = "\n\n".join(
            [
                markers,
                describe(meta, frames),
                f"Signature `{sig}`: {chain}.",
                f"**Crashing thread**\n\n```\n{stack}\n```",
                "**Occurrences.** Repeats of this stack update this table. "
                "The first crash on a new build also adds a comment.",
                table([row]),
                HELP,
            ]
        )
        ensure_label()
        title = issue_title(meta, top, frames)
        new = gh(
            "POST",
            f"repos/{GH_REPO}/issues",
            {"title": title, "body": body, "labels": [LABEL]},
        )
        created.append(time.time())
        state.setdefault("issues", {})[sig] = new["number"]
        slack(":boom:", "new issue", meta, new["number"], title, new.get("html_url"))
        return new["number"]

    number = issue["number"]
    body, new_build = bump(issue["body"] or "", row)
    patch, lead, note = {"body": body}, None, None
    if not exact:
        patch["body"] = f"{MARKER.format(sig)}\n{body}"
        lead = f"New call path into the same function (signature `{sig}`) on {row[0]} at {when} UTC."
        note = (":boom:", "new call path for")
    elif issue["state"] == "closed":
        if issue.get("state_reason") == "completed":  # not "not planned"/duplicate
            patch["state"] = "open"
            lead = f"Seen again on {row[0]} at {when} UTC after this issue was closed."
            note = (":rotating_light:", "reopened")
    elif new_build:
        lead = f"First seen on {row[0]} at {when} UTC."
        note = (":repeat:", "first time on this build for")
    gh("PATCH", f"repos/{GH_REPO}/issues/{number}", patch)
    if lead:
        comment = (
            f"{lead} Bundle `{bundle.name}`.\n\n{describe(meta, frames)}\n\n"
            f"<details><summary>Crashing thread</summary>\n\n```\n{stack}\n```\n\n</details>"
        )
        gh("POST", f"repos/{GH_REPO}/issues/{number}/comments", {"body": comment})
        slack(*note, meta, number, issue.get("title", ""), issue.get("html_url"))
    state.setdefault("issues", {})[sig] = number
    return number


# ── processing ───────────────────────────────────────────────────────────────


def cached(key):
    """(signature, top, frames) of an earlier gdb decode of the same trace."""
    for bundle in sorted(BUNDLES.iterdir(), reverse=True):
        try:
            meta = json.loads((bundle / "meta.json").read_text())
        except (OSError, ValueError):
            continue
        if meta.get("trace_key") == key and meta.get("decoded"):
            frames = gdb_report(read_text(bundle / "gdb.txt"))[3]
            return meta["signature"], meta["top"], frames
    return None


def decode(bundle, meta, state):
    """Returns (signature, top frames, frames, reason to keep it off GitHub).

    gdb is the one source of signatures. folly's in-process trace only
    identifies a repeat of a crash already decoded on the same image, so a
    crash loop does not rerun gdb.
    """
    run_log = last_run(read_text(bundle / "container.log"))
    fatal = FATAL_LINE.findall(run_log)
    meta["fatal"] = fatal[-1][:500] if fatal else None
    if shutdown := SHUTDOWN.search(run_log):
        meta["shutdown_signal"] = int(shutdown.group(1))
    if meta["oom"]:
        return "oom-kill", [], [], None

    signal, meta["sent"], lines, named = folly_trace(run_log)
    meta["signal"] = signal or meta["signal"]
    if lines:
        key = "\n".join([meta["image_id"], *lines])
        meta["trace_key"] = hashlib.sha1(key.encode()).hexdigest()[:16]
        if hit := cached(meta["trace_key"]):
            return (*hit, None)

    core = bundle / "core"
    if not core.exists():
        sig, top = signature(named)
        return sig or f"unknown-{meta['signal'].lower()}", top, named, None
    runs = recent(state, "gdb_runs", 3600)
    if len(runs) >= MAX_GDB_RUNS_PER_HOUR:
        return None, [], [], f"gdb skipped, {len(runs)} runs in the last hour"
    runs.append(time.time())
    text = gdb_decode(meta["image_id"], core)
    if text is None:
        return None, [], [], "no gdb image for the crashed image"
    (bundle / "gdb.txt").write_text(text)
    if "may not match" in text:
        return None, [], [], "the core does not match the image's binary"

    gdb_signal, si_code, fault, frames = gdb_report(text)
    if not signal:
        meta["signal"] = gdb_signal or meta["signal"]
        meta["sent"] = si_code is not None and si_code <= 0
    if meta["signal"] in ("SIGSEGV", "SIGBUS") and not meta["sent"]:
        meta["fault"] = fault
    sig, top = signature(frames)
    meta["decoded"] = sig is not None
    return sig or f"unknown-{meta['signal'].lower()}", top, frames, None


def process(bundle):
    """Decodes a captured bundle, reports it, and applies retention."""
    meta = json.loads((bundle / "meta.json").read_text())
    state = load_state()
    sig, top, frames, reason = decode(bundle, meta, state)
    meta["signature"], meta["top"] = sig, top

    core = bundle / "core"
    if core.exists():
        run(
            ["zstd", "-q", "-T0", "--rm", str(core), "-o", str(bundle / "core.zst")],
            1800,
        )

    issue = None
    if reason:
        log(f"{bundle.name}: {reason}, kept on the host")
    else:
        try:
            issue = report(sig, top, frames, meta, bundle, state)
        except (OSError, ValueError) as e:
            log(f"{bundle.name}: GitHub report failed: {e!r}")
    meta["issue"] = issue
    (bundle / "meta.json").write_text(json.dumps(meta, indent=1))
    save_state(state)
    prune(sig)
    log(f"{bundle.name}: {meta['signal']} signature {sig} issue {issue or '-'}")


def prune(sig):
    """Keeps the newest bundles and cores of sig, then holds the totals under
    MAX_BUNDLES and MAX_CORE_BYTES by dropping the oldest."""
    mine = []
    for bundle in BUNDLES.iterdir():
        with contextlib.suppress(OSError, ValueError):
            if json.loads((bundle / "meta.json").read_text()).get("signature") == sig:
                mine.append(bundle)
    mine.sort(key=lambda b: b.name, reverse=True)
    for bundle in mine[KEEP_BUNDLES:]:
        shutil.rmtree(bundle, ignore_errors=True)
    for bundle in mine[KEEP_CORES:KEEP_BUNDLES]:
        for name in ("core", "core.zst"):
            (bundle / name).unlink(missing_ok=True)

    oldest_first = sorted(BUNDLES.iterdir(), key=lambda b: b.name)
    for bundle in oldest_first[: max(len(oldest_first) - MAX_BUNDLES, 0)]:
        shutil.rmtree(bundle, ignore_errors=True)
    cores = sorted(BUNDLES.glob("*/core.zst"), key=lambda p: p.parent.name)
    total = sum(p.stat().st_size for p in cores)
    for core in cores:
        if total <= MAX_CORE_BYTES:
            break
        total -= core.stat().st_size
        core.unlink()


def worker(work):
    while True:
        bundle = work.get()
        try:
            process(bundle)
        except Exception:  # one bad bundle must not stop the watcher
            log(f"{bundle.name}: processing failed\n{traceback.format_exc()}")
        finally:
            work.task_done()


def on_event(line, seen, work):
    ev = json.loads(line)
    cid = ev["Actor"]["ID"]
    # The container filter also matches longer names (moqx-grafana).
    if ev["Actor"]["Attributes"].get("name") != CONTAINER:
        return
    if ev["Action"] == "start":
        seen["started"][cid] = int(ev["timeNano"]) / 10**9
        return
    if ev["Action"] == "oom":
        seen["oom"].add(cid)
        return
    code = int(ev["Actor"]["Attributes"].get("exitCode", "0"))
    signo = code - 128 if code > 128 else 0
    oom = signo == 9 and cid in seen["oom"]
    seen["oom"].discard(cid)
    run_start = seen["started"].pop(cid, None)
    if signo not in CRASH_SIGNALS and not oom:
        log(f"{CONTAINER} exited with code {code}, not a crash")
        return
    work.put(capture(ev, signo, oom, run_start))


def watch():
    BUNDLES.mkdir(parents=True, exist_ok=True)
    work = queue.Queue()
    threading.Thread(target=worker, args=(work,), daemon=True).start()
    github = "dry run" if DRY_RUN else ("on" if GH_TOKEN else "off (no GH_TOKEN)")
    log(f"watching container {CONTAINER}, GitHub reporting {github}")
    events = ["docker", "events", "--format", "{{json .}}"]
    for f in [
        "type=container",
        f"container={CONTAINER}",
        "event=start",
        "event=die",
        "event=oom",
    ]:
        events += ["--filter", f]
    seen = {"oom": set(), "started": {}}  # by container id
    with subprocess.Popen(events, stdout=subprocess.PIPE, text=True) as proc:
        for line in proc.stdout:
            try:
                on_event(line, seen, work)
            except Exception:  # a restart would miss the events in between
                log(f"event failed: {line.strip()[:300]}\n{traceback.format_exc()}")
    work.join()
    sys.exit("docker events ended")


def main():
    args = sys.argv[1:] or ["watch"]
    if args == ["watch"]:
        watch()
    elif len(args) == 2 and args[0] in ("decode", "gdb"):
        bundle = Path(args[1]) if Path(args[1]).is_dir() else BUNDLES / args[1]
        image_id = json.loads((bundle / "meta.json").read_text())["image_id"]
        with raw_core(bundle) as core:
            if args[0] == "gdb":
                image = gdb_image(image_id) or sys.exit("no gdb image")
                mount = ["-v", f"{core.parent}:/crash:ro"]
                subprocess.run(
                    ["docker", "run", "--rm", "-it", "--network", "none", *mount]
                    + [image, BINARY, f"/crash/{core.name}"]
                )
                return
            text = gdb_decode(image_id, core) or ""
        signal, si_code, fault, frames = gdb_report(text)
        sig, top = signature(frames)
        print(f"{signal} si_code {si_code} fault {fault} signature {sig}")
        print(" → ".join(top))
        print(render(frames))
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
