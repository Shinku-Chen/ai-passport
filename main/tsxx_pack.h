// main/tsxx_pack.h —— 《天使☆騒々 RE-BOOT!》资源包的只读解析层。
//
// 包由 tools/tsxx_pack.py 生成,烧在独立的 assets 分区里:没有解压、没有运行时
// JSON 解析。本文件只做"字节 -> 结构"的纯映射,不依赖 ESP-IDF 与 LVGL(分区模式
// 才用 esp_partition),因此可以在宿主机上用平坦模式跑单元测试
// (tests/test_tsxx_model.c 用真实包走完整条剧情线)。
//
// 设备上包有 5.86 MiB,远超 ESP32-C3 的 Flash MMU 容量,不能整包映射(上一版把
// 它编进 app 的 rodata 段,引导加载器一映射就复位)。所以拆成两层:
//   * 常驻映射 [0, SEC_BG 偏移)  —— 约 1.9 MiB 的脚本区间,整个运行期不变;
//   * 滑动窗口 256 KiB        —— 只为图片与 CGDIR 按需映射(见窗口契约)。
//
// 字节序与字段布局必须与 tools/tsxx_pack.py 的文件头说明一致:
//   段表 { type u32, offset u32, count u32, size u32 },按 type 升序排列
//   带目录的段(BG/FG/EVB/EVC)格式为 [目录][数据],目录项里的 off 相对数据区起点
//
// 剧本是**线性页表**:61,436 页,每页自带背景/说话人/立绘/事件图/正文。
// 没有独立的章节与场景层 —— 章节跳转点由正文页里的 [CHAPTER x-y] 标记推导
// (见 tsxx_model 的 tsxx_chapter_*)。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TSXX_PACK_MAGIC "TSXXPK01"
#define TSXX_PACK_VERSION 1u

// 屏幕尺寸(文字层在原生分辨率上绘制)。
#define TSXX_SCREEN_W 240
#define TSXX_SCREEN_H 320
// 美术层(画布)尺寸 = 整屏尺寸:固件把它 1:1 铺在屏幕上,所以立绘不会再被
// 重采样。必须与 tools/tsxx_pack.py 的 --art-width 一致;tsxx_pack_open*() 会
// 核对 META 的 art=。
// 背景与事件图在包里更小(由打包器的 --bg-width / --event-width 决定,META 的
// bg= / event= 记录),合成时按最近邻放大到这个画布。
#define TSXX_ART_W 240
#define TSXX_ART_H 320

// 页面字段里的"没有"。
#define TSXX_NONE8 0xFFu
#define TSXX_NONE16 0xFFFFu

// 段表条数,必须与 tools/tsxx_pack.py 的 SEC_COUNT 一致。
#define TSXX_PACK_SECTIONS 22

// 设备模式(tsxx_pack_open_partition)的两层映射:
//   常驻映射覆盖 [0, 脚本区间末尾 = SEC_BG 的偏移),类型 0..15 的脚本段都指向它;
//   图片与 CGDIR 用 256 KiB 的滑动窗口按需映射。
// 常驻区间硬上限:脚本区间实际只有约 1.9 MiB,超过这个数说明包被重切过,
// 不能再当作"两个连续区间"来读。
#define TSXX_PACK_RESIDENT_LIMIT (2500u * 1024u)
// 滑动窗口最小粒度与上限(ESP32-C3 的 Flash MMU 页正好是 64 KiB)。
#define TSXX_PACK_WINDOW_MIN (64u * 1024u)
#define TSXX_PACK_WINDOW_SIZE (256u * 1024u)
// META 段(在包末尾,不能常驻映射)拷进结构体保存时的字节上限。
#define TSXX_PACK_META_MAX 512u

// page.flags
#define TSXX_PAGE_ZOOM (1u << 0)   // 源数据带缩放提示(全库都是同一个值)

// 事件图渲染配方(tsxx_cg_t.kind)。
#define TSXX_CG_FRAME 0u   // 直接画 img 指向的整帧
#define TSXX_CG_PATCH 1u   // 先画 base 指向的整帧,再把 img 指向的补丁贴到 (x, y)

// 名字表选择(tsxx_pack_name)。
#define TSXX_TABLE_BG 0u
#define TSXX_TABLE_SPEAKER 1u
#define TSXX_TABLE_SPRITE 2u
#define TSXX_TABLE_EVENT 3u

