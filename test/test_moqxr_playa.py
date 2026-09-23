#!/usr/bin/env python3
"""Opt-in moqxr -> A -> B -> C -> Playa media test; --demo keeps playback live."""

import argparse
import contextlib
import functools
import hashlib
import http.server
import json
import os
import shutil
import signal
import subprocess
import tempfile
import threading
from pathlib import Path
from urllib.parse import urlencode

from lib.moq_harness import Harness, HarnessError, _resolve_base_port

ROOT = Path(__file__).resolve().parents[1]


class MediaHarness(Harness):
    def _write_config(self, relay):
        super()._write_config(relay)
        config = relay.config_path.read_text()
        # Only replace the listener TLS block; peering still trusts local test TLS.
        config = config.replace(
            "      insecure: true",
            "      insecure: false\n      cert_file: "
            + json.dumps(str(self.tmpdir / "cert.pem"))
            + "\n"
            "      key_file: " + json.dumps(str(self.tmpdir / "key.pem")),
            1,
        )
        relay.config_path.write_text(config)


@contextlib.contextmanager
def process(argv, log, *, stdin=None, stdout=None):
    with log.open("w") as output:
        proc = subprocess.Popen(
            argv,
            stdin=stdin,
            stdout=output if stdout is None else stdout,
            stderr=output,
            start_new_session=True,
        )
        try:
            yield proc
        finally:
            if proc.poll() is None:
                os.killpg(proc.pid, signal.SIGTERM)
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    os.killpg(proc.pid, signal.SIGKILL)
                    proc.wait()


