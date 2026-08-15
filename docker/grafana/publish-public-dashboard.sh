#!/usr/bin/env bash
# Manage the PUBLIC (tokenized, unauthenticated, read-only) view of a moqx
# Grafana dashboard — served ONLY on the nginx public port (STATS_PUBLIC_PORT),
# never on the private tunnel port.
#
#   ./publish-public-dashboard.sh publish     enable + print the public URL
#   ./publish-public-dashboard.sh status       show enabled state + URL (default)
#   ./publish-public-dashboard.sh pause        disable (keeps the token — kill switch)
#   ./publish-public-dashboard.sh resume       re-enable
#   ./publish-public-dashboard.sh unpublish    delete the public dashboard entirely
#
# Runs on the box (docker + python3), reaching Grafana via the container.
# Env: GRAFANA_CONTAINER (moqx-grafana), PROXY_CONTAINER (moqx-proxy),
#      DASH_UID (moqx-overview).
set -euo pipefail
export GRAFANA_CONTAINER="${GRAFANA_CONTAINER:-moqx-grafana}"
export PROXY_CONTAINER="${PROXY_CONTAINER:-moqx-proxy}"
export DASH_UID="${DASH_UID:-moqx-overview}"
export ACTION="${1:-status}"
exec python3 - <<'PY'
import os, sys, json, subprocess

C = os.environ["GRAFANA_CONTAINER"]; P = os.environ["PROXY_CONTAINER"]
DASH = os.environ["DASH_UID"]; ACTION = os.environ["ACTION"]
gpass = subprocess.check_output(["docker", "exec", C, "printenv", "GF_SECURITY_ADMIN_PASSWORD"], text=True).strip()

def api(method, path, body=None):
    cmd = ["docker", "exec"] + (["-i"] if body is not None else []) + \
          [C, "curl", "-s", "-u", f"admin:{gpass}", "-X", method]
    if body is not None:
        cmd += ["-H", "Content-Type: application/json", "-d", "@-"]
    cmd += [f"http://localhost:3000{path}"]
    r = subprocess.run(cmd, input=(json.dumps(body) if body is not None else None),
                       text=True, capture_output=True)
    return r.stdout

def get_pd():
    try:
        d = json.loads(api("GET", f"/api/dashboards/uid/{DASH}/public-dashboards"))
        return d if d.get("accessToken") else {}
    except Exception:
        return {}

root = subprocess.check_output(["docker", "exec", C, "printenv", "GF_SERVER_ROOT_URL"], text=True).strip()
domain = root.split("://", 1)[1].split("/", 1)[0]
try:
    port = subprocess.check_output(["docker", "exec", P, "printenv", "STATS_PUBLIC_PORT"], text=True).strip()
except Exception:
    port = "4533"
def url(tok): return f"https://{domain}:{port}/grafana/public-dashboards/{tok}"

pd = get_pd()
if ACTION == "publish":
    if pd.get("accessToken"):
        api("PATCH", f"/api/dashboards/uid/{DASH}/public-dashboards/{pd['uid']}", {"isEnabled": True})
        tok = pd["accessToken"]
    else:
        r = json.loads(api("POST", f"/api/dashboards/uid/{DASH}/public-dashboards",
                           {"isEnabled": True, "timeSelectionEnabled": True, "share": "public"}))
        tok = r["accessToken"]
    print("Published (read-only, unauthenticated). Public URL:")
    print("  " + url(tok))
    print("Reachable only when nginx is bound public (STATS_PUBLIC_BIND=0.0.0.0).")
elif ACTION in ("pause", "resume"):
    if not pd.get("uid"):
        print("Not published yet. Run: publish"); sys.exit(1)
    api("PATCH", f"/api/dashboards/uid/{DASH}/public-dashboards/{pd['uid']}",
        {"isEnabled": ACTION == "resume"})
    print(("Resumed" if ACTION == "resume" else "Paused (public URL now 404)") + f"  — {url(pd['accessToken'])}")
elif ACTION == "unpublish":
    if pd.get("uid"):
        api("DELETE", f"/api/dashboards/uid/{DASH}/public-dashboards/{pd['uid']}")
        print("Deleted the public dashboard (token revoked).")
    else:
        print("Nothing published.")
else:  # status
    if pd.get("accessToken"):
        print(f"enabled={pd.get('isEnabled')}   {url(pd['accessToken'])}")
    else:
        print("Not published. Run: ./publish-public-dashboard.sh publish")
PY
