#!/usr/bin/env python3
"""《anemoi》交付物回归：独立审计 + 包清单一致性 + 负例。

- 审计脚本不得 import 打包器（保持“独立源审计”的独立性）；
- 审计对真实包 PASS，对篡改过的包 FAIL；
- 清单里的字节数/段表/哈希必须与二进制一致。
"""
from __future__ import annotations

import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PACK = os.path.join(ROOT, "main", "anemoi_data", "anemoi_pack.bin")
MANIFEST = os.path.join(ROOT, "main", "anemoi_data", "anemoi_pack.json")
AUDIT = os.path.join(ROOT, "tools", "anemoi_audit.py")
SOURCE = os.environ.get("ANEMOI_SOURCE", "")


def test_audit_is_independent():
    text = open(AUDIT, encoding="utf-8").read()
    # 审计只读源快照与二进制包，不 import 打包器的解析/编码实现。
    assert "anemoi_pack import" not in text
    assert "import anemoi_pack" not in text
    assert "from anemoi_pack" not in text
    assert "tools.anemoi_pack" not in text


def test_manifest_matches_pack():
    blob = open(PACK, "rb").read()
    manifest = json.load(open(MANIFEST, encoding="utf-8"))
    assert manifest["bytes"] == len(blob)
    assert manifest["sha256"] == hashlib.sha256(blob).hexdigest()
    version, header_size, count = struct.unpack_from("<HHH", blob, 8)
    assert (version, header_size, count) == (1, 28, 24)
    total = struct.unpack_from("<I", blob, 16)[0]
    assert total == len(blob)
    for i in range(count):
        sec_type, off, cnt, size = struct.unpack_from("<IIII", blob, 28 + 16 * i)
        assert sec_type == i
        assert off + size <= total
        info = manifest["sections"][
            ["text", "txidx", "ops", "scene", "scnid", "entry", "chap", "var", "expr", "cond",
             "call", "set", "choice", "namelist", "pose", "actor", "image", "blob", "req",
             "helper", "micro", "gal", "namemap", "meta"][sec_type]]
        assert info["size"] == size and info["count"] == cnt
    assert manifest["ops"] > 50000
    assert manifest["images"]["body"] > 300
    assert manifest["images"]["face"] > 1000


def test_audit_pass_and_tamper():
    if not SOURCE or not os.path.isdir(SOURCE):
        print("SKIP: 未设置 ANEMOI_SOURCE，跳过独立审计（需要源快照）")
        return
    env = dict(os.environ, PYTHONIOENCODING="utf-8")
    result = subprocess.run([sys.executable, AUDIT, "--source", SOURCE, "--pack", PACK],
                            cwd=ROOT, capture_output=True, text=True, encoding="utf-8",
                            env=env)
    assert result.returncode == 0, result.stdout + result.stderr
    assert "审计: PASS" in result.stdout
    with tempfile.TemporaryDirectory() as tmp:
        broken = os.path.join(tmp, "broken.bin")
        blob = bytearray(open(PACK, "rb").read())
        blob[100000] ^= 0x5A  # 改一条指令
        open(broken, "wb").write(bytes(blob))
        result = subprocess.run([sys.executable, AUDIT, "--source", SOURCE, "--pack", broken],
                                cwd=ROOT, capture_output=True, text=True, encoding="utf-8",
                                env=env)
        assert result.returncode != 0
        assert "审计: FAIL" in result.stdout


def test_measure_does_not_touch_formal_pack():
    """--measure 必须只写独立目录：正式包/manifest/字库逐字节不变，且拒绝写正式目录。"""
    if not SOURCE or not os.path.isdir(SOURCE):
        print("SKIP: 未设置 ANEMOI_SOURCE")
        return
    formal = [PACK, MANIFEST,
              os.path.join(ROOT, "assets", "fonts", "gal_cjk_16.c"),
              os.path.join(ROOT, "assets", "fonts", "gal_cjk_symbols.txt")]
    before = {}
    for path in formal:
        if os.path.exists(path):
            before[path] = hashlib.sha256(open(path, "rb").read()).hexdigest()
    env = dict(os.environ, PYTHONIOENCODING="utf-8")
    with tempfile.TemporaryDirectory() as tmp:
        out = os.path.join(tmp, "measure")
        result = subprocess.run(
            [sys.executable, os.path.join(ROOT, "tools", "anemoi_pack.py"),
             "--source", SOURCE, "--measure", "--scope", "common",
             "--layout-scale", "0.52", "--palette-colors", "32", "--jpeg-quality", "30",
             "--out-dir", out],
            cwd=ROOT, capture_output=True, text=True, encoding="utf-8", env=env)
        assert result.returncode == 0, result.stdout[-800:] + result.stderr[-800:]
        assert os.path.exists(os.path.join(out, "anemoi_pack.bin")), "measurement did not write its pack"
        assert os.path.exists(os.path.join(out, "measure.json")), "measurement did not write its summary"
    for path, digest in before.items():
        assert hashlib.sha256(open(path, "rb").read()).hexdigest() == digest, (
            "measurement changed formal artifact: %s" % path)
    # 明确拒绝把测量写进正式目录
    result = subprocess.run(
        [sys.executable, os.path.join(ROOT, "tools", "anemoi_pack.py"),
         "--source", SOURCE, "--measure", "--scope", "common",
         "--out-dir", "main/anemoi_data"],
        cwd=ROOT, capture_output=True, text=True, encoding="utf-8", env=env)
    assert result.returncode != 0, "measurement accepted the formal data directory"
    assert "拒绝" in (result.stdout + result.stderr)


if __name__ == "__main__":
    tests = [test_audit_is_independent, test_manifest_matches_pack, test_audit_pass_and_tamper,
             test_measure_does_not_touch_formal_pack]
    failures = 0
    for test in tests:
        try:
            test()
            print("PASS", test.__name__)
        except AssertionError as exc:
            failures += 1
            print("FAIL", test.__name__, exc)
    raise SystemExit(1 if failures else 0)
