#!/usr/bin/env python3
"""Fetch the Sanoba Witch (MiBand 10 port) source material for pack builds.

Source project: https://github.com/hrk666666/Sanoba-Witch-MiBand-10
  - 小米手环 9 / 10(Xiaomi Vela / aiot 快应用)上的《魔女的夜宴》移植版,基于
    https://github.com/futrw4v/Sanoba-Witch-MiBand-9Pro(已存档)。
  - 剧本、背景、SD 装饰图版权归 Yuzusoft 所有,汉化文本版权归暗鸽汉化组;
    本仓库只保存转换产物(main/sanoba_data/),不再分发源素材 —— 重建 pack 时
    用本工具按需拉取。

The upstream repository is about 9.6 MB of third-party art and script, so this
tool pulls it from upstream on demand.  The file list comes from the jsDelivr
data API and the payloads from the jsDelivr CDN, with raw.githack.com as a
fallback, because a direct github.com clone is not reachable from every build
host.

UPSTREAM STATUS (checked 2026-10-03).  The repository's history was rewritten on
2026-09-26: `main` is now another port (Senren * Banka, with its own `ch/` and
`ev/` art) and the Sanoba Witch material no longer exists on it.  What is left
upstream is the `band9-10-sync` branch, a v1.2.1 sync point from 2026-09-18 that
serves `bg/`, `sd/`, `scn/` and `title_bg.jpg` byte-for-byte identically to this
archive, but has no `game.txt`, no `images/` and no `logo.png` (it also lacks
nothing that is not matched here).  Therefore:

  - `assets/sanoba-source/` is the authoritative copy.  A plain checkout rebuilds
    both packs offline; this tool is only needed to follow upstream, and the
    default ref is pinned to `band9-10-sync` instead of the repurposed `main`.
  - Files this ref cannot serve are taken from the archive, not re-fetched.

The port uses only the game's own material, never its quick-app code:

  <dest>/bg/*.jpg       107 背景(336x480 JPEG)
  <dest>/sd/*.jpg       292 SD 装饰图(240x144 JPEG,源工程 .sd-image 的显示框)
  <dest>/images/*.png    29 源工程的图标/装饰图(游戏素材,阅读器不使用)
  <dest>/title_bg.jpg     1 标题主视觉(336x480 JPEG)
  <dest>/logo.png         1 源工程 logo(游戏素材,阅读器不使用)
  <dest>/game.txt         内容包清单:场景顺序(101 章的分组与次序)与 019 的选线规则
  <dest>/scn/*.txt      101 剧本(--chunks,打包剧本时需要)
  <dest>/MANIFEST.json  每个文件的上游路径、字节数与 sha256,便于复现校验

Alongside those it keeps the upstream repository's own reference material, so the
material stays self-describing offline:

  <dest>/upstream-screenshots/*.png   源工程 README 用的 4 张截图
  <dest>/script-format-spec-v1.1.txt   上游的剧本格式规范(原文件名 剧本格式规范v1.1.md;本仓库打包器实现的即此格式)
  <dest>/LICENSE                       上游 GPL-3.0 许可证正本(素材与其文档随此许可分发)

src/**/*.ux、src/**/*.js、tests/、tools/ 等快应用代码不在此列。

Upstream carries no `ev/` (event CG) and no `ch/` (standing art) directory: the
script references 2,862 `ev*` and 19,224 standing-art entries that have no
image.  Those references are kept as MISSING placeholders by the packers, so the
reader skips them instead of failing; see docs/reference/shinku-chen/sanoba-witch/.

Usage:
  python tools/sanoba_fetch_source.py
  python tools/sanoba_fetch_source.py --dest build/sanoba-source --chunks
  python tools/sanoba_fetch_source.py --dest /tmp/sanoba-source --ref <commit>

Existing files with the expected size are skipped, so re-running is cheap.

本分支已把素材入库到 assets/sanoba-source/(来源与许可见 assets/README.md),
所以重建资源包不需要联网,直接 --source assets/sanoba-source 即可。只有要跟上游
最新版本时才需要跑本工具;注意上游 main 已被换成另一个移植(千恋＊万花),
Sanoba 素材只在 band9-10-sync 分支上,默认 ref 已改指该分支。
"""

