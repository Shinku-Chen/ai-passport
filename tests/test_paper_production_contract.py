#!/usr/bin/env python3
"""契约测试：扫描 paper 生产源码，锁住几条硬约束（不模拟逻辑，只防回退）。

1. 生产代码不得包含 nvs_flash_erase（启动时自动擦除会毁掉用户数据）
2. 自动存档必须走 PAPER_SLOT_AUTO（不能写死 0）
3. 新游戏必须走 paper_game_new（入口取自包内 flowScene，不能写死场景号）
4. 长按/松手必须使用 BSP 枚举，不得再出现裸数字 1/3 判定
"""
import io
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC_DIRS = [os.path.join(ROOT, "main")]
errors = []


def files():
    for d in SRC_DIRS:
        for dp, dn, fn in os.walk(d):
            for f in fn:
                if f.endswith(".c") or f.endswith(".h"):
                    yield os.path.join(dp, f)


for path in files():
    rel = os.path.relpath(path, ROOT)
    text = io.open(path, encoding="utf-8", errors="ignore").read()
    if "nvs_flash_erase" in text:
        errors.append(f"{rel}: 出现 nvs_flash_erase（禁止启动自动擦除 NVS）")

app = io.open(os.path.join(ROOT, "main/gal/gal_app.c"), encoding="utf-8").read()
if "paper_game_save(&app->game, PAPER_SLOT_AUTO)" not in app:
    errors.append("gal_app.c: 自动存档未使用 PAPER_SLOT_AUTO")
if "paper_game_save(&app->game, 0)" in app:
    errors.append("gal_app.c: 仍有写死槽 0 的存档调用")
if "paper_game_new(&app->game)" not in app:
    errors.append("gal_app.c: 新游戏未使用 paper_game_new（入口应取自包内 flowScene）")
if "paper_game_start(&app->game, 1)" in app:
    errors.append("gal_app.c: 仍有写死 scene 1 的新游戏入口")
if "key->ev == 3" in app or "key->btn == 2" in app:
    errors.append("gal_app.c: 按键判定仍有魔法数")

if errors:
    print("FAIL 生产契约:")
    for e in errors:
        print("  -", e)
    sys.exit(1)
print(f"PASS 生产契约: {sum(1 for _ in files())} 个源文件扫描通过")
