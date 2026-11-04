<p align="right"><a href="README.md">English</a> · <strong>简体中文</strong></p>

# AI Passport 《命运石之门》阅读器

从「小米手表 5」场景阅读器发布 `steins-rw5`(源快照
`28e5b52f11c77fc4781f532e2729a5374f5fd8ea`)移植到 AI Passport 的横屏视觉小说
阅读器。应用基线为 `feature/galgame-framework` @
`3c2e5013a9831c57124ef8f774193f0e06f1f14a`。

以下内容仅描述**本工作树**(`ai-passport-steins`,分支
`feature/port-steins-landscape`)。首批移植工程与主检出于本分支只作只读参考。

## 交付状态

**软件审核通过,待设备验收。**

| 项目 | 数值 |
| --- | --- |
| App 镜像 | **7,972,400 B**,位于 `0x30000`;factory `0x7d0000` = 8,192,000 B,**余量 219,600 B** |
| App SHA-256 | `a91cb88527ae1e3450208942ab08e3ceb1bf42f1bc85c3d4d4a0e9299e96437d` |
| 合并镜像 | **8,169,008 B** |
| 合并 SHA-256 | `2020c42f4bdd2030ae417ed8a4db13a7e0cf16376a1d10259c00907e09d34ab5` |
| ELF | 17,986,512 B,SHA-256 `8cff5469e883a3d0d1752ca5e6f8c6e657f3f15f08aa41757cb866bb5044b579` |
| MAP | 4,300,870 B,SHA-256 `bb7ff80ca52928816bcf1907e970fa25b257d9fa0338526a5d56a5bf996b6559` |
| 同次归档 | `build/firmware/2020c42f4bdd2030ae417ed8a4db13a7e0cf16376a1d10259c00907e09d34ab5/` |
| 分区表 | NVS `0x9000`/`0x26000`(38 页)、phy `0x2f000`/`0x1000`、factory `0x30000`/`0x7d0000` |
| 资源包(v5,压缩 TXT) | 6,712,980 B,SHA-256 `9fc30e7cc300508e4184b26b9de46c760a91615097ab6e1b14750a92eaab794f` |
| 字库 | story **3,412** 码位、UI **449**、metrics **3,861** |
| 字库 SHA-256 | story `a4b51108…`、ui16 `295981c0…`、ui18 `fa1b571e…`、metrics `0046d223…` |
| 构建 / Host 测试 / 设备 | **PASS** / **29 PASS,4 FAIL,1 SKIP** / **NOT RUN** |

固件已包含完整主线剧情**与**手机内容:646 条邮件(609 条非空正文)、2,001 行上的
4,072 个带类型事件、21 个通话站点的 371 帧、65 个可选项节点、136 个选项、9 个结局,
以及 56 键的 presentation 表。v5 资源包把 1,190,478 B 文本切成 582 个独立的 2 KiB
raw-DEFLATE 块,阅读器通过 ESP32-C3 ROM 的 `tinfl_decompress` 一次解一块。

## Host 门禁(实际)

`bash tools/validate.sh --static` → **29 PASS / 4 FAIL / 1 SKIP**(另有 21 个子用例
跳过)。4 个失败是既有的 `baseline_demo_{audio,low_power,ble,wifi}_runtime` 链接错误
(`undefined symbol: ui_pixel_screen_create`);Steins 侧测试全部通过。**这不是全绿
门禁,也不按全绿汇报。**

唯一的 SKIP 是源审计,原因是 **`STEINS_SOURCE` 未设置**,因此未执行。独立的源内容
审核另行执行并通过:plain 224/0、v5 33/0、PRES 12/0、字库覆盖 18/0。

## 静态 RAM(实测,部分)

`_heap_start = 0x3fccb410`;A = **70,400 B**(`0x3fcdc710`),B = **14,576 B**;
**A + B = 84,976 B**,不是最大连续块。`.data` 7,300 B、`.bss` 231,800 B
(含 `s_app` 149,216 B 与其中 115,200 B 画布)。解码器静态 17,108 B
(`s_tinfl` 10,992 + `s_win` 4,096 + `s_work` 2,016 + `s_work_pack` 4),已计入 BSS。

已知动态分配(draw 19,200 + input 6,144 + LVGL 7,168 + main 3,584 +
esp_timer 3,584 = 39,680 B);若再加离线 35 项页的单块缓存(7,672 B)为
47,352 B,A 仍余约 23,048 B,尚未计入 TCB、队列、驱动、分配器元数据与碎片。
38 页的整个 Page 数组为 Page[38] = 38 × 84 = 3,192 B;按每页最多 5 个 128 B
HashList 块,全部 38 页的 Page + Hash 松上界为 3,192 + 38 × 5 × 128 = 27,512 B,
非每页开销或运行时实测,仍不含分配器开销。

### 离线 NVS 容量上界(合成)

38 页布局在官方 ESP-IDF 5.5.3 下的镜像是
`356ebbcbb66835eac5efe2befcf528407b54ce154a6304264796d9ce08ee1a70`:31 × 4,096 B
合成 checkpoint 打到硬 cap,再加 30 条 legacy 16 B、meta 与 checkpoint/legacy
shadow。结果为 **4,293 used / 495 free entries、3 个完全空页、65 项内容匹配**;
旧的 22 页镜像会触发官方 `InsufficientSizeError`。这是合成共存上界 —— 不是合法
codec 布局,也不是 31 槽真实剧情的最大状态 —— 不证明同键物理提交、GC 或掉电行为。
报告:`build/review-steins-38-20261102T091159Z-43900/REPORT_STEINS_38_HARDCAP.md`。

**运行时 free heap、最大块、峰值、物理 NVS 行为、LCD、字形渲染、按键、音频、
睡眠与帧时间均 NOT RUN。**

## 历史

早前的 `f1fb3029…` 固件(资源包 `5d97102c…`、6,900,805 B、app 8,088,096 B)是
**主线支持版**,不是当前手机内容完整的身份。更早的候选、审核与损坏源事故均作为
历史保留,其日志与二进制未改动。

## 独立审核

- `build/review-steins1-binding-20261102T095518Z-46350/REPORT.md`
- `build/review-steins-final-20261102T095557Z-46400/REPORT_FINAL_BIND.md`
- `build/review-steins-bss-20261102T100020Z-46821/ERRATA_BSS_SCOPE.md`
- `build/review-mobile-075447-20261102T080910Z-39727/REPORT.md`
- `build/review-pack-reader-final-20261102T084145Z-41819/REPORT.md`
- `build/review-font-20261102T091556Z-44240/REPORT.md`
