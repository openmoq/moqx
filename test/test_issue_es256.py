#!/usr/bin/env python3
# Copyright (c) OpenMOQ contributors.
# Licensed under the Apache 2.0 license in the root LICENSE file.

import subprocess
import sys
import tempfile
from pathlib import Path

issuer = str(Path(sys.argv[1]).resolve())
fixtures = Path(__file__).resolve().parent
public = fixtures / "c4m_es256_public.pem"
private = fixtures / "c4m_es256_private.pem"
with tempfile.TemporaryDirectory(prefix="moqx-971-cli-") as work:
    root = Path(work)
    config = root / "relay.yaml"
    config.write_text(
        '''listeners:
  main:
    udp: {socket: {address: "127.0.0.1", port: 9668}}
    tls: {insecure: true}
    endpoint: "/moq"
service_defaults:
  max_subscription_buffer_kb: 51200
services:
  live:
    match: [{authority: {any: true}, path: {prefix: "/"}}]
    cache: {enabled: true, max_tracks: 100, max_groups_per_track: 3}
    auth:
      enabled: true
      es256_keys: [{id: "es-key", public_key_file: "'''
        + str(public)
        + """"}]
      audiences: ["relay-a"]
      min_revalidation_interval_seconds: 1
      max_tokens_per_message: 4
"""
    )
    base = [
        "--auth_actions=setup,publish",
        "--auth_namespace=live",
        "--auth_track=video",
        "--auth_output=hex",
    ]

    def run(name, args, good):
        result = subprocess.run(
            [issuer, *base, *args],
            stdin=subprocess.DEVNULL,
            capture_output=True,
            text=True,
            timeout=15,
        )
        assert (result.returncode == 0) == good, (
            name,
            result.returncode,
            result.stderr[-1500:],
        )
        if good:
            data = bytes.fromhex(result.stdout.strip())
            assert data[0] == (0xD1 if name == "hmac" else 0xD2), name
        else:
            assert not result.stdout.strip(), (name, "stdout is not empty on error")
            assert "local-test-secret" not in result.stdout + result.stderr, (
                name,
                "secret leaked",
            )
        print(name + ": PASS")

    run(
        "es256-direct",
        ["--auth_private_key_file=" + str(private), "--auth_key_id=es-key"],
        True,
    )
    configured = ["--config=" + str(config), "--auth_service=live"]
    run(
        "es256-public-only-config",
        [*configured, "--auth_private_key_file=" + str(private)],
        True,
    )
    run("public-only-cannot-mint-without-private-key", configured, False)
    run(
        "mutually-exclusive-signing-options",
        ["--auth_private_key_file=" + str(private), "--auth_secret=local-test-secret"],
        False,
    )
    run(
        "unknown-configured-key",
        [
            *configured,
            "--auth_private_key_file=" + str(private),
            "--auth_key_id=unknown",
        ],
        False,
    )
    wrong = root / "wrong-private.pem"
    subprocess.run(
        [
            "openssl",
            "genpkey",
            "-algorithm",
            "EC",
            "-pkeyopt",
            "ec_paramgen_curve:P-256",
            "-out",
            str(wrong),
        ],
        check=True,
        capture_output=True,
    )
    run(
        "mismatched-private-public-key",
        [*configured, "--auth_private_key_file=" + str(wrong)],
        False,
    )
    encrypted = root / "encrypted-private.pem"
    subprocess.run(
        [
            "openssl",
            "pkey",
            "-in",
            str(private),
            "-aes-256-cbc",
            "-passout",
            "pass:test",
            "-out",
            str(encrypted),
        ],
        check=True,
        capture_output=True,
    )
    run(
        "encrypted-private-key-does-not-prompt",
        ["--auth_private_key_file=" + str(encrypted)],
        False,
    )
    # The key-consistency check must accept the claims the token was issued with.
    run(
        "es256-config-with-audience",
        [
            *configured,
            "--auth_private_key_file=" + str(private),
            "--auth_audience=relay-a",
        ],
        True,
    )
    run(
        "es256-config-with-revalidation",
        [
            *configured,
            "--auth_private_key_file=" + str(private),
            "--auth_revalidate_seconds=30",
        ],
        True,
    )
    # A bad signature is still reported as a key mismatch when claims are present.
    run(
        "mismatched-key-with-audience",
        [
            *configured,
            "--auth_private_key_file=" + str(wrong),
            "--auth_audience=relay-a",
        ],
        False,
    )
    run("hmac", ["--auth_secret=local-validation-only", "--auth_key_id=hmac-key"], True)
