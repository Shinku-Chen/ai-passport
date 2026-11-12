#!/usr/bin/env bash
# 《纸上的魔法使》一键 gate：fixture 确定性生成 → 真实 C 回归 → 契约 → 包校验 → 文档。
# 不依赖 gitignored 的 build 脚本/产物；编译器可用 ZIG_CC / zig / python -m ziglang cc / CC。
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.."
mkdir -p build/logs tests/paper/fixtures
LOG=build/logs/paper-gate.log

{
  echo "== paper_gate $(date -Iseconds) =="
  echo "-- 1/5 生成测试 fixture（从已入库的资源包）"
  python3 tools/paper_verify.py          # 写 tests/paper/fixtures/unreachable.bin
  echo "-- 2/5 真实 C 回归（资源/解码/立绘几何/存档/路线/UI）"
  OUT_DIR="${OUT_DIR:-build/host}" bash tools/tests/paper_host_build.sh
  # patch 期望像素由打包器生成；若缺失则明确失败（不静默跳过）
  test -s tests/paper/fixtures/patch-expect.bin
  echo "-- 3/5 生产契约扫描"
  python3 tests/test_paper_production_contract.py
  echo "-- 4/5 包完整性（含 flowScene）"
  python3 tools/paper_verify.py
  echo "-- 5/5 仓库文档检查"
  python3 tools/check_repo.py
  echo "paper_gate: PASS"
} 2>&1 | tee "$LOG"
