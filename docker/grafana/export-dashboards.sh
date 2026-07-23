#!/usr/bin/env bash
# Export edited "-dev" moqx dashboards back to the version-controlled provisioned
# JSON (strips the -dev suffix + " (dev)" title), archiving the previous version.
# On the next clean redeploy the read-only provisioned dashboards reflect your
# edits. Run ./import-dashboards.sh first to create the editable copies.
#
#   ./export-dashboards.sh            write changes (archives prior versions)
#   ./export-dashboards.sh --dry-run  show diffs vs disk, write nothing
#
# Runs on the box (needs docker + python3). GRAFANA_CONTAINER overrides the
# container name (default: moqx-grafana).
set -euo pipefail
cd "$(dirname "$0")"
export GRAFANA_CONTAINER="${GRAFANA_CONTAINER:-moqx-grafana}"
export DRY_RUN="$([ "${1:-}" = "--dry-run" ] && echo 1 || true)"
exec python3 - <<'PY'
import json, os, subprocess, shutil, tempfile, time

C = os.environ["GRAFANA_CONTAINER"]
DRY = bool(os.environ.get("DRY_RUN"))
DEST = "provisioning/dashboards"; ARCH = f"{DEST}/archive"
os.makedirs(ARCH, exist_ok=True)
gpass = subprocess.check_output(
    ["docker", "exec", C, "printenv", "GF_SECURITY_ADMIN_PASSWORD"], text=True).strip()

def g(path):
    return subprocess.check_output(
        ["docker", "exec", C, "curl", "-sf", "-u", f"admin:{gpass}",
         f"http://localhost:3000{path}"], text=True)

items = [d for d in json.loads(g("/api/search?type=dash-db"))
         if d.get("uid", "").startswith("moqx-") and d["uid"].endswith("-dev")]
if not items:
    print("No '*-dev' dashboards found. Run ./import-dashboards.sh first."); raise SystemExit

stamp = time.strftime("%Y%m%d-%H%M%S")
changed = False
for it in items:
    d = json.loads(g(f"/api/dashboards/uid/{it['uid']}"))["dashboard"]
    d.pop("id", None); d.pop("version", None)
    d["uid"] = d["uid"][:-4]                       # strip -dev
    if d.get("title", "").endswith(" (dev)"):
        d["title"] = d["title"][:-6]
    path = f"{DEST}/{d['uid']}.json"
    content = json.dumps(d, indent=2) + "\n"
    old = open(path).read() if os.path.exists(path) else ""
    if old == content:
        print(f"  {d['uid']}.json: no changes"); continue
    changed = True
    if DRY:
        print(f"  {d['uid']}.json: DIFFERS")
        with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False) as t:
            t.write(content); tmp = t.name
        subprocess.run(["diff", "-u", path if old else "/dev/null", tmp])
        os.unlink(tmp)
    else:
        if old:
            shutil.copy2(path, f"{ARCH}/{d['uid']}-{stamp}.json")
            print(f"  archived -> archive/{d['uid']}-{stamp}.json")
        open(path, "w").write(content)
        print(f"  exported {d['uid']}.json")

if DRY and changed:
    print("\nDifferences found. Run without --dry-run to write.")
elif not changed:
    print("\nGrafana matches disk — nothing to export.")
PY
