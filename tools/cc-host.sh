#!/usr/bin/env bash
# Host C compiler wrapper for this machine (no system cc): Zig's clang.
exec python -m ziglang cc "$@"
