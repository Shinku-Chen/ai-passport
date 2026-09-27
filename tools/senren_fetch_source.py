#!/usr/bin/env python3
"""Fetch the Senren * Banka (MiBand port) source material for pack builds.

Source project: https://github.com/hrk666666/Senren-Banka-MiBand-10
  - 小米手环 10(Xiaomi Vela / aiot quick app)上的《千恋＊万花》移植版;内容来自
    https://github.com/hezdaaa/qlwh-mibandported,引擎与 UI 来自
    https://github.com/hrk666666/Sanoba-Witch-MiBand-10。
  - 剧本、立绘、背景、事件图版权归 SAGA PLANETS 所有;本仓库只保存转换产物
    (main/senren_data/),不再分发源素材 —— 重建 pack 时用本工具按需拉取。

The repository does not carry the source material (about 16 MB of third-party
art and script), so this tool pulls it from upstream on demand.  The file list
comes from the jsDelivr data API and the payloads from the jsDelivr CDN, with
raw.githack.com as a fallback, because a direct github.com clone is not
reachable from every build host.

Layout written below --dest (same shape the packer expects):

  <dest>/bg/*.jpg       92 背景(336x480 JPEG)
  <dest>/ch/*.png      123 立绘(紧裁调色板 PNG,高 480)
  <dest>/ev/*          570 事件 CG(ev*.jpg) / SD 装饰(sd*.png) / 特效 / 道具 / 画面
  <dest>/scn/*.txt     112 剧本块(--chunks,打包脚本用)
  <dest>/MANIFEST.json 每个文件的相对路径、字节数与 sha256,便于复现校验

Usage:
  python tools/senren_fetch_source.py
  python tools/senren_fetch_source.py --dest build/senren-source --chunks
  python tools/senren_fetch_source.py --dest /tmp/senren-source --ref <commit>

Existing files with the expected size are skipped, so re-running is cheap.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import hashlib
import json
import sys
import urllib.error
import urllib.request
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable

REPO = "hrk666666/Senren-Banka-MiBand-10"
DATA_API = "https://data.jsdelivr.com/v1/packages/gh/{repo}@{ref}?structure=flat"
MIRRORS = (
    "https://cdn.jsdelivr.net/gh/{repo}@{ref}/{path}",
    "https://raw.githack.com/{repo}/{ref}/{path}",
)
ASSET_ROOTS = ("src/common/bg/", "src/common/ch/", "src/common/ev/")
SCRIPT_ROOT = "src/common/scn/"
MANIFEST = "MANIFEST.json"


@dataclass(frozen=True)
class RemoteFile:
    path: str          # 相对仓库根,如 src/common/bg/山_山道a.jpg
    rel: str           # 落盘相对路径,如 bg/山_山道a.jpg
    size: int


def log(msg: str) -> None:
    print(msg, file=sys.stderr)


def http_get(url: str, timeout: float = 60.0) -> bytes:
    request = urllib.request.Request(url, headers={"User-Agent": "senren-fetch-source"})
    with urllib.request.urlopen(request, timeout=timeout) as response:
        return response.read()


def list_files(ref: str) -> tuple[str, list[RemoteFile]]:
    """Return the resolved version and every asset/script path we care about."""
    payload = json.loads(http_get(DATA_API.format(repo=REPO, ref=ref)))
    version = str(payload.get("version", ref))
    files: list[RemoteFile] = []
    for entry in payload.get("files", []):
        name = str(entry.get("name", "")).lstrip("/")
        size = int(entry.get("size", 0))
        if name.startswith(ASSET_ROOTS):
            files.append(RemoteFile(name, name[len("src/common/"):], size))
        elif name.startswith(SCRIPT_ROOT):
            files.append(RemoteFile(name, name[len("src/common/"):], size))
    if not files:
        sys.exit(f"ERROR: {REPO}@{ref} 没有列出任何素材,jsDelivr 目录 API 返回了 {len(payload.get('files', []))} 项")
    return version, files


def quoted(path: str) -> str:
    # 文件名含中文与全角括号,逐段编码后仍保持 '/' 分隔
    from urllib.parse import quote

    return "/".join(quote(part) for part in path.split("/"))


def download(remote: RemoteFile, ref: str, timeout: float = 90.0) -> bytes:
    last: Exception | None = None
    for template in MIRRORS:
        url = template.format(repo=REPO, ref=ref, path=quoted(remote.path))
        try:
            return http_get(url, timeout=timeout)
        except (urllib.error.URLError, TimeoutError, OSError) as exc:  # 换镜像重试
            last = exc
    raise RuntimeError(f"{remote.path}: {last}")


def fetch(remote: RemoteFile, dest: Path, ref: str) -> tuple[RemoteFile, bool, str]:
    target = dest / remote.rel
    if target.is_file() and target.stat().st_size == remote.size:
        return remote, False, hashlib.sha256(target.read_bytes()).hexdigest()
    data = download(remote, ref)
    if len(data) != remote.size:
        raise RuntimeError(f"{remote.path}: 字节数不符 (期望 {remote.size}, 收到 {len(data)})")
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(data)
    return remote, True, hashlib.sha256(data).hexdigest()


def human(count: int) -> str:
    if count >= 1048576:
        return f"{count / 1048576:.2f} MB"
    return f"{count / 1024:.1f} KB"


def run(args: argparse.Namespace) -> int:
    ref = args.ref
    log(f"列出 {REPO}@{ref} 的文件…")
    version, files = list_files(ref)
    assets = [f for f in files if f.path.startswith(ASSET_ROOTS)]
    scripts = [f for f in files if f.path.startswith(SCRIPT_ROOT)]
    if not args.chunks:
        scripts = []
    wanted = assets + scripts
    if args.limit_images and args.limit_images < len(assets):
        # 只做冒烟测试用:按路径排序取前 N 个图
        assets = sorted(assets, key=lambda f: f.rel)[: args.limit_images]
        wanted = assets + scripts
    if not wanted:
        sys.exit("ERROR: 没有要下载的文件")

    dest = Path(args.dest)
    dest.mkdir(parents=True, exist_ok=True)
    log(f"下载 {len(wanted)} 个文件 ({human(sum(f.size for f in wanted))}) → {dest}")

    entries: dict[str, dict[str, object]] = {}
    downloaded = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        futures = [pool.submit(fetch, remote, dest, version) for remote in wanted]
        for index, future in enumerate(concurrent.futures.as_completed(futures), start=1):
            remote, fresh, digest = future.result()
            entries[remote.rel] = {"path": remote.path, "bytes": remote.size, "sha256": digest}
            downloaded += 1 if fresh else 0
            if index % 100 == 0 or index == len(wanted):
                log(f"  {index}/{len(wanted)}  (新增 {downloaded})")

    manifest = dest / MANIFEST
    previous = {}
    if manifest.is_file():
        try:
            previous = json.loads(manifest.read_text(encoding="utf-8")).get("files", {})
        except (json.JSONDecodeError, OSError):
            previous = {}
    merged = {**previous, **entries}
    manifest.write_text(
        json.dumps(
            {
                "repo": REPO,
                "ref": version,
                "requested_ref": ref,
                "files": dict(sorted(merged.items())),
            },
            ensure_ascii=False,
            indent=1,
        )
        + "\n",
        encoding="utf-8",
    )
    total = sum(int(item["bytes"]) for item in entries.values())
    log(f"完成: {len(entries)} 个文件,{human(total)},新增 {downloaded};清单 {manifest}")
    return 0


def main(argv: Iterable[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--dest", default="build/senren-source", help="落地目录(默认 build/senren-source)")
    parser.add_argument("--ref", default="main", help="上游分支/标签/commit(默认 main)")
    parser.add_argument("--chunks", action="store_true", help="同时拉 112 个剧本块(打包脚本时需要)")
    parser.add_argument("--jobs", type=int, default=12, help="并发数(默认 12)")
    parser.add_argument("--limit-images", type=int, default=0, help="只拉前 N 张图,冒烟测试用")
    return run(parser.parse_args(argv))


if __name__ == "__main__":
    raise SystemExit(main())