from __future__ import annotations

import argparse
import concurrent.futures
import hashlib
import json
import sys
import time
import urllib.error
import urllib.request
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable
from urllib.parse import quote

REPO = "hrk666666/Sanoba-Witch-MiBand-10"
# 上游 main 在 2026-09-26 被重写成另一个移植(千恋＊万花),Sanoba 素材只剩这条分支
DEFAULT_REF = "band9-10-sync"
ARCHIVE_ONLY = ("src/common/game.txt", "src/common/images/", "src/common/logo.png")
DATA_API = "https://data.jsdelivr.com/v1/packages/gh/{repo}@{ref}?structure=flat"
MIRRORS = (
    "https://cdn.jsdelivr.net/gh/{repo}@{ref}/{path}",
    "https://raw.githack.com/{repo}/{ref}/{path}",
)
ASSET_ROOTS = ("src/common/bg/", "src/common/sd/", "src/common/images/")
EXTRA_FILES = ("src/common/title_bg.jpg", "src/common/game.txt", "src/common/logo.png")
SCRIPT_ROOT = "src/common/scn/"
# 上游随仓库一起提供的参考材料:重定向前缀,以及逐个文件的重命名
REFERENCE_ROOTS = (("screenshots/", "upstream-screenshots/"),)
REFERENCE_FILES = (("docs/剧本格式规范v1.1.md", "script-format-spec-v1.1.txt"), ("LICENSE", "LICENSE"))
MANIFEST = "MANIFEST.json"
COMMON_PREFIX = "src/common/"


@dataclass(frozen=True)
class RemoteFile:
    path: str          # 相对仓库根,如 src/common/bg/空_青空.jpg
    rel: str           # 落盘相对路径,如 bg/空_青空.jpg
    size: int
    kind: str = "asset"  # asset(打包输入)/script(剧本)/reference(上游参考材料)


def log(msg: str) -> None:
    print(msg, file=sys.stderr)


def http_get(url: str, timeout: float = 60.0) -> bytes:
    request = urllib.request.Request(url, headers={"User-Agent": "sanoba-fetch-source"})
    with urllib.request.urlopen(request, timeout=timeout) as response:
        return response.read()


def retry(fn, what: str, attempts: int = 5, delay: float = 3.0):
    """构建机到 jsDelivr 的链路会间歇性握手超时,逐个退避重试。"""
    last: Exception | None = None
    for attempt in range(1, attempts + 1):
        try:
            return fn()
        except Exception as exc:  # noqa: BLE001 - 网络异常类型很多,统一退避重试
            last = exc
            if attempt < attempts:
                log(f"  {what} 第 {attempt}/{attempts} 次失败({exc}),{delay * attempt:.0f}s 后重试")
                time.sleep(delay * attempt)
    raise RuntimeError(f"{what}: {last}")


def list_files(ref: str) -> tuple[str, list[RemoteFile]]:
    """Return the resolved version and every asset/script path we care about."""
    payload = json.loads(retry(lambda: http_get(DATA_API.format(repo=REPO, ref=ref)), "目录 API"))
    version = str(payload.get("version", ref))
    files: list[RemoteFile] = []
    for entry in payload.get("files", []):
        name = str(entry.get("name", "")).lstrip("/")
        size = int(entry.get("size", 0))
        if name.startswith(ASSET_ROOTS):
            files.append(RemoteFile(name, name[len(COMMON_PREFIX):], size))
        elif name in EXTRA_FILES:
            files.append(RemoteFile(name, name[len(COMMON_PREFIX):], size))
        elif name.startswith(SCRIPT_ROOT):
            files.append(RemoteFile(name, name[len(COMMON_PREFIX):], size, "script"))
        else:
            for prefix, dest_prefix in REFERENCE_ROOTS:
                if name.startswith(prefix):
                    files.append(RemoteFile(name, dest_prefix + name[len(prefix):], size, "reference"))
                    break
            else:
                for path, rel in REFERENCE_FILES:
                    if name == path:
                        files.append(RemoteFile(name, rel, size, "reference"))
                        break
    if not files:
        sys.exit(f"ERROR: {REPO}@{ref} 没有列出任何素材,jsDelivr 目录 API 返回了 {len(payload.get('files', []))} 项")
    return version, files


