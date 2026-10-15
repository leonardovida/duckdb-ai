#!/usr/bin/env python3
"""Check packaging/signing locally; AWS and Brotli are stand-ins, no uploads."""

import gzip
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import uuid
from pathlib import Path


def run():
    script = Path(__file__).resolve().parents[2] / "scripts/extension-upload.sh"
    openssl = shutil.which("openssl")
    if not openssl:
        raise RuntimeError("openssl is required for the release packaging smoke")
    name = "upload-smoke-" + uuid.uuid4().hex
    extension_dir = Path("/tmp/extension")
    extension_dir.mkdir(exist_ok=True)
    extension = extension_dir / (name + ".duckdb_extension")
    wasm = Path(str(extension) + ".wasm")
    original = b"deterministic extension upload fixture\n" * 40000
    extension.write_bytes(original)
    wasm.write_bytes(b"\x00asm\x01\x00\x00\x00")
    checks = 0
    try:
        with tempfile.TemporaryDirectory(prefix="upload-smoke-") as temporary:
            root = Path(temporary)
            tools = root / "tools"
            tools.mkdir()
            wrapper = f"""#!{sys.executable}
import json, os, stat, sys
from pathlib import Path
args = sys.argv[1:]
if 'pkeyutl' in args and '-sign' in args:
    key = Path(args[args.index('-inkey') + 1])
    assert stat.S_IMODE(key.stat().st_mode) == 0o600
    Path(os.environ['SIGN_LOG']).write_text(str(key.parent))
    if os.environ.get('FAIL_SIGN'):
        sys.exit(17)
os.execv({openssl!r}, [{openssl!r}] + args)
"""
            standins = {
                "openssl": wrapper,
                "aws": f"#!{sys.executable}\nimport json, os, sys\nwith open(os.environ['AWS_LOG'], 'a') as f:\n    f.write(json.dumps(sys.argv[1:]) + '\\n')\n",
                "brotli": f"#!{sys.executable}\nimport shutil, sys\nshutil.copyfileobj(sys.stdin.buffer, sys.stdout.buffer)\n",
            }
            for tool, content in standins.items():
                path = tools / tool
                path.write_text(content)
                path.chmod(0o700)
            env = os.environ.copy()
            for variable in ("DUCKDB_EXTENSION_SIGNING_PK", "AWS_ACCESS_KEY_ID"):
                env.pop(variable, None)
            env.update(
                PATH=str(tools) + os.pathsep + env["PATH"],
                SIGN_LOG=str(root / "sign.log"),
                AWS_LOG=str(root / "aws.log"),
            )
            (root / "private.pem").write_text("caller-owned file")
            (root / "x-important.txt").write_text("caller-owned hash sentinel")

            def invoke(architecture="linux_amd64", overrides=None, flags=()):
                return subprocess.run(
                    ["bash", str(script), name, "test-version", "v1.5.5", architecture, "mock-bucket", *flags],
                    cwd=root,
                    env=env | (overrides or {}),
                    text=True,
                    capture_output=True,
                    timeout=30,
                )

            def sidecar(suffix):
                return Path(str(extension) + "." + suffix)

            def assert_clean():
                assert (root / "private.pem").read_text() == "caller-owned file"
                assert (root / "x-important.txt").read_text() == "caller-owned hash sentinel"
                if (root / "sign.log").exists():
                    assert not Path((root / "sign.log").read_text()).exists(), "signing staging directory leaked"

            # No optional environment variables or upload flags are required.
            result = invoke()
            assert result.returncode == 0, result.stderr
            assert gzip.decompress(sidecar("compressed").read_bytes()) == original + bytes(256)
            assert_clean()
            checks += 1

            key = root / "test-signing-key.pem"
            subprocess.run(
                [openssl, "genpkey", "-algorithm", "RSA", "-pkeyopt", "rsa_keygen_bits:2048", "-out", str(key)],
                check=True,
                capture_output=True,
            )
            signing = {"DUCKDB_EXTENSION_SIGNING_PK": key.read_text()}
            result = invoke(overrides=signing)
            assert result.returncode == 0, result.stderr
            signature = sidecar("sign").read_bytes()
            assert len(signature) == 256 and signature != bytes(256)
            assert gzip.decompress(sidecar("compressed").read_bytes()) == original + signature
            chunk_hashes = b"".join(
                hashlib.sha256(original[i : i + 1024 * 1024]).digest() for i in range(0, len(original), 1024 * 1024)
            )
            expected_hash = hashlib.sha256(chunk_hashes).digest()
            assert sidecar("hash").read_bytes() == expected_hash
            public = root / "public.pem"
            subprocess.run(
                [openssl, "pkey", "-in", str(key), "-pubout", "-out", str(public)], check=True, capture_output=True
            )
            subprocess.run(
                [
                    openssl,
                    "pkeyutl",
                    "-verify",
                    "-pubin",
                    "-inkey",
                    str(public),
                    "-in",
                    str(sidecar("hash")),
                    "-sigfile",
                    str(sidecar("sign")),
                    "-pkeyopt",
                    "digest:sha256",
                ],
                check=True,
                capture_output=True,
            )
            assert_clean()
            checks += 1

            result = invoke()
            assert result.returncode == 0, result.stderr
            assert sidecar("sign").read_bytes() == bytes(256), "unsigned rerun retained the old signature"
            assert gzip.decompress(sidecar("compressed").read_bytes()) == original + bytes(256)
            assert not sidecar("hash").exists()
            checks += 1

            before = sidecar("compressed").read_bytes()
            result = invoke(overrides=signing | {"FAIL_SIGN": "true"})
            assert result.returncode == 17, result.stderr
            assert sidecar("compressed").read_bytes() == before, "failed signing published partial sidecars"
            assert_clean()
            checks += 1

            # A successful OpenSSL call with an unsupported key must also fail.
            subprocess.run(
                [openssl, "genpkey", "-algorithm", "RSA", "-pkeyopt", "rsa_keygen_bits:1024", "-out", str(key)],
                check=True,
                capture_output=True,
            )
            result = invoke(overrides={"DUCKDB_EXTENSION_SIGNING_PK": key.read_text()})
            assert result.returncode and "256-byte" in result.stderr, result.stderr
            assert_clean()
            checks += 1

            # Both 'wasm' and 'wasm_mvp' must use the Brotli branch and headers.
            for architecture in ("wasm", "wasm_mvp"):
                result = invoke(architecture, {"AWS_ACCESS_KEY_ID": "dummy"}, ("true", "true"))
                assert result.returncode == 0, result.stderr
                packed = Path(str(wasm) + ".compressed").read_bytes()
                header = b"\x00\x93\x02\x10duckdb_signature\x80\x02"
                assert packed == wasm.read_bytes() + header + bytes(256)
                calls = [json.loads(line) for line in (root / "aws.log").read_text().splitlines()][-2:]
                for call in calls:
                    assert call[:2] == ["s3", "cp"]
                    assert call[3].endswith(f"/{architecture}/{name}.duckdb_extension.wasm")
                    assert call[-3:] == ["--content-encoding", "br", "--content-type=application/wasm"]
                assert_clean()
                checks += 1

            # Native versioned and latest upload paths are passed intact.
            result = invoke(overrides={"AWS_ACCESS_KEY_ID": "dummy"}, flags=("true", "true"))
            assert result.returncode == 0, result.stderr
            calls = [json.loads(line) for line in (root / "aws.log").read_text().splitlines()][-2:]
            assert calls[0][3] == f"s3://mock-bucket/{name}/test-version/v1.5.5/linux_amd64/{name}.duckdb_extension.gz"
            assert calls[1][3] == f"s3://mock-bucket/v1.5.5/linux_amd64/{name}.duckdb_extension.gz"
            checks += 1
    finally:
        for binary in (extension, wasm):
            for suffix in ("", ".append", ".sign", ".hash", ".compressed"):
                Path(str(binary) + suffix).unlink(missing_ok=True)
    print(f"extension upload smoke passed ({checks} checks)")


if __name__ == "__main__":
    run()
