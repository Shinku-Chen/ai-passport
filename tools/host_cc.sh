#!/usr/bin/env bash
# Reproducible host C compiler wrapper for the CLANNAD gate.
# Prefer a system cc; otherwise use the project's documented Zig fallback.
set -euo pipefail

if command -v cc >/dev/null 2>&1; then
    exec cc "$@"
fi
if python3 -c 'import ziglang' >/dev/null 2>&1; then
    exec python3 -m ziglang cc "$@"
fi
echo "host_cc.sh: neither cc nor python -m ziglang is available" >&2
exit 127