def quoted(path: str) -> str:
    # 文件名含中文与全角括号,逐段编码后仍保持 '/' 分隔
    return "/".join(quote(part) for part in path.split("/"))


def download(remote: RemoteFile, ref: str, timeout: float = 90.0) -> bytes:
    last: Exception | None = None
    for template in MIRRORS:
        url = template.format(repo=REPO, ref=ref, path=quoted(remote.path))
        try:
            return retry(lambda: http_get(url, timeout=timeout), remote.path, attempts=3, delay=2.0)
        except (urllib.error.URLError, TimeoutError, OSError, RuntimeError) as exc:  # 换镜像重试
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


def resolve_commit(ref: str) -> str | None:
    """把分支解析成提交 sha,便于把快照钉在不可变版本上;失败不影响下载。"""
    url = f"https://api.github.com/repos/{REPO}/commits/{ref}"
    try:
        payload = json.loads(retry(lambda: http_get(url, timeout=30.0), f"解析 {ref}", attempts=3, delay=2.0))
        return str(payload.get("sha", "")) or None
    except Exception as exc:  # noqa: BLE001 - 拿不到 sha 不该让整个抓取失败
        log(f"  无法解析 {ref} 的提交 sha({exc}),MANIFEST 里留空")
        return None


def human(count: int) -> str:
    if count >= 1048576:
        return f"{count / 1048576:.2f} MB"
    return f"{count / 1024:.1f} KB"


def run(args: argparse.Namespace) -> int:
    ref = args.ref
    if ref in ("main", "master"):
        log(f"警告: {REPO}@{ref} 自 2026-09-26 起是另一个移植的工程,不含 Sanoba 素材;"
            f"默认请用 {DEFAULT_REF}(或直接不指定 --ref)。")
    log(f"列出 {REPO}@{ref} 的文件…")
    version, files = list_files(ref)
    assets = [f for f in files if f.kind == "asset"]
    references = [f for f in files if f.kind == "reference"]
    scripts = [f for f in files if f.kind == "script"] if args.chunks else []
    wanted = assets + references + scripts
    if args.limit_images and args.limit_images < len(assets):
        # 只做冒烟测试用:按路径排序取前 N 个图
        assets = sorted(assets, key=lambda f: f.rel)[: args.limit_images]
        wanted = assets + references + scripts
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
    missing_archive_only = [
        path for path in ARCHIVE_ONLY
        if not any(str(item["path"]).startswith(path) for item in merged.values())
    ]
    if missing_archive_only:
        log("提示: 该 ref 不提供以下文件,使用 assets/sanoba-source/ 里的归档副本:"
            f" {', '.join(missing_archive_only)}")
    manifest.write_text(
        json.dumps(
            {
                "repo": REPO,
                "ref": ref if ref != version else version,
                "requested_ref": ref,
                "resolved_commit": resolve_commit(ref) or "",
                "archive_only": list(ARCHIVE_ONLY),
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
    parser.add_argument("--dest", default="build/sanoba-source", help="落地目录(默认 build/sanoba-source)")
    parser.add_argument("--ref", default=DEFAULT_REF, help=f"上游分支/标签/commit(默认 {DEFAULT_REF})")
    parser.add_argument("--chunks", action="store_true", help="同时拉 101 个剧本文件(打包剧本时需要)")
    parser.add_argument("--jobs", type=int, default=12, help="并发数(默认 12)")
    parser.add_argument("--limit-images", type=int, default=0, help="只拉前 N 张图,冒烟测试用")
    return run(parser.parse_args(argv))


if __name__ == "__main__":
    raise SystemExit(main())
