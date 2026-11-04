#!/usr/bin/env bash
# 宿主机 C 编译器的兜底包装:Zig 自带 clang,不依赖系统安装 MSVC/MinGW。
# 用于没有 cc/gcc/clang 的开发机(例如只装了 ESP-IDF + Python 的 Windows)。
#
#   pip install ziglang            # 一次性
#   CC=tools/cc-zig.sh ./tools/validate.sh --static
#
# validate.sh 在找不到 cc/gcc/clang 时会自动回退到本脚本;找不到 ziglang 才报 SKIP。
set -euo pipefail
target="x86_64-windows-gnu"
case "$(uname -s)" in
    Linux) target="x86_64-linux-gnu" ;;
    Darwin) target="x86_64-macos-none" ;;
esac
exec python3 -m ziglang cc -target "${target}" "$@"
