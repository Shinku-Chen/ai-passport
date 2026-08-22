# 生字卡片识记功能

为 FoloToy AI Passport (ESP32-C3) 新增的"生字卡片识记"演示页，作为 main 菜单的第 5 个 demo。

- 数据源：《同步生字大卡 一年级上 2025.8》（书链平台，书ID 565717）
- 字表：212 个不重复生字，见 `shengsheng.txt`
- 拼音/笔画：由 `tools/sz_gen/gen_table.py` 用 pypinyin 离线生成（多音字按一年级语境校正），固化成 `main/gen/sz_cards_table.c`
- 中文字体：`main/fonts/sz_big.c`（212 汉字 LVGL 字体）+ `sz_small.c`（拼音声调字母），由 lv_font_conv 生成

## 生成数据

```
python tools/sz_gen/gen_table.py
```

## 备注

- 字表为平台页面目录抓取（212 字）。每字的拼音、读音需平台购买/微信端授权；当前离线版只显示汉字+拼音+笔画。
