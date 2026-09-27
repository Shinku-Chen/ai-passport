#!/usr/bin/env python3
"""《沙耶之歌》资源包的可重建性守卫(纯 Python,不需要编译器/IDF)。

仓库里提交了 main/saya_data/saya_pack.bin(社区变体),文档承诺"用同一 commit 与默认
参数重建可以逐字节复现"。这个承诺一旦被打破 —— 比如改了打包器忘了重新提交 pack,
或者素材被误改 —— 社区变体就和源码脱节了,而设备上跑的还是旧包,很难发现。所以放进
静态门禁:

  1. 已提交的社区 pack 通过 tools/saya_pack.py --check(结构/尺寸/接缝/遮罩/元数据);
  2. 用 assets/saya-source 与 pack 元数据里记录的 commit 重建,结果与提交的 pack
     逐字节一致;
  3. 用 assets/saya-patch 构建 release 变体并通过自检(补丁源文件可用;产物只落在
     临时目录,不写进仓库)。
"""

from __future__ import annotations

import os
import struct
import subprocess
import sys
import tempfile

COMMUNITY_PACK = os.path.join("main", "saya_data", "saya_pack.bin")
RELEASE_PACK_NAME = "saya_pack_release_check.bin"
MAGIC = b"SAYAPK01"
SEC_META = 7


def pack_meta(path: str) -> dict:
    """读 pack 元数据段的 key=value(打包器与设备端都依赖这个段)。"""
    with open(path, "rb") as fh:
        data = fh.read()
    if data[:8] != MAGIC:
        raise SystemExit(f"ERROR: {path} 不是资源包")
    section_count = struct.unpack_from("<I", data, 16)[0]
    for i in range(section_count):
        sec_type, off, _count, size = struct.unpack_from("<IIII", data, 20 + 16 * i)
        if sec_type != SEC_META:
            continue
        meta = {}
        for line in data[off:off + size].decode("utf-8", "replace").splitlines():
            if "=" in line:
                key, value = line.split("=", 1)
                meta[key.strip()] = value.strip()
        return meta
    raise SystemExit(f"ERROR: {path} 缺少元数据段")


def run_pack(tool: str, args: list, errors: list) -> None:
    proc = subprocess.run([sys.executable, tool] + args, capture_output=True, text=True)
    if proc.returncode != 0:
        errors.append(f"saya_pack.py {' '.join(args)} 失败:\n{proc.stdout}{proc.stderr}")
    else:
        tail = (proc.stdout.strip().splitlines() or [""])[-1]
        print(f"  {tail}")


def main() -> int:
    root = sys.argv[1] if len(sys.argv) > 1 else "."
    os.chdir(root)

    tool = os.path.join("tools", "saya_pack.py")
    if not os.path.isfile(tool):
        raise SystemExit("ERROR: 找不到 tools/saya_pack.py")
    if not os.path.isfile(COMMUNITY_PACK):
        raise SystemExit(f"ERROR: 找不到 {COMMUNITY_PACK}")

    errors: list = []

    # 1) 提交的社区包自检
    print("检查已提交的社区资源包:")
    run_pack(tool, ["--check", COMMUNITY_PACK], errors)
    meta = pack_meta(COMMUNITY_PACK)
    if meta.get("variant") != "community":
        errors.append(f"提交的 pack 记录了 variant={meta.get('variant')!r},应为 community")
    if meta.get("patch"):
        errors.append(f"社区 pack 不应记录补丁目录,实际为 {meta.get('patch')!r}")

    # 2) 逐字节重建
    print("从 assets/saya-source 重建社区资源包:")
    commit = meta.get("commit") or ""
    if not commit or commit == "unknown":
        errors.append("元数据里没有可用的源 commit,无法验证重建")
    else:
        with tempfile.TemporaryDirectory(prefix="saya-pack-rebuild-") as tmp:
            rebuilt = os.path.join(tmp, "saya_pack.bin")
            run_pack(tool, ["--source", "assets/saya-source", "--commit", commit,
                            "--out", rebuilt], errors)
            if os.path.isfile(rebuilt):
                with open(rebuilt, "rb") as fh:
                    got = fh.read()
                with open(COMMUNITY_PACK, "rb") as fh:
                    want = fh.read()
                if got != want:
                    errors.append(
                        f"重建结果与提交的 {COMMUNITY_PACK} 不一致: "
                        f"{len(got)} 字节 vs {len(want)} 字节 —— 改了打包器就要重新提交 pack")
                else:
                    print(f"  重建结果与提交的 pack 逐字节一致({len(want)} 字节)")

    # 3) 补丁源文件可用(release 变体),产物只落临时目录
    print("从 assets/saya-patch 构建 release 变体并自检:")
    if not os.path.isdir("assets/saya-patch"):
        errors.append("缺少 assets/saya-patch(补丁源文件),release 变体无法重建")
    else:
        with tempfile.TemporaryDirectory(prefix="saya-pack-release-") as tmp:
            release = os.path.join(tmp, RELEASE_PACK_NAME)
            run_pack(tool, ["--source", "assets/saya-source", "--patch", "assets/saya-patch",
                            "--commit", commit or "unknown", "--out", release], errors)
            if os.path.isfile(release):
                run_pack(tool, ["--check", release], errors)
                rel_meta = pack_meta(release)
                if rel_meta.get("variant") != "release":
                    errors.append(f"release pack 的 variant={rel_meta.get('variant')!r}")
                if not rel_meta.get("patch"):
                    errors.append("release pack 没有记录补丁目录")

    if errors:
        for line in errors:
            print(f"ERROR: {line}", file=sys.stderr)
        print("Saya pack rebuild check: FAIL")
        return 1
    print("Saya pack rebuild check: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
