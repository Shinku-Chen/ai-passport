#!/usr/bin/env bash
# Host C compiler for the repository gates on Windows/Git Bash.
# Preference: an explicit SUMMER_HOST_CC, then zig's bundled clang, then cc.
set -uo pipefail
if [[ -n "${SUMMER_HOST_CC:-}" ]]; then
    exec ${SUMMER_HOST_CC} "$@"
fi
if python3 -m ziglang cc --version >/dev/null 2>&1; then
    exec python3 -m ziglang cc "$@"
fi
exec cc "$@"
