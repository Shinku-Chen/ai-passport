#!/usr/bin/env python3
"""limelight 剧本打包器的"正文清洗"规则测试。

源数据(手环移植版)里混进了两种非剧情内容,真机上都表现为"剧情不对":
  1. 排版指令 %f … %r / $名字$ / #rrggbb[aa]  —— 当普通文字显示就是满屏乱码
  2. 译者备注 memo: …(7 条,最长 740 字节 = 6 屏)—— 表现为"对话框被文字填满"
本测试锁定这两条的清除规则,并保证普通对话一字不改。
"""

import sys
from pathlib import Path


def _safe(text: str) -> str:
    """Windows 控制台是 GBK,打印 ♥ 之类会炸;统一转成可打印形式。"""
    return text.encode("unicode_escape").decode("ascii")

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
from limelight_script_pack import clean_text  # noqa: E402

# (原文, 期望正文, 期望命中标签)
CASES = [
    # 排版指令:没有收尾 $ 的写法(id 6542)
    ("「像，这样吗……？嗯哼～……%f$ハート♥」", "「像，这样吗……？嗯哼～……♥」", "directive"),
    # 排版指令:成对写法(104 条里的绝大多数)
    ("「哦？怎么了～%f$ハート$#00ffadd6♥%r」", "「哦？怎么了～♥」", "directive"),
    # 同一句里出现两次指令
    ("「嗯嗯%f$ハート$#00ffadd6♥%r嘞恰，嘞呖%f$ハート$#00ffadd6♥%r」",
     "「嗯嗯♥嘞恰，嘞呖♥」", "directive"),
    # 指令后面还有正文
    ("「哈啊……哈啊……好厉害%f$ハート$#00ffadd6♥%r\n出来了」",
     "「哈啊……哈啊……好厉害♥\n出来了」", "directive"),
    # 译者备注:冒号写法
    ("只有我制作的部分。，\nmemo:\nThe original text means…", "只有我制作的部分。，", "memo"),
    # 译者备注:引号夹冒号的三种变体(35630/35632/35635)
    ("「嗯……嗯……memo「:」The original onomatopoeia", "「嗯……嗯……", "memo"),
    ("「那优花调整过的音源memo“:”The original …", "「那优花调整过的音源", "memo"),
    ("喂。memo : The original", "喂。", "memo"),
    # 两者同时出现(memo 之后的部分整段丢弃,所以只命中 memo)
    ("「嗯……memo:\nThe original …%f$ハート$#00ffadd6♥%r", "「嗯……", "memo"),
    # 无收尾写法只吃假名/汉字/字母组成的名字(工程里只有 $ハート)
    ("「不要……$ハート♥」", "「不要……♥」", "directive"),
    # 普通对话必须一字不改
    ("「是、是这样吗……？」", "「是、是这样吗……？」", ""),
    ("第1章 放学后的吉他", "第1章 放学后的吉他", ""),
    # 单独的 % 或 $ 不是指令,不能吃掉
    ("打折 50% 的谱子", "打折 50% 的谱子", ""),
    ("价格 $5 的琴弦", "价格 $5 的琴弦", ""),
    ("第1章 #标签", "第1章 #标签", ""),
]


def main() -> int:
    failures = 0
    for i, (raw, want, want_hit) in enumerate(CASES):
        got, hit = clean_text(raw)
        if got != want or hit != want_hit:
            failures += 1
            print(f"FAIL case {i}: {_safe(raw)} / got {_safe(got)} ({hit}) / want {_safe(want)} ({want_hit})")
    print(f"limelight script clean: checks={len(CASES)} failures={failures}")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
