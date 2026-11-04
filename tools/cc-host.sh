#!/usr/bin/env bash
# Host C compiler wrapper used by the maintainer gate.
#
# This machine has no system `cc`, so the bundled Zig clang is used:
#   CC="$(pwd)/tools/cc-host.sh" ./tools/validate.sh --static
# On a machine with a normal compiler you can just use `CC=cc`.
exec python -m ziglang cc "$@"
