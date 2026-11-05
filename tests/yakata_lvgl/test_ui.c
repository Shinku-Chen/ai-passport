#include "gal_ui.h"
#include "gal_settings.h"
#include "gal_text.h"
#include "gal_art.h"
#include "yakata_flow.h"
#include "fixture.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

LV_FONT_DECLARE(gal_cjk_16);
LV_FONT_DECLARE(gal_cjk_20);
uint8_t art[8*1024*1024] __asm__("_binary_yakata_art_bin_start");
static uint16_t frame[320*240];
static uint8_t drawbuf[320*40*2];
uint8_t gal_panel_opa(uint8_t v) { return (uint8_t)(255-(v*255+50)/100); }
static void flush(lv_display_t *disp,const lv_area_t *a,uint8_t *map) {
    lv_draw_buf_t *buf=lv_display_get_buf_active(disp);
    for(int y=a->y1;y<=a->y2;++y) {
        assert(y>=0&&y<240&&a->x1>=0&&a->x2<320);
        memcpy(frame+y*320+a->x1,map+(y-a->y1)*buf->header.stride,(a->x2-a->x1+1)*2);
    }
    lv_display_flush_ready(disp);
}
static void inside(lv_obj_t *obj,const char *name) {
    if(!obj)return;
    lv_area_t a;lv_obj_get_coords(obj,&a);
    if(a.x1<0||a.y1<0||a.x2>=320||a.y2>=240) {
        fprintf(stderr,"outside %s: %d,%d .. %d,%d\n",name,(int)a.x1,(int)a.y1,(int)a.x2,(int)a.y2);assert(0);
    }
}
static void shot(const char *name,int large) {
    char path[128];snprintf(path,sizeof(path),"%s-%d.ppm",name,large);
    FILE *f=fopen(path,"wb");assert(f);fprintf(f,"P6\n320 240\n255\n");
    for(int i=0;i<320*240;++i) {
        uint16_t p=frame[i];uint8_t rgb[]={((p>>11)&31)*255/31,((p>>5)&63)*255/63,(p&31)*255/31};fwrite(rgb,1,3,f);
    }fclose(f);
}
static void compact_text_checks(gal_ui_t *ui,int large) {
    gal_ui_show_page(ui,GAL_PAGE_GAME);
    const lv_font_t *font=large?ui->font_large:ui->font_small;
    for(int i=0;i<gal_content_line_count;++i) {
        const gal_line_t *line=&gal_content_lines[i];
        uint32_t offsets[33];
        int pages=gal_ui_text_pages(ui,line->text,offsets,GAL_TEXT_MAX_PAGES+1);
        assert(pages>=1 && pages<=GAL_TEXT_MAX_PAGES);
        for(int p=0;p<pages;++p) {
            size_t n=offsets[p+1]-offsets[p];
            while(n && (line->text[offsets[p]+n-1]==10 || line->text[offsets[p]+n-1]==13))--n;
            char text[GAL_TEXT_CAPACITY];assert(n<sizeof(text));memcpy(text,line->text+offsets[p],n);text[n]=0;
            gal_ui_set_text(ui,line->speaker,text,0,NULL,NULL);
            lv_obj_update_layout(lv_screen_active());
            lv_area_t panel,body,name;lv_obj_get_coords(ui->panel,&panel);lv_obj_get_coords(ui->text,&body);
            if(body.y1<panel.y1 || body.y2>panel.y2 || body.x2>=320)
                fprintf(stderr,"record=%d font=%d page=%d bytes=%zu panel=%d..%d body=%d..%d x2=%d\n",i,large,p,n,(int)panel.y1,(int)panel.y2,(int)body.y1,(int)body.y2,(int)body.x2);
            assert(body.y1>=panel.y1 && body.y2<=panel.y2 && body.x2<320);
            assert(lv_obj_get_height(ui->panel)==80);
            if(line->speaker[0]) {
                lv_point_t size;lv_text_get_size(&size,line->speaker,font,0,0,LV_COORD_MAX,LV_TEXT_FLAG_NONE);
                assert(!lv_obj_has_flag(ui->name,LV_OBJ_FLAG_HIDDEN));
                assert(lv_obj_get_width(ui->name)==size.x+16);
                lv_obj_get_coords(ui->name,&name);assert(name.y2==panel.y1-3);
                inside(ui->name,"dynamic name");
            } else assert(lv_obj_has_flag(ui->name,LV_OBJ_FLAG_HIDDEN));
        }
    }
    printf("Compact dialogue: all %d records fit, content-sized names and layout %d PASS\n",gal_content_line_count,large);
}
static void cache_checks(lv_display_t *disp) {
    assert(gal_art_init());
    lv_obj_t *picture=lv_image_create(lv_screen_active());
    lv_obj_set_pos(picture,0,0);
    bool changed=false;
    const lv_image_dsc_t *cached=gal_art_large(&fixture,&changed);
    assert(cached && changed);
    lv_image_set_src(picture,cached);
    lv_refr_now(disp);
    // Fixed primary colours catch BGR/RGB and RGB565 byte-order mistakes.
    assert((frame[60*320+80] & 0xf800) >= 0xf000);
    assert((frame[60*320+240] & 0x07e0) >= 0x0700);
    assert((frame[180*320+80] & 0x001f) >= 0x001e);
    assert(frame[180*320+240] >= 0xffde);
    static uint16_t expected[320*240];memcpy(expected,frame,sizeof(frame));
    gal_art_stats_t start=gal_art_stats();
    for(int i=0;i<80;++i) {
        changed=true;assert(gal_art_large(&fixture,&changed)==cached && !changed);
        lv_area_t clip={.x1=13+i,.y1=151,.x2=211,.y2=174};
        lv_obj_invalidate_area(picture,&clip);lv_refr_now(disp);
        assert(memcmp(frame,expected,sizeof(frame))==0);
    }
    gal_art_stats_t end=gal_art_stats();
    assert(end.jpeg_decodes==start.jpeg_decodes && end.cache_hits>=start.cache_hits+80);
    // Each partial draw touches only clipped rows, not all 240 rows.
    assert(end.output_rows>start.output_rows && end.output_rows-start.output_rows<=80*24);
    for(int i=0;i<8;++i) assert(gal_art_thumb(i,&fixture,&changed) && changed);
    start=gal_art_stats();
    for(int i=0;i<8;++i) assert(gal_art_thumb(i,&fixture,&changed) && !changed);
    assert(gal_art_stats().jpeg_decodes==start.jpeg_decodes);
    assert(gal_art_large(&fixture,&changed) && changed);
    assert(!gal_art_thumb(-1,&fixture,&changed));
    assert(!gal_art_thumb(8,&fixture,&changed));
    lv_image_dsc_t invalid=fixture;invalid.data_size=10;
    assert(!gal_art_large(&invalid,&changed));
    assert(gal_art_large(&fixture,&changed) && changed);
    lv_obj_delete(picture);
    puts("Cached art: colours, clipped rows, 80 redraws without JPEG decode, arena switches and invalid input PASS");
}
int main(int argc,char **argv) {
    assert(argc==2);FILE *f=fopen(argv[1],"rb");assert(f);size_t n=fread(art,1,sizeof(art),f);fclose(f);assert(n>5000000);
    lv_init();lv_display_t *disp=lv_display_create(320,240);assert(disp);
    lv_display_set_color_format(disp,LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(disp,drawbuf,NULL,sizeof(drawbuf),LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp,flush);
    cache_checks(disp);
    static gal_ui_t ui;assert(gal_ui_create(&ui,&gal_cjk_16,&gal_cjk_20));
    gal_ui_set_brand(&ui,GAL_CONTENT_TITLE,GAL_CONTENT_SUBTITLE);
    gal_ui_set_title_image(&ui,gal_content_title_image);
    gal_ui_set_battery(&ui,95);
    const char *base_rows[]={"开始游戏","继续游戏","读取存档","设置","鉴赏室","关于","返回标题","保存进度"};
    const char *base_values[]={"中","大","开启","30%","可选","锁定","返回",""};
    const char *rows[16], *values[16];
    for(int i=0;i<16;++i) { rows[i]=base_rows[i%8]; values[i]=base_values[i%8]; }
    const int counts[GAL_LIST_COUNT]={0,6,3,6,3,3,7,16,4,8,0,8,0,2};
    gal_cg_t cgs[8];for(int i=0;i<8;++i){cgs[i]=gal_content_cgs[i];cgs[i].unlocked=true;}
    for(int large=0;large<2;++large) {
        gal_ui_set_font_size(&ui,large);
        gal_ui_set_panel_transparency(&ui,30);
        compact_text_checks(&ui,large);
        for(int list=0;list<GAL_LIST_COUNT;++list) {
            int count=counts[list];
            lv_mem_monitor_t before;lv_mem_monitor(&before);printf("list %d free %zu\n",list,before.free_size);fflush(stdout);
            const char *const *labels=rows;
            if(list==GAL_LIST_TITLE)labels=gal_content_title_menu;
            if(list==GAL_LIST_MENU)labels=gal_content_quick_menu;
            if(list==GAL_LIST_GALLERY)labels=gal_content_gallery_menu;
            gal_ui_set_rows(&ui,list,labels,list==GAL_LIST_SETTINGS?values:NULL,count,0);
        }
        const char *story="这是用于横屏排版检查的文字。画面保持原始比例。\n第二行文字检查。\n第三行文字检查。";
        uint32_t offsets[33];assert(gal_ui_text_pages(&ui,story,offsets,33)>0);
        size_t length=offsets[1];while(length && (story[length-1]==10 || story[length-1]==13))--length;
        char page_text[1024];memcpy(page_text,story,length);page_text[length]=0;
        gal_ui_set_text(&ui,"角色名称",page_text,0,gal_content_lines[60].image,NULL);
        gal_ui_set_chapter(&ui,"终章 · 602_1");
        gal_ui_set_gallery(&ui,cgs,8,7);
        gal_ui_set_cg(&ui,"CG 56 1/2","CG 56","上/下差分；确定返回",gal_content_lines[60].image);
        gal_ui_set_transition(&ui,"SHINUYAKATA","终章 · 602_1","死馆 · 离线阅读");
        gal_ui_set_ending(&ui,"SOURCE EXIT","终章 · 602_1","源移植版此处结束。请长按确定返回标题。完成横屏排版检查。");
        gal_ui_set_about(&ui,GAL_CONTENT_TITLE,GAL_CONTENT_LOGO_SUB,GAL_CONTENT_ABOUT);
        gal_ui_set_chapter_info(&ui,"支线 · 512_3_2H");
        gal_ui_set_action_hint(&ui,"上/下选择；长按确定返回");
        for(int page=0;page<GAL_PAGE_COUNT;++page) {
            gal_ui_show_page(&ui,page);
            if(page==GAL_PAGE_CG_LIST)gal_ui_set_gallery(&ui,cgs,8,7);
            if(page==GAL_PAGE_CHOICE) gal_ui_set_choice(&ui,"请选择",gal_content_choices[5].options,3,0);
            if(page==GAL_PAGE_MENU)gal_ui_select_row(&ui,GAL_LIST_MENU,5);
            lv_obj_update_layout(lv_screen_active());lv_refr_now(disp);
            inside(ui.pages[page],"page");
            if(page==GAL_PAGE_TITLE) { inside(ui.brand_title,"brand");inside(ui.brand_sub,"subtitle"); }
            if(page==GAL_PAGE_GAME) { inside(ui.panel,"panel");inside(ui.name,"speaker");inside(ui.text,"text"); }
            if(page==GAL_PAGE_CG_LIST) {
                for(int i=0;i<8;++i) {
                    inside(ui.cg_tiles[i],"tile");inside(ui.tile_img[i],"thumbnail");inside(ui.cg_tiles_label[i],"CG label");
                    lv_area_t image,label;lv_obj_get_coords(ui.tile_img[i],&image);lv_obj_get_coords(ui.cg_tiles_label[i],&label);assert(image.y2<label.y1);
                }
            }
            if(page==GAL_PAGE_CHOICE) { lv_area_t a,b;lv_obj_get_coords(ui.choice_box,&a);lv_obj_get_coords(ui.panel,&b);assert(a.y2<b.y1); }
            if(page==GAL_PAGE_MENU) { lv_area_t a,b;lv_obj_get_coords(lv_obj_get_parent(ui.lists[GAL_LIST_MENU].rows[5]),&a);lv_obj_get_coords(ui.lists[GAL_LIST_MENU].page,&b);assert(a.y1>=b.y1 && a.y2<=b.y2); }
            if(page==GAL_PAGE_ABOUT)inside(ui.about_view,"about viewport");
            if(page==GAL_PAGE_CG){inside(ui.cg_name,"CG name");inside(ui.cg_caption,"CG caption");}
            char name[40];snprintf(name,sizeof(name),"page-%02d",page);shot(name,large);
        }
        const char *actions[]={"取消","确认覆盖"};
        for(int empty=0;empty<2;++empty) {
            gal_ui_show_page(&ui,GAL_PAGE_SAVE);
            gal_ui_show_modal(&ui,empty?GAL_MODAL_EMPTY:GAL_MODAL_CONFIRM,"确认覆盖",empty?"尚未保存任何进度。":"槽位 3：支线 · 512_3_2H\n覆盖后无法撤销。",actions,2,1);
            lv_obj_update_layout(lv_screen_active());lv_refr_now(disp);
            inside(ui.modal_box,"modal");inside(ui.modal_body,"modal body");
            inside(lv_obj_get_parent(ui.lists[GAL_LIST_MODAL].rows[1]),"modal last action");shot(empty?"modal-empty":"modal-confirm",large);
            gal_ui_hide_modal(&ui);
        }
    }
    // Real typewriter ticks through a translucent panel must never decode JPEG.
    gal_ui_show_page(&ui,GAL_PAGE_GAME);
    gal_ui_set_text(&ui,"角色", "文字逐字显示时背景图片只解码一次。继续阅读不会重复解码同一图片。",40,gal_content_lines[60].image,NULL);
    lv_refr_now(disp);gal_art_stats_t typing_start=gal_art_stats();
    for(int i=0;i<80;++i) {lv_tick_inc(41);lv_timer_handler();lv_refr_now(disp);}
    assert(!gal_ui_typing(&ui));
    assert(gal_art_stats().jpeg_decodes==typing_start.jpeg_decodes);
    for(int i=0;i<30;++i) {
        gal_ui_set_text(&ui,"角色",i%2?"继续阅读。":"下一页文字。",0,gal_content_lines[60].image,NULL);
        lv_refr_now(disp);
    }
    assert(gal_art_stats().jpeg_decodes==typing_start.jpeg_decodes);
    puts("Reader: typewriter and 30 same-scene page updates without JPEG decode PASS");
    // Every imported image must decode; no JPEG ever reaches LVGL's unsafe
    // built-in information callback. Gallery selection reuses all eight slots.
    const void *seen[900];int seen_count=0;
    for(int i=0;i<gal_content_line_count;++i) {
        const void *image=gal_content_lines[i].image;bool known=false;
        for(int j=0;j<seen_count;++j)if(seen[j]==image){known=true;break;}
        if(known || !image)continue;
        assert(seen_count<900);seen[seen_count++]=image;
        assert(gal_art_large(image,NULL));
    }
    int variants=yakata_cg_offsets[gal_content_cg_count];
    for(int i=0;i<variants;++i)assert(gal_art_large(yakata_cg_images[i],NULL));
    for(int first=0;first<gal_content_cg_count;first+=8) {
        int count=gal_content_cg_count-first;if(count>8)count=8;
        for(int i=0;i<count;++i){cgs[i]=gal_content_cgs[first+i];cgs[i].unlocked=true;}
        gal_ui_show_page(&ui,GAL_PAGE_CG_LIST);gal_ui_set_gallery(&ui,cgs,count,0);lv_refr_now(disp);
        for(int i=0;i<count;++i)assert(!lv_obj_has_flag(ui.tile_img[i],LV_OBJ_FLAG_HIDDEN));
        gal_art_stats_t start=gal_art_stats();
        for(int i=0;i<count;++i){gal_ui_set_gallery(&ui,cgs,count,i);lv_refr_now(disp);}
        assert(gal_art_stats().jpeg_decodes==start.jpeg_decodes);
    }
    gal_ui_show_page(&ui,GAL_PAGE_GAME);lv_refr_now(disp);
    gal_art_stats_t restored=gal_art_stats();
    gal_ui_show_page(&ui,GAL_PAGE_GAME);lv_refr_now(disp);
    assert(gal_art_stats().jpeg_decodes==restored.jpeg_decodes);
    printf("Imported art: %d distinct story images, %d CG variants, %d thumbnail groups and reader restore PASS\n",seen_count,variants,gal_content_cg_count);
    lv_mem_monitor_t mon;lv_mem_monitor(&mon);
    printf("Headless LVGL: all reader pages and both modals, two font tiers PASS; host pool max %u%%\n",(unsigned)mon.max_used*100/(96*1024));
    return 0;
}