typedef struct {
    const uint8_t *blob;
    uint32_t blob_size;

    const uint8_t *syms;          // u32 x sym_count,下标 = 符号 id
    uint32_t sym_count;
    const uint8_t *text;          // 变长码流
    uint32_t text_size;
    const uint8_t *toff;          // u32 检查点,每 256 页一个
    const uint8_t *tlen;          // 每页字符数
    const uint8_t *pbg;           // 每页背景下标
    const uint8_t *pspk;          // 每页说话人下标
    const uint8_t *pspr;          // 每页立绘下标
    const uint8_t *pflag;         // 每页标志
    const uint8_t *pcgb;          // 有事件图的页位图
    const uint8_t *pcg;           // 有事件图的页 -> CGDIR 下标
    uint32_t page_count;
    uint32_t cg_page_count;

    const uint8_t *bg_names;      // \n 分隔
    uint32_t bg_name_size;
    const uint8_t *spk_names;
    uint32_t spk_name_size;
    const uint8_t *spr_names;
    uint32_t spr_name_size;
    const uint8_t *cg_names;
    uint32_t cg_name_size;

    const uint8_t *choices;       // { page u32, count u8, pad u8, pad u16, first u32 }
    uint32_t choice_count;
    const uint8_t *choice_opts;   // { off u32, len u8, pad u8, pad u16, target u32 }
    uint32_t choice_opt_count;

    // 带目录的图片段:目录 { ... } 与数据区都在包内靠 sec_off[SEC_*] + 计数寻址。
    // 目录与数据指针不再常驻:分区模式下它们落在滑动窗口里,取图时才算地址。
    uint32_t bg_count;            // BG 目录项 { off u32, len u32, w u16, h u16 }
    uint32_t bg_data_size;

    uint32_t fg_count;            // FG 目录项 { off, len, mask_off, mask_len, w, h, x, y }
    uint32_t fg_data_size;

    uint32_t evb_count;           // EVB 目录项,整帧 JPEG
    uint32_t evb_data_size;

    uint32_t evc_count;           // EVC 目录项,补丁 JPEG
    uint32_t evc_data_size;

    uint32_t cg_count;            // CGDIR 条数 = 事件名表条数
                                  // { kind u8, pad u8, pad u16, base u32, x,y,w,h u16, img u32 }

    // 背景与事件图的**存储**尺寸(META 的 bg= / event=)。它们可以比美术层小
    // (默认 180x240),合成时整帧放大到 TSXX_ART_W/H、补丁矩形按同一比例换算。
    // META 里没有这两行时按 1:1(= 美术层尺寸)处理。
    uint16_t bg_w;
    uint16_t bg_h;
    uint16_t ev_w;
    uint16_t ev_h;

    const uint8_t *meta;
    uint32_t meta_size;

    // ---- 存储方式 ----
    // false:tsxx_pack_open() 的"整块在内存"平坦模式(宿主机测试)。
    // true :tsxx_pack_open_partition() 的两层映射(设备),blob 指向常驻映射基址。
    bool partitioned;
    const uint8_t *resident;      // 常驻映射(覆盖 [0, resident_size))
    uint32_t resident_size;
    uint32_t resident_map;        // esp_partition_mmap_handle_t
    const void *partition;        // 分区模式:const esp_partition_t *
    const uint8_t *window;        // 当前滑动窗口基址(window_off 处)
    uint32_t window_off;
    uint32_t window_len;
    uint32_t window_map;          // esp_partition_mmap_handle_t
    // 每段在包内的绝对偏移:脚本段已被常驻映射覆盖,图片段靠它算窗口。
    uint32_t sec_off[TSXX_PACK_SECTIONS];
    // 分区模式下 META 的私有副本(meta 指向它);末尾留一个 NUL。
    uint8_t meta_buf[TSXX_PACK_META_MAX + 1u];
} tsxx_pack_t;

typedef struct {
    uint8_t bg;         // 背景下标
    uint8_t speaker;    // TSXX_NONE8 = 旁白
    uint8_t sprite;     // TSXX_NONE8 = 本页没有立绘
    uint8_t flags;      // TSXX_PAGE_*
    bool has_cg;        // 本页有事件图;CGDIR 下标用 tsxx_pack_cg_at() 取
    uint8_t text_len;   // 本页字符数;0 = 没有正文
    uint32_t text_off;  // 正文码流里的起始字节偏移
} tsxx_page_t;

typedef struct {
    const uint8_t *jpeg;
    uint32_t jpeg_len;
    uint16_t w;
    uint16_t h;
} tsxx_image_t;

// 立绘:JPEG(边缘做过颜色膨胀)+ 1bpp 遮罩,stride = (w + 7) / 8,位 1 = 不透明。
// x/y 是美术层坐标。
typedef struct {
    const uint8_t *jpeg;
    uint32_t jpeg_len;
    const uint8_t *mask;
    uint32_t mask_len;
    uint16_t w;
    uint16_t h;
    uint16_t x;
    uint16_t y;
} tsxx_sprite_t;

typedef struct {
    uint8_t kind;      // TSXX_CG_FRAME / TSXX_CG_PATCH
    uint32_t base;     // PATCH:基准帧的 CGDIR 下标
    uint16_t x;
    uint16_t y;
    uint16_t w;
    uint16_t h;
    uint32_t img;      // FRAME:EVB 下标;PATCH:EVC 下标
} tsxx_cg_t;

