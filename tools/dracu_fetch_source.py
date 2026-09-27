#!/usr/bin/env python3
"""Fetch the DRACU-RIOT (MiBand port) source material for pack builds.

Source project: https://github.com/hezdaaa/dracu-riot-miband
  - 小米手环快应用上的《DRACU-RIOT!》(Yuzusoft)移植,自带一条 KiriKiri2 解包 →
    全局线性页表 → 立绘分层合成的工具链。
  - 剧本、背景、立绘、事件 CG 版权归 Yuzusoft 所有;本仓库只保存转换产物
    (main/dracu_data/),不分发源素材 —— 需要重建 pack 时用本工具按需拉取。

The checkout is about 60 MB (mostly the converted 336x480 art and the page
tables), so it stays in build/ and never lands in the repository.

Usage:
  python tools/dracu_fetch_source.py
  python tools/dracu_fetch_source.py --dest build/dracu-source --ref <commit>
  python tools/dracu_fetch_source.py --dest build/dracu-source --proxy http://127.0.0.1:7897

Existing checkouts are reused: the tool only clones when --dest is missing,
and re-checks out --ref when an explicit ref is given.
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
from pathlib import Path

REPO = "https://github.com/hezdaaa/dracu-riot-miband.git"
DEFAULT_DEST = Path("build/dracu-source")
MARKER = Path("src/common/script")
MANIFEST = "SOURCE.txt"


def log(message: str) -> None:
    print(message, file=sys.stderr)


def run(command: list[str], env: dict[str, str]) -> None:
    log("$ " + " ".join(command))
    subprocess.run(command, check=True, env=env)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="拉取 dracu-riot-miband 源素材")
    parser.add_argument("--dest", type=Path, default=DEFAULT_DEST)
    parser.add_argument("--ref", default="", help="指定 commit / tag(缺省用默认分支)")
    parser.add_argument("--proxy", default=os.environ.get("DRACU_PROXY", ""),
                        help="HTTPS 代理,例如 http://127.0.0.1:7897")
    parser.add_argument("--depth", type=int, default=1)
    args = parser.parse_args(argv)

    env = dict(os.environ)
    if args.proxy:
        env["https_proxy"] = env["http_proxy"] = args.proxy
    env.setdefault("GIT_TERMINAL_PROMPT", "0")

    dest: Path = args.dest
    if (dest / MARKER).is_dir():
        log(f"已有 checkout: {dest}")
        if args.ref:
            run(["git", "-C", str(dest), "fetch", "--depth", str(args.depth), "origin", args.ref], env)
            run(["git", "-C", str(dest), "checkout", "--detach", "FETCH_HEAD"], env)
    else:
        dest.parent.mkdir(parents=True, exist_ok=True)
        command = ["git", "clone", "--depth", str(args.depth)]
        if args.ref:
            command += ["--branch", args.ref]
        command += [REPO, str(dest)]
        run(command, env)

    if not (dest / MARKER).is_dir():
        log(f"{dest} 里没有 {MARKER},不像 dracu-riot-miband 的 checkout")
        return 1

    head = subprocess.run(["git", "-C", str(dest), "rev-parse", "HEAD"], check=True,
                          capture_output=True, text=True, env=env).stdout.strip()
    size = sum(path.stat().st_size for path in dest.rglob("*") if path.is_file())
    log(f"HEAD {head}; {size / 1048576:.1f} MB")
    (dest / MANIFEST).write_text(
        f"repo={REPO}\nhead={head}\n", encoding="utf-8")
    log("可以开始打包: python tools/dracu_pack.py --source %s --out build/dracu-pack/dracu_pack.bin" % dest)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