def run(args, directory):
    h = MediaHarness(args.binary, Path(), _resolve_base_port("playa"), directory, [])
    h.moqt_versions = "[18]"
    for name, upstream in (("A", None), ("B", "A"), ("C", "B")):
        h.relay(name, upstream=upstream)
    subprocess.run(
        [
            "openssl",
            "req",
            "-x509",
            "-newkey",
            "ec",
            "-pkeyopt",
            "ec_paramgen_curve:prime256v1",
            "-nodes",
            "-days",
            "7",
            "-subj",
            "/CN=localhost",
            "-addext",
            "subjectAltName=DNS:localhost,IP:127.0.0.1,IP:::1",
            "-keyout",
            str(directory / "key.pem"),
            "-out",
            str(directory / "cert.pem"),
        ],
        check=True,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    sample = directory / "sample.mp4"
    subprocess.run(
        [
            "ffmpeg",
            "-hide_banner",
            "-loglevel",
            "error",
            "-nostdin",
            "-y",
            "-f",
            "lavfi",
            "-i",
            "testsrc2=size=320x180:rate=24",
            "-f",
            "lavfi",
            "-i",
            "sine=frequency=880:sample_rate=48000",
            "-t",
            "8",
            "-c:v",
            "libx264",
            "-preset",
            "ultrafast",
            "-pix_fmt",
            "yuv420p",
            "-g",
            "24",
            "-sc_threshold",
            "0",
            "-c:a",
            "aac",
            "-b:a",
            "64k",
            "-movflags",
            "+frag_keyframe+empty_moov+default_base_moof+separate_moof",
            str(sample),
        ],
        check=True,
        timeout=30,
    )
    namespace = "cluster-media"
    url = f"https://localhost:{h.relays['C'].listen}/moq-relay"
    # Remux the loop as one live timeline. Reusing MP4 bytes with --loop also
    # reuses their tfdt timestamps, which causes MSE to stop at the loop boundary.
    remux = [
        "ffmpeg",
        "-hide_banner",
        "-loglevel",
        "error",
        "-nostdin",
        "-re",
        "-stream_loop",
        "-1",
        "-i",
        str(sample),
        "-c",
        "copy",
        "-movflags",
        "+frag_keyframe+empty_moov+default_base_moof+separate_moof",
        "-f",
        "mp4",
        "pipe:1",
    ]
    publisher = [
        str(args.moqxr),
        "--input",
        "-",
        "--namespace",
        namespace,
        "--draft",
        "18",
        "--endpoint",
        f"moqt://localhost:{h.relays['A'].listen}/moq-relay",
        "--insecure",
        "--forward",
        "0",
        "--coalesce-cmaf-chunks",
        "--catalog-republish-interval",
        "2",
        "--timeout",
        "86400",
    ]
    try:
        h.start()
        with (
            process(remux, directory / "ffmpeg.log", stdout=subprocess.PIPE) as source,
            process(publisher, directory / "moqxr.log", stdin=source.stdout) as pub,
        ):
            source.stdout.close()
            h.wait_namespace("C", namespace)
            subscriber = [
                args.node,
                str(ROOT / "test/playa/subscribe.mjs"),
                str(args.playa),
                url,
                namespace,
                str(directory / "cert.pem"),
                str(args.timeout),
            ]
            with process(subscriber, directory / "playa.log") as sub:
                result = sub.wait(timeout=args.timeout + 10)
            print((directory / "playa.log").read_text(), end="")
            if result or pub.poll() is not None or source.poll() is not None:
                raise HarnessError(
                    f"Playa exit={result}; publisher exit={pub.poll()}; FFmpeg exit={source.poll()}"
                )
            for name in h.relays:
                h.expect_relay_alive(name)
                h.expect_namespace_present(name, namespace)
            if h.failures:
                raise HarnessError(f"{h.failures} topology assertions failed")
            if args.demo:
                demo(args, directory, url, namespace, pub)
    finally:
        if h.cleanup():
            raise HarnessError("a relay failed during teardown")


def demo(args, directory, url, namespace, publisher):
    # Serve only the three browser bundles and the demo, never the TLS key.
    web = directory / "web"
    web.mkdir()
    for package in ("player", "browser", "webtransport"):
        shutil.copyfile(
            args.playa / f"packages/{package}/dist/browser/index.js",
            web / f"{package}.js",
        )
    shutil.copyfile(ROOT / "test/playa/demo.html", web / "index.html")
    der = subprocess.check_output(
        ["openssl", "x509", "-in", str(directory / "cert.pem"), "-outform", "DER"]
    )
    query = urlencode(
        {"url": url, "ns": namespace, "hash": hashlib.sha256(der).hexdigest()}
    )
    handler = functools.partial(
        http.server.SimpleHTTPRequestHandler, directory=str(web)
    )
    with http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler) as server:
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        print(f"Open http://localhost:{server.server_port}/?{query}", flush=True)
        print("Click Play in Chrome/Chromium. Ctrl-C stops the demo.", flush=True)
        try:
            while publisher.poll() is None:
                threading.Event().wait(1)
            raise HarnessError("publisher exited during demo")
        except KeyboardInterrupt:
            pass
        finally:
            server.shutdown()
            thread.join()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "binary", type=Path, nargs="?", default=ROOT / "build/default/moqx"
    )
    parser.add_argument(
        "--moqxr", type=Path, default=ROOT.parent / "moqxr/build/openmoq-publisher"
    )
    parser.add_argument("--playa", type=Path, default=ROOT.parent / "moq-playa")
    parser.add_argument("--node", default="node")
    parser.add_argument("--timeout", type=int, default=25)
    parser.add_argument(
        "--save-logs", type=Path, default=ROOT / ".scratch/moqxr-playa-logs"
    )
    parser.add_argument("--demo", action="store_true")
    args = parser.parse_args()
    if args.timeout <= 0:
        parser.error("--timeout must be positive")
    args.binary, args.moqxr, args.playa = (
        p.resolve() for p in (args.binary, args.moqxr, args.playa)
    )
    for binary in (args.binary, args.moqxr):
        if not os.access(binary, os.X_OK):
            parser.error(f"not executable: {binary}")
    for package in ("webtransport", "msf", "player", "browser"):
        if not (args.playa / f"packages/{package}/dist/index.js").is_file():
            parser.error(f"build Playa first: missing {package}/dist/index.js")

    def stop(_signum, _frame):
        raise KeyboardInterrupt

    signal.signal(signal.SIGTERM, stop)
    with tempfile.TemporaryDirectory(prefix="moqx-playa-") as scratch:
        directory = Path(scratch)
        try:
            run(args, directory)
        except KeyboardInterrupt:
            return 130
        except (HarnessError, OSError, subprocess.SubprocessError) as error:
            print(f"FAIL: {error}")
            return 1
        finally:
            args.save_logs.mkdir(parents=True, exist_ok=True)
            for pattern in ("*.log", "*.yaml"):
                for file in directory.glob(pattern):
                    shutil.copy2(file, args.save_logs / file.name)
            print(f"Logs saved to {args.save_logs}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
