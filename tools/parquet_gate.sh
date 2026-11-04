#!/usr/bin/env bash
# 《PARQUET》一键 gate：fixtures → 真实 C 回归（资源/剧情/存档/UI/图像）→
# 负例编译门禁 → 独立源审计 → 包校验 → 字体覆盖 → 文档检查。
# 不依赖 build/ 里的旧脚本/旧报告；编译器可用 ZIG_CC / zig / python -m ziglang cc / CC。
# 每次运行使用唯一输出目录（build/host-<UTC>-$$）与唯一日志/报告，不覆盖既有通过证据；
# 显式设置 OUT_DIR / FIXTURES_DIR / PARQUET_GATE_LOG / PARQUET_REPORT_DIR 时尊重调用方。
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.."

STAMP="$(date -u +%Y%m%dT%H%M%SZ)-$$"
OUT_DIR="${OUT_DIR:-build/host-${STAMP}}"
FIXTURES_DIR="${FIXTURES_DIR:-${OUT_DIR}/fixtures}"
LOG="${PARQUET_GATE_LOG:-build/logs/parquet-gate-${STAMP}.log}"
REPORT_DIR="${PARQUET_REPORT_DIR:-build/reports/${STAMP}}"
mkdir -p "$OUT_DIR" "$FIXTURES_DIR" "$(dirname "$LOG")" "$REPORT_DIR"

{
  echo "== parquet_gate $(date -Iseconds) =="
  echo "out: $(cd -- "$OUT_DIR" && pwd)"
  echo "fixtures: $(cd -- "$FIXTURES_DIR" && pwd)"
  echo "reports: $(cd -- "$REPORT_DIR" && pwd)"
  echo "-- 1/6 生成 C fixture（独立参考解码：BLOCK/PATCH 逐像素、JPEG 网格）"
  python3 tools/parquet_verify.py --fixtures "$FIXTURES_DIR" \
      --report "${REPORT_DIR}/parquet-verify.json"
  test -s "${FIXTURES_DIR}/images.txt"
  echo "-- 2/6 真实 C 回归（资源/剧情/存档/路由/图像 + 生产 UI 按键序列）"
  OUT_DIR="$OUT_DIR" FIXTURES_DIR="$FIXTURES_DIR" \
      bash tools/tests/parquet_host_build.sh
  echo "-- 3/6 门禁负例（图片测试编译失败必须非零退出）"
  OUT_DIR="$OUT_DIR" bash tools/tests/parquet_negative_gate.sh
  echo "-- 4/6 独立源审计（源快照 ↔ 包逐 op 对照）"
  python3 tools/parquet_audit.py \
      --report "${REPORT_DIR}/parquet-audit.json" \
      --markdown "${REPORT_DIR}/parquet-audit.md"
  echo "-- 5/6 字库覆盖（2589 码位，16/20px）"
  python3 tools/gal_font.py --check --out-dir assets/fonts --sizes 16,20
  echo "-- 6/6 仓库文档检查"
  python3 tools/check_repo.py
  echo "parquet_gate: PASS | out=$(cd -- "$OUT_DIR" && pwd) | log=$(cd -- "$(dirname "$LOG")" && pwd)/$(basename "$LOG")"
} 2>&1 | tee "$LOG"
