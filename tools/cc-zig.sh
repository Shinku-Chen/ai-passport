#!/usr/bin/env bash
# 宿主机 C 编译器的兜底包装：Zig 自带 clang，不依赖系统安装 MSVC/MinGW。
# 用于没有 cc/gcc/clang 的开发机（例如只装了 ESP-IDF + Python 的 Windows）：
#   pip install ziglang
#   CC=tools/cc-zig.sh ./tools/validate.sh --static
set -euo pipefail
target="x86_64-windows-gnu"
case "$(uname -s)" in
    Linux) target="x86_64-linux-gnu" ;;
    Darwin) target="x86_64-macos-none" ;;
esac
exec python -m ziglang cc -target "${target}" "$@"
