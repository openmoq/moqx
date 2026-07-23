#!/usr/bin/env bash
# Import the provisioned moqx dashboards as EDITABLE working copies so you can
# tweak them in the Grafana UI. The provisioned originals stay read-only (the
# landing page); the copies get uid "<uid>-dev" + title "… (dev)" in a
# "moqx (dev)" folder. When you're happy, run ./export-dashboards.sh to write
# your edits back to the version-controlled JSON here.
#
# Runs on the box (needs docker + python3). Reaches Grafana via the container,
# so no published port / basic-auth juggling. Override the container name with
# GRAFANA_CONTAINER (default: moqx-grafana).
set -euo pipefail
cd "$(dirname "$0")"
export GRAFANA_CONTAINER="${GRAFANA_CONTAINER:-moqx-grafana}"
exec python3 - <<'PY'
import json, os, subprocess, glob

C = os.environ["GRAFANA_CONTAINER"]
SRC = "provisioning/dashboards"
gpass = subprocess.check_output(
    ["docker", "exec", C, "printenv", "GF_SECURITY_ADMIN_PASSWORD"], text=True).strip()

def gcurl(args, body=None):
    cmd = ["docker", "exec"] + (["-i"] if body is not None else []) + \
          [C, "curl", "-sf", "-u", f"admin:{gpass}"] + args
    return subprocess.run(cmd, input=body, text=True, capture_output=True, check=True).stdout

# Ensure the "moqx (dev)" folder exists.
folders = json.loads(gcurl(["http://localhost:3000/api/folders"]))
fuid = next((f["uid"] for f in folders if f["title"] == "moqx (dev)"), "")
if not fuid:
    r = gcurl(["-H", "Content-Type: application/json", "-d", "@-",
               "http://localhost:3000/api/folders"], body='{"title":"moqx (dev)"}')
    fuid = json.loads(r)["uid"]

files = sorted(glob.glob(f"{SRC}/*.json"))
if not files:
    print(f"No dashboards in {SRC}/"); raise SystemExit
for f in files:
    d = json.load(open(f))
    d.pop("id", None); d.pop("version", None)
    d["uid"] = d["uid"] + "-dev"
    d["title"] = d["title"] + " (dev)"
    d["editable"] = True
    payload = json.dumps({"dashboard": d, "folderUid": fuid,
                          "overwrite": True, "message": "dev copy"})
    gcurl(["-H", "Content-Type: application/json", "-d", "@-",
           "http://localhost:3000/api/dashboards/db"], body=payload)
    print(f"  imported {os.path.basename(f)} -> {d['uid']}  ({d['title']})")
print("\nEdit the '(dev)' copies in Grafana, then: ./export-dashboards.sh")
PY
