#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
生成生字卡片数据表 C 文件 (main/gen/sz_cards_table.c)。

从 shengzi-cards/shengsheng.txt 读 212 个字, 用 pypinyin 生成带声调拼音,
叠加内置笔画数表, 输出 C 数组 sz_cards[]。

用法: python tools/sz_gen/gen_table.py
"""
import os
import sys
from pypinyin import pinyin, Style

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CHAR_FILE = os.path.join(ROOT, "shengzi-cards", "shengsheng.txt")
OUT_FILE = os.path.join(ROOT, "main", "gen", "sz_cards_table.c")

# 内置笔画数表: 一年级上常见字, 缺失的用 0 表示"未知"
STROKES = {
    "天":4,"地":6,"人":2,"你":7,"我":7,"他":5,"一":1,"二":2,"三":3,"四":5,"五":4,
    "上":3,"下":3,"口":3,"耳":6,"目":5,"手":4,"足":7,"站":10,"坐":7,"日":4,
    "月":4,"山":3,"川":3,"水":4,"火":4,"田":5,"禾":5,"六":4,"七":2,"八":2,
    "九":2,"十":2,"爸":8,"妈":6,"大":3,"马":3,"路":13,"土":3,"本":5,
    "学":8,"校":10,"班":10,"级":6,"姓":8,"名":6,"王":4,"哥":10,"弟":7,"画":8,
    "花":7,"打":5,"棋":12,"积":10,"木":4,"字":6,"词":7,"句":5,"子":3,"桌":10,
    "纸":7,"读":10,"书":4,"鱼":8,"鸭":10,"乌":4,"鸦":9,"午":4,"星":9,"期":12,
    "语":9,"文":4,"数":13,"写":5,"会":6,"白":5,"菜":11,"西":6,"瓜":5,"果":8,
    "小":3,"桥":10,"流":10,"柳":9,"开":4,"雪":11,"夜":8,"色":6,"美":9,"蓝":13,
    "云":4,"草":9,"原":10,"冰":6,"自":6,"行":6,"车":4,"晚":11,"昨":9,"今":4,
    "明":8,"个":3,"这":7,"去":5,"年":6,"秋":9,"气":4,"了":2,"树":9,"叶":5,
    "黄":11,"片":4,"从":4,"来":7,"飞":3,"江":6,"南":9,"可":5,"采":8,"莲":10,
    "戏":6,"间":7,"东":5,"北":5,"的":8,"家":10,"鸡":7,"竹":6,"牙":4,"用":5,
    "几":2,"步":7,"没":7,"参":8,"加":5,"鸟":5,"说":9,"是":9,"春":9,"青":8,
    "蛙":12,"夏":10,"着":11,"皮":5,"就":12,"冬":5,"男":7,"女":3,"关":6,"正":5,
    "反":4,"先":6,"后":6,"内":4,"外":5,"对":5,"歌":14,"雨":8,"风":4,"虫":6,
    "清":11,"绿":11,"桃":10,"红":6,"力":2,"尖":6,"尘":6,"众":6,"双":4,"林":8,
    "森":12,"不":4,"条":7,"心":4,"金":8,"包":5,"尺":4,"作":7,"业":5,"笔":10,
    "刀":2,"宝":8,"贝":4,"少":4,"课":10,"早":6,"升":4,"国":8,"旗":14,"中":4,
    "们":5,"声":7,"起":10,"多":6,"么":3,"向":6,"立":5,"老":6,"师":6,"工":3,
    "厂":2,"医":7,"院":9,"生":5,"门":3,"卫":3,"船":11,"弯":9,"儿":2,"两":7,
    "头":5,"在":6,"田":5,
}

# 多音字校正: 按一年级上语境定音 (字 -> 应读拼音)
# 这里用 pypinyin 默认读音, 但多音字按常见一年级语境覆盖
PINYIN_OVERRIDE = {
    "着": "zhe",       # "看着"轻声, 此处用轻声zhe
    "了": "le",        # 完"了"
    "行": "xíng",      # "自行车" xíng
    "乐": "lè",
    "数": "shù",       # "数学"
    "只": "zhī",       # "一只"
    "地": "dì",        # "天地"
    "种": "zhǒng",     # "种子" (若在表内)
    "发": "fā",
    "长": "cháng",     # "长江" / "长短"
    "觉": "jué",
    "空": "kōng",
    "为": "wèi",       # "为什么"
    "那": "nà",
    "得": "de",
    "结": "jié",
}

def main():
    with open(CHAR_FILE, "r", encoding="utf-8") as f:
        chars = []
        for line in f:
            if line.startswith("#"):
                continue
            for ch in line.strip():
                if ch and ch != " ":
                    chars.append(ch)

    # 去重保序
    seen = set()
    ordered = []
    for ch in chars:
        if ch not in seen:
            seen.add(ch)
            ordered.append(ch)

    cards = []
    for ch in ordered:
        # pinyin 带声调, 如 tiān
        py = pinyin(ch, style=Style.TONE, heteronym=False)
        py_str = py[0][0] if py else ""
        if ch in PINYIN_OVERRIDE:
            py_str = PINYIN_OVERRIDE[ch]
        strokes = STROKES.get(ch, 0)
        cards.append((ch, py_str, strokes))

    os.makedirs(os.path.dirname(OUT_FILE), exist_ok=True)
    with open(OUT_FILE, "w", encoding="utf-8") as f:
        f.write("// 自动生成: tools/sz_gen/gen_table.py — 请勿手改\n")
        f.write("// 数据源: shengzi-cards/shengsheng.txt (%d 字)\n\n" % len(cards))
        f.write('#include <stdint.h>\n')
        f.write('#include "sz_data.h"\n\n')
        f.write("const sz_card_t sz_cards[] = {\n")
        for ch, py, st in cards:
            # UTF-8 汉字
            f.write('    {"%s", "%s", %d},\n' % (ch, py, st))
        f.write("};\n")
        f.write("const uint16_t sz_card_count = %d;\n" % len(cards))

    # 打印摘要
    print("生成 %d 张字卡 -> %s" % (len(cards), OUT_FILE))
    missing_py = [c for c, p, s in cards if not p]
    missing_st = [c for c, p, s in cards if s == 0]
    if missing_py:
        print("警告: 无拼音字:", missing_py)
    if missing_st:
        print("提示: 笔画未知字(%d): %s" % (len(missing_st), "".join(missing_st)))

if __name__ == "__main__":
    main()