// 校验魔数/版本/段自洽并建立各段视图;失败返回 false(不改动 pack)。
// "平坦模式":整块资源包必须已经在内存里(宿主机测试走这条路)。
bool tsxx_pack_open(tsxx_pack_t *pack, const uint8_t *data, uint32_t size);

// 打开 label 分区里的资源包(设备模式):常驻映射脚本区间,图片与 CGDIR 用滑动窗口。
// 校验与 tsxx_pack_open() 相同(魔数/版本/总长/22 个段/各段长度),只读包头 512 字节
// 就能判完,不会等到真正取图时才报错。失败返回 false("未打开"状态,调用方不要再使用)。
//
// **窗口契约**:tsxx_pack_bg()/tsxx_pack_sprite()/tsxx_pack_cg_image() 返回的指针只在
// 下一次图片或 CGDIR 访问之前有效 —— 再访问会重新映射滑动窗口,旧指针立刻失效。
// 调用方必须"拿到就立刻解码",不要在两次访问之间同时保存多张图的指针。
// (tsxx_pack_sprite() 例外:它一次返回同一张立绘的 jpeg 与 mask,两者保证同时有效。)
//
// 窗口是包结构体里的共享状态:这些读接口不能从两个任务并发调用。
// 本应用的所有取图都在 bsp_lvgl_lock() 里串行执行。
bool tsxx_pack_open_partition(tsxx_pack_t *pack, const char *label);

uint32_t tsxx_pack_pages(const tsxx_pack_t *pack);
uint8_t tsxx_pack_bg_count(const tsxx_pack_t *pack);
uint8_t tsxx_pack_sprite_count(const tsxx_pack_t *pack);
uint16_t tsxx_pack_cg_count(const tsxx_pack_t *pack);

// 读取页表的一条。index 越界返回 false。
bool tsxx_pack_page(const tsxx_pack_t *pack, uint32_t index, tsxx_page_t *out);

// 事件图的稀疏表:位图 + 按页序排列的 CGDIR 下标。
// 顺序阅读时页序号与 rank 同步递增,只有跳页(读档 / 章节跳转)才需要重算 rank。
// tsxx_pack_cg_rank 的代价是 O(page/8) 次字位统计,一次性扫描请直接用 rank 走。
uint32_t tsxx_pack_cg_rank(const tsxx_pack_t *pack, uint32_t page);
// rank 必须小于 tsxx_pack_cg_total();返回 CGDIR 下标。
uint16_t tsxx_pack_cg_at(const tsxx_pack_t *pack, uint32_t rank);
uint32_t tsxx_pack_cg_total(const tsxx_pack_t *pack);

// 把页正文解码成 UTF-8:跳过 skip 个字符,最多写 max_chars 个字符。
// max_chars = 0 表示到结尾。out 总是以 NUL 结尾;超长时按字符边界截断。
// 返回写入的字节数(不含 NUL)。
size_t tsxx_pack_text(const tsxx_pack_t *pack, const tsxx_page_t *page, uint32_t skip,
                      uint32_t max_chars, char *out, size_t capacity);

// 名字表取值(table = TSXX_TABLE_*)。id 越界返回 0 字节。
size_t tsxx_pack_name(const tsxx_pack_t *pack, uint8_t table, uint16_t id, char *out,
                      size_t capacity);

bool tsxx_pack_bg(const tsxx_pack_t *pack, uint8_t id, tsxx_image_t *out);
bool tsxx_pack_sprite(const tsxx_pack_t *pack, uint8_t id, tsxx_sprite_t *out);
bool tsxx_pack_cg(const tsxx_pack_t *pack, uint16_t id, tsxx_cg_t *out);
// 取渲染配方指向的实际 JPEG(整帧或补丁)。
bool tsxx_pack_cg_image(const tsxx_pack_t *pack, const tsxx_cg_t *cg, tsxx_image_t *out);

// META 文本(key=value 逐行),便于日志与诊断。
const char *tsxx_pack_meta(const tsxx_pack_t *pack);

// 标题背景在背景表里的下标(META 的 title_bg=)。源素材没有可用的标题画,
// 由打包器的 --title-image 单独提供;没有标题图时返回 0。
uint8_t tsxx_pack_title_bg(const tsxx_pack_t *pack);

// 第 page 页是不是选项点;是则返回选项数(1..5),否则 0。
uint8_t tsxx_pack_choice_count(const tsxx_pack_t *pack, uint32_t page);
// 读第 slot(0 起)个选项的文案与目标页。成功返回 true。
bool tsxx_pack_choice_option(const tsxx_pack_t *pack, uint32_t page, uint8_t slot,
                             char *out, size_t capacity, uint32_t *target_page);
