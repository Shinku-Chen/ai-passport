<p align="right"><strong>简体中文</strong> · <a href="README.md">English</a></p>

# 《星之终途》+《渺茫希望的日记》阅读器

面向 AI Passport 的横屏视觉小说阅读器，移植自小米手表 5 版本
`xuanyide01/stella-rw5`（commit `4fa1e7b5534aafbcae0c45b732a05615c3b46587`）。
应用基线为 `feature/galgame-framework` 的
`3c2e5013a9831c57124ef8f774193f0e06f1f14a`。

## 冻结交付状态：软件审核通过，待设备验收

- 状态：**软件审核通过，待设备验收**；Device **NOT RUN**。
- App：**8,255,424 字节**，SHA256
  `5851ceeb9bd7597c935a22d00d0c3667e7d437e25264421be843f65bcc98dff2`。
- 合并镜像：**8,320,960 字节**，SHA256
  `9ef1b9f94543213139dd71f4222c6de8cc6674be4d9fbce90232ec4c29cef1f1`。
- 同次已验证归档：
  `build/firmware/9ef1b9f94543213139dd71f4222c6de8cc6674be4d9fbce90232ec4c29cef1f1`
  （含 manifest、ELF/MAP、flash_args）。
- Lead-v2 独立证据：`build/lead-review-v2/app-probe.log`、
  `source-dialogue.log`、`images-probe.log`、`archive-verify.log`、`static.log`
  （11/11 追加 app 检查、932/932 图像资源、归档 verify 全通过）。
- 门禁修复证据：`build/gate-fix-20261102/normal-with-source.log`、
  `normal-no-source.log`、`gate-failure-injection.log`、`probe-gate-result.log`。
  图片测试编译失败现在输出 `Host tests: FAIL`、C 回归
  `PASS=19/20 FAIL=1` 并非零退出；source audit 的 SKIP 也会强制整体
  PARTIAL。正常运行：unittest PASS=93 SKIP=21 FAIL=0；C 回归
  PASS=20/20 FAIL=0；source audit PASS。
- Host 基线是 **PARTIAL**，不是全 PASS。21 个 SKIP 是平台相关 unittest
  跳过（音频/BLE/Wi-Fi/demo 环境），与 C 回归、source audit 分开计数。
- 应用分区仅余 **67,648 字节**；新增内容/代码前必须重新核分区。没有删除
  任何剧情/CG/差分/后日谈内容。
- 真机与真实 NVS 适配器验收均未运行。本轮未重新构建固件，上述字节/SHA
  就是被审核的产物。

## 内容覆盖

- 正篇：21 章、9,425 句对白、1 个结局（第 21 章）。
- 后日谈《渺茫希望的日记》：8 幕（101–107 与 109），正常路线 1,378 句，
  附加故事 109 另有 15 句；107:423 处有 1 个确认选项，目标为 107:424。
- 旧版尾声 108（157 句）保留在资源包中，但不列在章节选择里；它是上游为
  旧存档保留的重复尾声。
- 合计：10,975 句、30 幕、39 个源剧本分块、4 个结局、1 个选项。源目录里
  所有 `.txt` 分块都纳入（包括 3、8、9、10、13 等多分块场景）。
- 美术：932 个去重资源覆盖源仓库全部 1,066 个 ID（含别名）；256 个鉴赏组 /
  394 张差分；后日谈素材 187 个；没有把 `as_*` 当作未引用素材删掉。

## 操作

- 正文：UP 推进（长按快进、松手停）；DOWN 打开历史（长按切换自动阅读）；
  OK 打开菜单。
- 列表：UP/DOWN 选择，OK 确认，长按 OK 返回。
- 选项：OK 确认；确认后游标从选项目标继续。
- 标题：继续、开始故事、后日谈（需正篇通关）、章节、读取、鉴赏、设置。
  正篇结局后标题页出现后日谈入口。

## 存储与内存

- 资源包 6,722,596 字节，嵌入应用镜像。
- 图片：背景/CG 为 320×180 JPEG（质量 33）；立绘为 JPEG（质量 54）+
  1bpp alpha 遮罩，保留原始锚点偏移。
- 解码：TJpgDec 回调把 MCU 块直接写进 320×240 RGB565 画布；立绘在同一
  回调里按遮罩混合。没有整帧解码缓冲，也没有 PSRAM。
- 字体：2,775 个码位，16px 与 20px 子集（4bpp），由资源包正文 + UI
  字面量生成；`tools/stella_font.py --check` 通过。
- 应用分区：8,255,424 / 8,323,072 字节；合并镜像 8,320,960 字节（应用分区
  余 67,648 字节，约 1%）。静态 DRAM：289,456 / 321,296 字节。按同一 MAP，
  `_heap_start=0x3fcc6ab0` 到
  `SOC_ROM_STACK_START-SOC_ROM_STACK_SIZE=0x3fcdc710` 的堆为 89,184 字节；
  已点名/估算分配包括 153,600 字节画布和 40,960 字节 LVGL 池（静态）、
  19,200 字节刷屏缓冲、24,320 字节任务栈（main/idle/timer/esp_timer/event/
  LVGL/input）、队列/TCB 估算，剩余 43,840 字节，尚未扣除 SPI/面板/I2C/NVS
  等运行期驱动分配。这是链接期证据，不是真机堆测量；40,960 字节 LVGL 池
  只是选定配置，不是真机证明。
- NVS：`stella` 命名空间的 `cfg`、`unlock`、`s0`–`s6`；自动存档独立使用
  `stella_auto`/`auto`。存档 v2 显式保存 `story`/`chapter` 模式；旧 v1
  没有模式，明确拒绝而不猜测。`nvs_commit` 失败会显示保存失败并保留旧
  RAM/NVS 档；固件不会自动擦除 NVS。

## 构建与验证

```bash
python tools/stella_pack.py --source <stella-rw5 checkout> \
    --out assets/stella/stella_pack.bin --report build/stella_pack_report.json
python tools/stella_font.py --font <NotoSansSC-VF.ttf> \
    --pack assets/stella/stella_pack.bin --out-dir assets/fonts
idf.py build
./tools/validate.sh --static
```

`tools/stella_audit.py <source> assets/stella/stella_pack.bin` 会独立逐条核对
源目录的每个分块、正文/说话人、跳转、选项目标、结局与鉴赏/后日谈素材；
资产 ID 从资源包 META 的别名映射复算，不依赖 build report。
`tools/stella_mem_budget.py` 从同一 MAP 推导 DRAM/堆预算；
`tests/test_stella_app_regressions.c` 覆盖返回来源、真实
PRESS→RELEASE→CLICK 序列、NVS 提交失败、CG 解码失败与解锁重试；
`tests/test_stella_ui_visibility.c` 用仅记录 flag 的 LVGL 对象驱动生产版
`stella_ui.c`，检查页面/行可见性；`tests/test_stella_image.c` 链接生产版
图像层与项目 TJpgDec 源码，验证几何、缩放、JPEG 损坏与遮罩截断。

## 状态

构建、固件布局与宿主单元测试通过，但宿主有 21 个平台跳过，因此基线是
**PARTIAL**，不是全 PASS；真机测试 **NOT RUN**。本项目全程离线，
不含 Wi-Fi、BLE、音频、语音或视频。素材与文本版权归原权利方所有，本移植
仅供个人学习。
