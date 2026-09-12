/**
 * page_storage.c — 存储管理 页面模块 (3.3 版 list_dialog 复刻).
 *
 * 主菜单"存储"打开 → 存储管理列表弹窗:
 *   浏览文件 / 挂载到电脑 / 格式化TF卡 / 存储信息 / 返回
 * 与 3.3 一致用 list_dialog 弹窗样式 (固定 350×250, 底部固定返回行).
 * 私有 state 全部 static 留在本文件.
 */
#include "os.h"
#include "ui_common.h"   /* UI_SCREEN_W/H, fill_rect, draw_hline/vline, draw_text_centered */
#include "input.h"
#include "sd_scan.h"
#include "usbh_msc_sdspi.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>

#define TAG "PSTO"

static void open_sd_dlg(ui_ctx_t *ctx);

/* 返回/取消 → 退出存储页 (回主菜单) */
static void sd_close(ui_ctx_t *ctx, int result, void *ud) {
    (void)result; (void)ud;
    os_pop(ctx);
}

/* ---- 存储信息弹窗 (3.3: 只读多行 + 返回) ---- */
static void sd_info_noop(ui_ctx_t *ctx, int result, void *ud) {
    (void)ctx; (void)result; (void)ud;   /* 只读 */
}
static void open_sd_info(ui_ctx_t *ctx) {
    EXT_RAM_BSS_ATTR static char items[6][40];
    static const char *ptrs[6];
    int n = 0;
    uint64_t total = 0, free = 0;
    if (sd_get_info(&total, &free) == 0) {
        uint32_t total_mb = (uint32_t)(total / (1024ULL * 1024ULL));
        uint32_t free_mb  = (uint32_t)(free  / (1024ULL * 1024ULL));
        uint32_t used_mb  = total_mb > free_mb ? (total_mb - free_mb) : 0;
        snprintf(items[n++], sizeof(items[0]), "\xe6\x80\xbb\xe5\xae\xb9\xe9\x87\x8f: %luMB", (unsigned long)total_mb); /* 总容量 */
        snprintf(items[n++], sizeof(items[0]), "\xe5\xb7\xb2\xe7\x94\xa8:  %luMB", (unsigned long)used_mb);            /* 已用 */
        snprintf(items[n++], sizeof(items[0]), "\xe5\x89\xa9\xe4\xbd\x99:  %luMB", (unsigned long)free_mb);            /* 剩余 */
    } else {
        snprintf(items[n++], sizeof(items[0]), "%s", sd_is_mounted()
                 ? "\xe7\x8a\xb6\xe6\x80\x81: \xe9\x87\x8d\xe8\xaf\xbb\xe5\xa4\xb1\xe8\xb4\xa5"   /* 状态: 重读失败 */
                 : "\xe7\x8a\xb6\xe6\x80\x81: \xe6\x9c\xaa\xe6\x8c\x82\xe8\xbd\xbd");            /* 状态: 未挂载 */
    }
    for (int i = 0; i < n; i++) ptrs[i] = items[i];
    os_dialog_list(ctx, "\xe5\xad\x98\xe5\x82\xa8\xe4\xbf\xa1\xe6\x81\xaf", ptrs, n, 0, sd_info_noop, NULL); /* 存储信息 */
}

/* ---- 格式化TF卡: 三次重复确认防误触, 第 3 次确认才真正执行 ---- */
static int s_fmt_stage = 0;   /* 1..3 当前确认到第几步; 取消/完成归零 */

static void sd_format_stage(ui_ctx_t *ctx);   /* 按 s_fmt_stage 弹出当前步的确认框 */

static void sd_format_done(ui_ctx_t *ctx, int result, void *ud) {
    (void)ud;
    if (result != 0) {           /* 取消 / 返回: 直接关确认框, 归零 */
        s_fmt_stage = 0;
        return;
    }
    os_dialog_pop(ctx);          /* 确认: 先关当前确认框 */
    if (s_fmt_stage < 3) {
        s_fmt_stage++;
        sd_format_stage(ctx);    /* 还有未走完的确认 → 出下一步 */
    } else {
        s_fmt_stage = 0;
        int rc = sd_format_and_create_dirs();
        os_dialog_toast(ctx, (rc == 0)
                        ? "\xe6\xa0\xbc\xe5\xbc\x8f\xe5\x8c\x96\xe5\xae\x8c\xe6\x88\x90"           /* 格式化完成 */
                        : "\xe6\xa0\xbc\xe5\xbc\x8f\xe5\x8c\x96\xe5\xa4\xb1\xe8\xb4\xa5");         /* 格式化失败 */
    }
}

static void sd_format_stage(ui_ctx_t *ctx) {
    const char *msg;
    switch (s_fmt_stage) {
    case 1: msg = "\xe7\xa1\xae\xe5\xae\x9a\xe6\xa0\xbc\xe5\xbc\x8f\xe5\x8c\x96TF\xe5\x8d\xa1?"; /* 确定格式化TF卡? */
        break;
    case 2: msg = "\xe5\xb0\x86\xe6\xb8\x85\xe7\xa9\xba\xe5\x8d\xa1\xe5\x86\x85\xe6\x89\x80\xe6\x9c\x89\xe6\x95\xb0\xe6\x8d\xae!"; /* 将清空卡内所有数据! */
        break;
    default: msg = "\xe6\x9c\x80\xe5\x90\x8e\xe7\xa1\xae\xe8\xae\xa4\xe6\xa0\xbc\xe5\xbc\x8f\xe5\x8c\x96?"; /* 最后确认格式化? */
        break;
    }
    os_dialog_confirm_ex(ctx, msg, 0, 0, sd_format_done, NULL);
}

static void open_sd_format(ui_ctx_t *ctx) {
    s_fmt_stage = 1;
    sd_format_stage(ctx);
}

/* =====================================================================
 * 挂载/卸载 — 复用确认弹窗 + 图标弹窗, 真实挂载移到后台任务 (UI 不卡顿).
 *
 * 关键修复(彻底解决"点挂载后等十几秒"): 之前 sd_mount_confirm 在 UI 回调里
 * 同步执行 usbh_msc_start()——Serial/JTAG 卸载 + tinyusb 安装 + PHY 切 OTG
 * 独占式耗时十几秒, UI 白屏卡死. 现在:
 *   1. 点"确定" → 立刻关闭确认框, 弹出"挂载中…"图标弹窗(含挂载图标 + 单按钮"退出挂载");
 *   2. 真实挂载 (sd_unmount_vfs_keep_card + usbh_msc_start) 放到独立低优先级后台任务,
 *      UI 继续流畅; 完成后由弹窗 on_poll 把标题切成"已挂载";
 *   3. 全程可取消: 挂载中按"退出挂载" → 置取消标志, 后台就绪后立即拆除重挂;
 *      已挂载按"退出挂载" → 停止 MSC + 重挂 VFS + 恢复 watcher, 无需重启.
 * ===================================================================== */
#include "mount_icon.inc"

/* 后台挂载/卸载状态机 (on_poll 轮询, 避免跨任务直接改 UI):
 *  -2=挂载中; -1=空闲; 0=已挂载; 1=卸载VFS失败; 2=usbh_msc_start失败; 3=挂载中取消; 4=已卸载 */
#define M_MOUNTING   -2
#define M_IDLE       -1
#define M_MOUNTED     0
#define M_MOUNT_FAIL1 1
#define M_MOUNT_FAIL2 2
#define M_CANCELLED   3
#define M_UNMOUNTED   4
static volatile int  s_mount_result  = M_IDLE;
static volatile bool s_mount_cancel  = false;
static volatile bool s_unmounting    = false;   /* 后台正执行卸载 (UI 不卡顿) */
static TaskHandle_t  s_mount_task    = NULL;

/* 停止 USB MSC 并重挂 TF (退出挂载/取消挂载共用) */
static void mount_teardown(void) {
    usbh_msc_stop();
    sd_remount_vfs_from_card();
    sd_watcher_set_paused(false);
}

/* 后台任务: 真实执行挂载, 并记录"内部速度/挂载耗时"供串口核对 */
static void mount_worker_task(void *arg) {
    (void)arg;
    sd_watcher_set_paused(true);          /* 先暂停 SD watcher (VFS 卸载后 PC 读写扇区) */
    int64_t t0 = esp_timer_get_time();

    int rc1 = sd_unmount_vfs_keep_card();               /* 卸载 VFS, 保留 card */
    int64_t t1 = esp_timer_get_time();
    if (rc1 != 0) {
        sd_watcher_set_paused(false);
        s_mount_result = M_MOUNT_FAIL1;
        ESP_LOGE(TAG, "挂载失败: 卸载 VFS 失败");
        vTaskDelete(NULL);
        return;
    }

    esp_err_t rc2 = usbh_msc_start();                   /* Serial/JTAG 卸载 + tinyusb + PHY 切 OTG */
    int64_t t2 = esp_timer_get_time();
    ESP_LOGI(TAG, "[挂载耗时] 卸载VFS %lldms | tinyusb+PHY切换 %lldms | 合计 %lldms (内部速度)",
             (long long)((t1 - t0) / 1000),
             (long long)((t2 - t1) / 1000),
             (long long)((t2 - t0) / 1000));

    if (rc2 != ESP_OK) {
        sd_remount_vfs_from_card();
        sd_watcher_set_paused(false);
        s_mount_result = M_MOUNT_FAIL2;
        ESP_LOGE(TAG, "挂载失败: usbh_msc_start %s", esp_err_to_name(rc2));
        vTaskDelete(NULL);
        return;
    }
    if (s_mount_cancel) {                                /* 用户挂载中已取消 → 立即拆除 */
        mount_teardown();
        s_mount_result = M_CANCELLED;
        ESP_LOGI(TAG, "挂载中取消: MSC 停止, VFS 已重挂");
        vTaskDelete(NULL);
        return;
    }
    s_mount_result = M_MOUNTED;                          /* 成功 */
    ESP_LOGI(TAG, "挂载成功 (USB MSC), 请连接电脑");
    vTaskDelete(NULL);
}

/* 后台任务: 卸载 (停止 USB MSC + 重挂 VFS + 恢复 watcher), 记录耗时 — UI 不卡顿 */
static void unmount_worker_task(void *arg) {
    (void)arg;
    int64_t t0 = esp_timer_get_time();
    usbh_msc_stop();
    sd_remount_vfs_from_card();
    sd_watcher_set_paused(false);
    int64_t t1 = esp_timer_get_time();
    ESP_LOGI(TAG, "[卸载耗时] 停止MSC+重挂VFS %lldms (内部速度)",
             (long long)((t1 - t0) / 1000));
    s_unmounting = false;
    s_mount_result = M_UNMOUNTED;
    ESP_LOGI(TAG, "卸载完成 (MSC 已停止, VFS 已重挂)");
    vTaskDelete(NULL);
}

/* 图标弹窗 on_poll: 后台挂载/卸载完成后的 UI 收尾.
 * 挂载中/后台卸载中/已挂载 均保持弹窗; 仅在 取消/失败/卸载完成 时收尾. */
static void mount_state_poll(ui_ctx_t *ctx, os_dlg_stack_t *d, void *ud) {
    (void)ud;
    int st = s_mount_result;
    if (s_unmounting || st == M_MOUNTING || st == M_MOUNTED || st == M_IDLE) return;
    s_mount_result = M_IDLE;                             /* 消费本次状态 */
    if (st == M_CANCELLED) {
        os_dialog_pop(ctx);
        os_dialog_toast(ctx, "\xe5\xb7\xb2\xe5\x8f\x96\xe6\xb6\x88\xe6\x8c\x82\xe8\xbd\xbd"); /* 已取消挂载 */
    } else if (st == M_UNMOUNTED) {
        os_dialog_pop(ctx);
        os_dialog_toast(ctx, "\xe5\xb7\xb2\xe5\x8d\xb8\xe8\xbd\xbd"); /* 已卸载 */
    } else { /* M_MOUNT_FAIL1 / M_MOUNT_FAIL2 */
        os_dialog_pop(ctx);
        os_dialog_toast(ctx, (st == M_MOUNT_FAIL1)
                        ? "\xe5\x8d\xb8\xe8\xbd\xbd\xe5\xa4\xb1\xe8\xb4\xa5"   /* 卸载失败 */
                        : "\xe6\x8c\x82\xe8\xbd\xbd\xe5\xa4\xb1\xe8\xb4\xa5"); /* 挂载失败 */
    }
}

/* "已挂载" 状态按钮 (同一框仅图标): 0=卸载, 1=取消(无反应). 卸载走后台, 不卡 UI. */
static void mount_btn_cb(ui_ctx_t *ctx, int result, void *ud) {
    (void)ud;
    if (result != 0) {                    /* 取消: 无任何反应, 保持弹窗 */
        os_dialog_mark_handled(ctx);
        return;
    }
    if (s_unmounting || s_mount_result == M_MOUNTING) {  /* 挂载/卸载中 → 标记取消 */
        s_mount_cancel = true;
        os_dialog_mark_handled(ctx);
        return;
    }
    if (s_mount_result == M_MOUNTED) {    /* 已挂载 → 后台卸载 (UI 不卡顿) */
        s_unmounting = true;
        s_mount_cancel = false;
        os_dialog_mark_handled(ctx);      /* 保持弹窗 (显示图标), 卸载完成后再关 */
        if (xTaskCreate(unmount_worker_task, "unmount", 3072, NULL, 5, &s_mount_task) != pdPASS) {
            s_unmounting = false;
            ESP_LOGE(TAG, "卸载后台任务创建失败");
        }
        return;
    }
}

/* 确定挂载 (同一弹窗原位变形, 不弹新窗): 确认框 → "已挂载!"+图标+卸载/取消.
 * 真实挂载移到后台任务 (UI 不卡顿); 宽保持同宽 fix_w, 高度随图标扩高. */
static void sd_mount_confirm(ui_ctx_t *ctx, int result, void *ud) {
    (void)ud;
    if (result != 0) return;   /* 取消/返回 → 默认弹栈关闭, 不挂载 */
    os_dlg_stack_t *d = os_dialog_top_mut(ctx);   /* 同一框可写句柄 */
    if (!d) return;
    os_dialog_mark_handled(ctx);                    /* 保持同一弹窗, 不自动关闭 */
    /* 原位改写成"仅图标" 状态 (去掉文字, 只留图片): 标题清空 + 图标 + 卸载/取消 */
    d->title[0] = '\0';
    d->count = 2;
    snprintf(d->items[0], sizeof(d->items[0]), "\xe5\x8d\xb8\xe8\xbd\xbd");      /* 卸载 */
    snprintf(d->items[1], sizeof(d->items[1]), "\xe5\x8f\x96\xe6\xb6\x88");      /* 取消 */
    d->sel = 0;
    d->icon = mount_icon_data;                       /* 触发图标窗扩高 */
    d->icon_w = MOUNT_ICON_W;
    d->icon_h = MOUNT_ICON_H;
    d->cb = mount_btn_cb;
    d->on_poll = mount_state_poll;
    s_mount_result = M_MOUNTING;
    s_mount_cancel = false;
    s_unmounting = false;
    if (xTaskCreate(mount_worker_task, "mount", 3072, NULL, 5, &s_mount_task) != pdPASS) {
        s_mount_result = M_MOUNT_FAIL2;              /* 任务创建失败按挂载失败处理 */
        ESP_LOGE(TAG, "挂载后台任务创建失败");
    }
}

static void open_sd_mount(ui_ctx_t *ctx) {
    os_dlg_stack_t dlg;
    memset(&dlg, 0, sizeof(dlg));
    snprintf(dlg.title, sizeof(dlg.title), "\xe7\xa1\xae\xe5\xae\x9a\xe6\x8c\x82\xe8\xbd\xbd\xe5\x88\xb0\xe7\x94\xb5\xe8\x84\x91?"); /* 确定挂载到电脑? */
    dlg.count = 2;
    snprintf(dlg.items[0], sizeof(dlg.items[0]), "\xe7\xa1\xae\xe5\xae\x9a");   /* 确定 */
    snprintf(dlg.items[1], sizeof(dlg.items[1]), "\xe5\x8f\x96\xe6\xb6\x88");   /* 取消 */
    dlg.sel = 0;
    dlg.no_footer = true;
    dlg.small = true;
    dlg.fix_w = text_width(dlg.title) + 24;           /* 固定宽度: 两阶段/变形前后完全一致 */
    dlg.cb = sd_mount_confirm;
    dlg.on_poll = mount_state_poll;
    os_dialog_push(ctx, &dlg);
}

/* ==== 浏览文件 (融合弹窗) 状态与入口, 前置供 sd_main_key 使用 ==== */
#define FB_MAX 60
#define FB_BX 25      /* 弹窗左 */
#define FB_BY 25      /* 弹窗顶 */
#define FB_BW 350     /* 弹窗宽 (与存储菜单一致) */
#define FB_BH 250     /* 弹窗高 */
#define FB_RH 30      /* 行高 */
static EXT_RAM_BSS_ATTR char  s_fb_path[160] = "";          /* 相对根目录, ""= /sdcard */
static EXT_RAM_BSS_ATTR char  s_fb_names[FB_MAX][30];       /* 条目名 (不含分隔符) */
static EXT_RAM_BSS_ATTR bool  s_fb_isdir[FB_MAX];
static EXT_RAM_BSS_ATTR int   s_fb_count = 0;
static EXT_RAM_BSS_ATTR char  s_fb_delpath[180];            /* 待删除文件完整路径 */
static void open_fb_dlg(ui_ctx_t *ctx);

/* ===== NVS 备份 / 恢复 (整区备份到 TF) ===== */
#define NVS_BAK_DIR "/sdcard/backup"
static EXT_RAM_BSS_ATTR char s_bak_paths[8][140];
static EXT_RAM_BSS_ATTR char s_bak_pend[140];   /* 待覆盖/删除的备份路径 */
static void nvs_del_cb(ui_ctx_t*ctx,int result,void*ud);
static void nvs_restore_do(ui_ctx_t*ctx,int result,void*ud);
static void nvs_restore_list(ui_ctx_t*ctx);

static void nvs_backup_to_tf(ui_ctx_t *ctx){
    const esp_partition_t *p=esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, "nvs");
    if(!p){ os_dialog_toast(ctx,"\xe6\x97\xa0NVS\xe5\x88\x86\xe5\x8c\xba"); return; }
    if(!sd_is_mounted()){ os_dialog_toast(ctx,"TF\xe6\x9c\xaa\xe6\x8c\x82\xe8\xbd\xbd"); return; }
    mkdir(NVS_BAK_DIR,0755);
    uint8_t *buf=heap_caps_malloc(p->size, MALLOC_CAP_SPIRAM);
    if(!buf){ os_dialog_toast(ctx,"\xe5\x86\x85\xe5\xad\x98\xe4\xb8\x8d\xe8\xb6\xb3"); return; }
    char path[140]; time_t t=time(NULL);
    snprintf(path,sizeof(path),"%s/nvs_%ld.bin",NVS_BAK_DIR,(long)t);
    if(esp_partition_read(p,0,buf,p->size)==ESP_OK){
        FILE *f=fopen(path,"wb");
        if(f){ size_t w=fwrite(buf,1,p->size,f); fclose(f);
               os_dialog_toast(ctx, w==p->size?"\xe5\xb7\xb2\xe5\xa4\x87\xe4\xbb\xbd":"\xe5\x86\x99\xe5\x85\xa5\xe5\xa4\xb1\xe8\xb4\xa5"); }
        else os_dialog_toast(ctx,"\xe6\x89\x93\xe5\xbc\x80\xe6\x96\x87\xe4\xbb\xb6\xe5\xa4\xb1\xe8\xb4\xa5");
    } else os_dialog_toast(ctx,"\xe8\xaf\xbbNVS\xe5\xa4\xb1\xe8\xb4\xa5");
    heap_caps_free(buf);
}
static void nvs_restore_do(ui_ctx_t*ctx,int result,void*ud){
    (void)ud; if(result!=0) return;   /* 取消 → 不动 */
    const esp_partition_t *p=esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, "nvs");
    if(!p){ os_dialog_toast(ctx,"\xe6\x97\xa0NVS\xe5\x88\x86\xe5\x8c\xba"); return; }
    FILE *f=fopen(s_bak_pend,"rb"); if(!f){ os_dialog_toast(ctx,"\xe6\x89\x93\xe5\xbc\x80\xe5\xa4\xb1\xe8\xb4\xa5"); return; }
    uint8_t *buf=heap_caps_malloc(p->size, MALLOC_CAP_SPIRAM); if(!buf){ fclose(f); os_dialog_toast(ctx,"\xe5\x86\x85\xe5\xad\x98\xe4\xb8\x8d\xe8\xb6\xb3"); return; }
    size_t rd=fread(buf,1,p->size,f); fclose(f);
    esp_partition_erase_range(p,0,p->size);
    esp_partition_write(p,0,buf,rd);
    heap_caps_free(buf);
    os_dialog_toast_ms(ctx,"\xe5\xb7\xb2\xe6\x81\xa2\xe5\xa4\x8d,\xe9\x87\x8d\xe5\x90\xaf\xe7\x94\x9f\xe6\x95\x88",1500); /* 已恢复,重启生效 */
    esp_restart();
}
static void nvs_restore_confirm2(ui_ctx_t*ctx,int result,void*ud);
static void nvs_restore_sel(ui_ctx_t*ctx,int result,void*ud){
    (void)ud; if(result<0) return;
    snprintf(s_bak_pend,sizeof(s_bak_pend),"%s",s_bak_paths[result]);
    os_dialog_confirm_ex(ctx,"\xe7\xa1\xae\xe5\xae\x9a\xe6\x81\xa2\xe5\xa4\x8d\xe8\xaf\xa5\xe5\xa4\x87\xe4\xbb\xbd?",0,-1,nvs_restore_confirm2,NULL); /* ①确认还原该备份? */
}
static void nvs_restore_confirm2(ui_ctx_t*ctx,int result,void*ud){
    (void)ud; if(result!=0) return;
    os_dialog_confirm_ex(ctx,"\xe6\x9c\x80\xe5\x90\x8e\xe7\xa1\xae\xe8\xae\xa4\xe8\xa6\x86\xe7\x9b\x96NVS\xe5\xb9\xb6\xe9\x87\x8d\xe5\x90\xaf?",0,-1,nvs_restore_do,NULL); /* ②最后确认覆盖NVS并重启? */
}
static void nvs_del_cb(ui_ctx_t*ctx,int result,void*ud){
    (void)ud; if(result!=0) return;
    remove(s_bak_pend);
    os_dialog_toast(ctx,"\xe5\xb7\xb2\xe5\x88\xa0\xe9\x99\xa4");  /* 已删除 */
    os_dialog_pop(ctx);
    nvs_restore_list(ctx);   /* 刷新列表 */
}
static bool nvs_restore_key(ui_ctx_t*ctx,os_dlg_stack_t*d,os_action_t a,void*ud){
    (void)ud;
    if(a==OS_ACTION_LONG_PRESS){   /* 长按备份项 → 确认删除 */
        if(d->sel<0 || d->sel>=d->count) return false;
        snprintf(s_bak_pend,sizeof(s_bak_pend),"%s",s_bak_paths[d->sel]);
        os_dialog_confirm_ex(ctx,"\xe5\x88\xa0\xe9\x99\xa4\xe8\xaf\xa5\xe5\xa4\x87\xe4\xbb\xbd?",0,-1,nvs_del_cb,NULL); /* 删除该备份? */
        return true;
    }
    return false;
}
static void nvs_restore_list(ui_ctx_t*ctx){
    if(!sd_is_mounted()){ os_dialog_toast(ctx,"TF\xe6\x9c\xaa\xe6\x8c\x82\xe8\xbd\xbd"); return; }
    os_dlg_stack_t dlg; memset(&dlg,0,sizeof(dlg));
    DIR *d=opendir(NVS_BAK_DIR); int n=0;
    if(d){ struct dirent *e;
        while((e=readdir(d)) && n<8){
            if(!strstr(e->d_name,".bin")) continue;
            snprintf(s_bak_paths[n],sizeof(s_bak_paths[0]),"%s/%s",NVS_BAK_DIR,e->d_name);
            snprintf(dlg.items[n],sizeof(dlg.items[0]),"%s",e->d_name); n++;
        }
        closedir(d);
    }
    if(!n){ os_dialog_toast(ctx,"\xe6\x97\xa0\xe5\xa4\x87\xe4\xbb\xbd\xe6\x96\x87\xe4\xbb\xb6"); return; }
    dlg.count=n; dlg.sel=0; dlg.cb=nvs_restore_sel; dlg.on_key=nvs_restore_key;
    os_dialog_push(ctx,&dlg);
}

static void bak_restore_cb(ui_ctx_t*ctx,int result,void*ud){
    (void)ud;
    if(result==0) nvs_backup_to_tf(ctx);       /* ① 备份NVS到TF */
    else if(result==1) nvs_restore_list(ctx);  /* ② 还原备份文件 */
}
/* 备份还原 二级菜单 */
static void open_sd_bak_restore(ui_ctx_t*ctx){
    static const char *ptrs[2]={ "\xe5\xa4\x87\xe4\xbb\xbdNVS\xe5\x88\xb0TF",   /* 备份NVS到TF */
                                 "\xe8\xbf\x98\xe5\x8e\x9f\xe5\xa4\x87\xe4\xbb\xbd\xe6\x96\x87\xe4\xbb\xb6" }; /* 还原备份文件 */
    os_dialog_list(ctx,"",ptrs,2,0,bak_restore_cb,NULL);
}

static bool sd_main_key(ui_ctx_t *ctx, os_dlg_stack_t *d, os_action_t a, void *ud) {
    (void)ud;
    if (a != OS_ACTION_CONFIRM) return false;   /* UP/DOWN/BACK 走默认 */
    if (d->sel >= d->count) return false;       /* 底部返回行 → 默认关闭 */
    switch (d->sel) {
    case 0:  /* 浏览文件 → 融合进同一弹窗: 快扫/目录进退/长按删除 */
        s_fb_path[0] = '\0';   /* 每次进入从根开始 */
        open_fb_dlg(ctx);
        break;
    case 1:  /* 挂载到电脑 (3.3: 卸载VFS + USB MSC; 退出重启恢复) */
        open_sd_mount(ctx);
        break;
    case 2:  /* 格式化TF卡 */
        open_sd_format(ctx);
        break;
    case 3:  /* 存储信息 */
        open_sd_info(ctx);
        break;
    case 4:  /* 备份还原 */
        open_sd_bak_restore(ctx);
        break;
    default:
        break;
    }
    return true;   /* 不关闭主弹窗, 子弹窗压栈 */
}

/* ==== 浏览文件 (融合进同一弹窗): 快扫 / 进目录 / 长按或点×删除 ==== */
static bool fb_at_root(void) { return s_fb_path[0] == '\0'; }
static const char *fb_dir_path(void) {
    if (fb_at_root()) return "/sdcard";
    static EXT_RAM_BSS_ATTR char buf[160];
    snprintf(buf, sizeof(buf), "/sdcard/%s", s_fb_path);
    return buf;
}
static void fb_fullpath(char *out, int cap, const char *name) {
    snprintf(out, cap, "%s/%s", fb_dir_path(), name);
}
/* 标题截断: title[32] 很小, 按 UTF-8 边界截短当前路径放标题栏 */
static const char *fb_fit_title(const char *p) {
    static EXT_RAM_BSS_ATTR char buf[32];
    int i = 0;
    while (p[i] && i < 30) {
        unsigned char c = (unsigned char)p[i];
        int len = (c < 0x80) ? 1 : (c < 0xE0) ? 2 : (c < 0xF0) ? 3 : (c < 0xF8) ? 4 : 1;
        if (i + len > 30) break;
        for (int k = 0; k < len; k++) buf[i + k] = p[i + k];
        i += len;
    }
    buf[i] = 0;
    return buf;
}
/* 文件类型 (用于行首 【xxx】 标签) */
typedef enum { FBT_DIR, FBT_IMG, FBT_MP3, FBT_VID, FBT_GAME, FBT_TXT, FBT_OTHER } fb_type_t;
static EXT_RAM_BSS_ATTR fb_type_t s_fb_type[FB_MAX];
static EXT_RAM_BSS_ATTR int  s_fb_sel = 0;
static EXT_RAM_BSS_ATTR int  s_fb_off = 0;         /* 滚动像素偏移 (跟手拖动) */
static EXT_RAM_BSS_ATTR int  s_fb_drag = -1, s_fb_drag_off0 = 0;

/* 隐藏文件 / Windows 系统垃圾 (不显示) */
static bool fb_junk(const char *n) {
    if (n[0] == '.') return true;                       /* 点文件含 .DS_Store/.Trashes/._* 等 */
    int l = (int)strlen(n);
    if (l > 4 && !strcasecmp(n + l - 4, ".tmp")) return true;
    if (l > 5 && !strcasecmp(n + l - 5, ".temp")) return true;
    if (n[0] == '~' && n[1] == '$') return true;        /* office/word 临时文件 */
    if (!strcasecmp(n, "Thumbs.db")) return true;
    if (!strcasecmp(n, "desktop.ini")) return true;
    if (!strcasecmp(n, "System Volume Information")) return true;
    if (!strcasecmp(n, "$RECYCLE.BIN")) return true;
    if (!strcasecmp(n, "RECYCLER")) return true;
    return false;
}
static fb_type_t fb_type_of(const char *n, bool dir) {
    if (dir) return FBT_DIR;
    const char *e = strrchr(n, '.');
    if (!e || !e[1]) return FBT_OTHER;
    e++;
    if (!strcasecmp(e,"jpg")||!strcasecmp(e,"jpeg")||!strcasecmp(e,"png")||
        !strcasecmp(e,"bmp")||!strcasecmp(e,"gif")||!strcasecmp(e,"webp")) return FBT_IMG;
    if (!strcasecmp(e,"mp3")||!strcasecmp(e,"wav")||!strcasecmp(e,"ogg")||
        !strcasecmp(e,"flac")||!strcasecmp(e,"aac")||!strcasecmp(e,"m4a")) return FBT_MP3;
    if (!strcasecmp(e,"mp4")||!strcasecmp(e,"avi")||!strcasecmp(e,"mkv")||
        !strcasecmp(e,"mov")||!strcasecmp(e,"flv")) return FBT_VID;
    if (!strcasecmp(e,"gb")||!strcasecmp(e,"gbc")||!strcasecmp(e,"nes")||
        !strcasecmp(e,"fds")||!strcasecmp(e,"swc")||!strcasecmp(e,"ab")||
        !strcasecmp(e,"lav")) return FBT_GAME;
    if (!strcasecmp(e,"txt")||!strcasecmp(e,"md")||!strcasecmp(e,"log")||
        !strcasecmp(e,"ini")||!strcasecmp(e,"csv")) return FBT_TXT;
    return FBT_OTHER;
}
static const char *fb_type_label(fb_type_t t) {
    switch (t) {
    case FBT_DIR:  return "\xe3\x80\x90\xe6\x96\x87\xe4\xbb\xb6\xe5\xa4\xb9\xe3\x80\x91"; /* 【文件夹】 */
    case FBT_IMG:  return "\xe3\x80\x90\xe5\x9b\xbe\xe7\x89\x87\xe3\x80\x91";             /* 【图片】 */
    case FBT_MP3:  return "\xe3\x80\x90MP3\xe3\x80\x91";
    case FBT_VID:  return "\xe3\x80\x90\xe8\xa7\x86\xe9\xa2\x91\xe3\x80\x91";             /* 【视频】 */
    case FBT_GAME: return "\xe3\x80\x90\xe6\xb8\xb8\xe6\x88\x8f\xe3\x80\x91";             /* 【游戏】 */
    case FBT_TXT:  return "\xe3\x80\x90TXT\xe3\x80\x91";
    default:       return "\xe3\x80\x90\xe6\x96\x87\xe4\xbb\xb6\xe3\x80\x91";             /* 【文件】 */
    }
}
/* 快扫 + 隐藏 + 类型: 用 d_type 判断目录 (FAT 通常直接给), 未识别才 stat */
static void fb_scan(void) {
    s_fb_count = 0;
    DIR *d = opendir(fb_dir_path());
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && s_fb_count < FB_MAX) {
        if (fb_junk(e->d_name)) continue;
        bool isdir = (e->d_type == DT_DIR);
        if (e->d_type == DT_UNKNOWN) {
            char p[180];
            fb_fullpath(p, sizeof(p), e->d_name);
            struct stat st;
            if (stat(p, &st) == 0) isdir = S_ISDIR(st.st_mode);
        }
        memcpy(s_fb_names[s_fb_count], e->d_name, 29);
        s_fb_names[s_fb_count][29] = 0;
        s_fb_isdir[s_fb_count] = isdir;
        s_fb_type[s_fb_count] = fb_type_of(e->d_name, isdir);
        s_fb_count++;
    }
    closedir(d);
    /* 目录在前, 各自字母序 (同步类型) */
    for (int i = 1; i < s_fb_count; i++)
        for (int j = i; j > 0; j--) {
            int c = (s_fb_isdir[j - 1] == s_fb_isdir[j])
                        ? strcmp(s_fb_names[j], s_fb_names[j - 1])
                        : (s_fb_isdir[j - 1] ? -1 : 1);
            if (c < 0) {
                char tn[30]; bool tf; fb_type_t tt;
                strcpy(tn, s_fb_names[j - 1]); tf = s_fb_isdir[j - 1]; tt = s_fb_type[j - 1];
                strcpy(s_fb_names[j - 1], s_fb_names[j]); s_fb_isdir[j - 1] = s_fb_isdir[j]; s_fb_type[j - 1] = s_fb_type[j];
                strcpy(s_fb_names[j], tn); s_fb_isdir[j] = tf; s_fb_type[j] = tt;
            } else break;
        }
}
static void fb_enter_dir(int ei) {
    if (fb_at_root()) strncpy(s_fb_path, s_fb_names[ei], sizeof(s_fb_path) - 1);
    else if (strlen(s_fb_path) + 1 + strlen(s_fb_names[ei]) < sizeof(s_fb_path))
        snprintf(s_fb_path + strlen(s_fb_path), sizeof(s_fb_path) - strlen(s_fb_path),
                 "/%s", s_fb_names[ei]);
}
static void fb_go_up(void) {
    char *sl = strrchr(s_fb_path, '/');
    if (sl) *sl = '\0';
    else s_fb_path[0] = '\0';
}
static void fb_delete_cb(ui_ctx_t *ctx, int result, void *ud) {
    (void)ud;
    if (result != 0) return;   /* 取消: 只关删除确认, 回浏览弹窗 */
    if (s_fb_delpath[0]) remove(s_fb_delpath);
    os_dialog_pop(ctx);        /* 关删除确认 */
    os_dialog_pop(ctx);        /* 关当前浏览层 */
    open_fb_dlg(ctx);
    os_dialog_toast(ctx, "\xe5\xb7\xb2\xe5\x88\xa0\xe9\x99\xa4"); /* 已删除 */
}
/* 名称截断: 按 UTF-8 边界截到不超过 maxw 像素 */
static const char *fb_fit_name(const char *s, int maxw) {
    static EXT_RAM_BSS_ATTR char buf[48];
    int i = 0;
    while (s[i] && i < (int)sizeof(buf) - 6) {
        unsigned char c = (unsigned char)s[i];
        int len = (c < 0x80) ? 1 : (c < 0xE0) ? 2 : (c < 0xF0) ? 3 : (c < 0xF8) ? 4 : 1;
        if (i + len > (int)sizeof(buf) - 7) break;
        memcpy(buf + i, s + i, (size_t)len);
        buf[i + len] = 0;
        if (text_width(buf) > maxw) { buf[i] = 0; break; }
        i += len;
    }
    buf[i] = 0;
    return buf;
}
/* 让选中行跟随滚动: 框在可视窗内移动, 越界才滚动 */
static void fb_follow(void) {
    const int rtop = FB_BY + 30, rh = FB_RH, rbot = FB_BY + FB_BH - 32;
    int maxvis = (rbot - rtop) / rh; if (maxvis < 1) maxvis = 1;
    int first = s_fb_off / rh;
    if (s_fb_sel < first) first = s_fb_sel;
    else if (s_fb_sel >= first + maxvis) first = s_fb_sel - maxvis + 1;
    if (first < 0) first = 0;
    s_fb_off = first * rh;
    int maxoff = s_fb_count * rh - (rbot - rtop); if (maxoff < 0) maxoff = 0;
    if (s_fb_off > maxoff) s_fb_off = maxoff;
}

/* 自定义整层渲染: 左对齐 + 类型标签 + 选中黑条 + 底部返回行 */
static bool fb_render(ui_ctx_t *ctx, os_dlg_stack_t *d, void *ud) {
    (void)d; (void)ud;
    st7305_handle_t *lcd = ctx->lcd;
    if (!lcd) return true;
    int bx = FB_BX, by = FB_BY, bw = FB_BW, bh = FB_BH;
    fill_rect(lcd, bx, by, bx + bw - 1, by + bh - 1, ST7305_COLOR_WHITE);
    for (int k = 0; k < 2; k++) {
        draw_hline(lcd, bx + k, bx + bw - 1 - k, by + k, ST7305_COLOR_BLACK);
        draw_hline(lcd, bx + k, bx + bw - 1 - k, by + bh - 1 - k, ST7305_COLOR_BLACK);
        draw_vline(lcd, bx + k, by + k, by + bh - 1 - k, ST7305_COLOR_BLACK);
        draw_vline(lcd, bx + bw - 1 - k, by + k, by + bh - 1 - k, ST7305_COLOR_BLACK);
    }
    /* 无标题名: 列表由弹窗顶部开始 (同标准选择菜单), 文件项左对齐 */
    if (s_fb_count == 0) {
        draw_text(lcd, bx + 12, by + 8, "\xe7\xa9\xba\xe7\x9b\xae\xe5\xbd\x95", false); /* 空目录 */
    }
    int rtop = by + 4, rbot = by + bh - 30, rh = FB_RH;
    int view = rbot - rtop;
    int off = s_fb_off; if (off < 0) off = 0;
    int first = off / rh, mod = off % rh;
    int maxvis = view / rh; if (maxvis < 1) maxvis = 1;
    for (int i = 0; i <= maxvis; i++) {
        int idx = first + i;
        if (idx >= s_fb_count) break;
        int y = rtop + i * rh - mod;
        if (y + rh - 2 < by + 30) continue;
        bool sel = (idx == s_fb_sel);
        if (sel) fill_rect(lcd, bx + 3, y, bx + bw - 4, y + rh - 2, ST7305_COLOR_BLACK);
        int ty = y + (rh - 24) / 2;
        const char *tag = fb_type_label(s_fb_type[idx]);
        draw_text(lcd, bx + 6, ty, tag, sel);
        int tw = text_width(tag);
        int maxw = (bx + bw - 6) - (bx + 6 + tw + 4); if (maxw < 12) maxw = 12;
        draw_text(lcd, bx + 6 + tw + 4, ty, fb_fit_name(s_fb_names[idx], maxw), sel);
    }
    /* 右侧滚动条 (内容超出可见区, 同标准选择菜单) */
    if (s_fb_count > maxvis) {
        int bxr = bx + bw - 3;
        int bar_y0 = rtop, bar_y1 = rtop + maxvis * rh - 1;
        draw_vline(lcd, bxr, bar_y0, bar_y1, ST7305_COLOR_BLACK);
        int track = bar_y1 - bar_y0;
        int th = track * maxvis / s_fb_count; if (th < 4) th = 4;
        int maxoff = s_fb_count * rh - (rbot - rtop); if (maxoff < 0) maxoff = 0;
        int py = maxoff > 0 ? (int)((long)s_fb_off * (track - th) / maxoff) : 0;
        if (py < 0) py = 0;
        fill_rect(lcd, bxr, bar_y0 + py, bxr, bar_y0 + py + th - 1, ST7305_COLOR_BLACK);
    }

    /* 底部"返回/退出"行 */
    draw_hline(lcd, bx, bx + bw - 1, by + bh - 30, ST7305_COLOR_BLACK);
    draw_text(lcd, bx + 10, by + bh - 26,
              fb_at_root() ? "\xe9\x80\x80\xe5\x87\xba" : "\xe8\xbf\x94\xe5\x9b\x9e\xe4\xb8\x8a\xe7\xba\xa7", false);
    return true;
}
/* 按键: 上下移动 / 确认进目录 / 长按删除 / 返回上级 */
static bool fb_key(ui_ctx_t *ctx, os_dlg_stack_t *d, os_action_t a, void *ud) {
    (void)d; (void)ud;
    if (a == OS_ACTION_UP) {
        if (s_fb_sel > 0) { s_fb_sel--; fb_follow(); }
        ctx->needs_redraw = true; return true;
    }
    if (a == OS_ACTION_DOWN) {
        if (s_fb_sel + 1 < s_fb_count) { s_fb_sel++; fb_follow(); }
        ctx->needs_redraw = true; return true;
    }
    if (a == OS_ACTION_CONFIRM) {
        int ei = s_fb_sel;
        if (ei >= 0 && ei < s_fb_count && s_fb_isdir[ei]) {
            fb_enter_dir(ei);
            os_dialog_pop(ctx); open_fb_dlg(ctx);
        }
        return true;   /* 文件项不打开 */
    }
    if (a == OS_ACTION_LONG_PRESS) {    /* 长按确认键 → 弹删除确认 */
        int ei = s_fb_sel;
        if (ei >= 0 && ei < s_fb_count && !s_fb_isdir[ei]) {
            fb_fullpath(s_fb_delpath, sizeof(s_fb_delpath), s_fb_names[ei]);
            os_dialog_confirm_ex(ctx,
                "\xe5\x88\xa0\xe9\x99\xa4\xe8\xbf\x99\xe4\xb8\xaa\xe6\x96\x87\xe4\xbb\xb6?", /* 删除这个文件? */
                0, 0, fb_delete_cb, NULL);
        }
        return true;
    }
    if (a == OS_ACTION_BACK) {          /* 返回 = 上级 (根=退出) */
        if (fb_at_root()) { os_dialog_pop(ctx); return true; }
        fb_go_up();
        os_dialog_pop(ctx); open_fb_dlg(ctx);
        return true;
    }
    return false;
}
/* 触摸: 点行→选中(文件夹即进入), 点底部行→返回上级 */
static bool fb_touch(ui_ctx_t *ctx, os_dlg_stack_t *d, int x, int y, void *ud) {
    (void)d; (void)ud;
    int bx = FB_BX, by = FB_BY, bw = FB_BW, bh = FB_BH;
    if (x < bx || x > bx + bw - 1 || y < by || y > by + bh - 1) return false;
    if (y >= by + bh - 30) {           /* 底部返回行 */
        if (fb_at_root()) { os_dialog_pop(ctx); }
        else { fb_go_up(); os_dialog_pop(ctx); open_fb_dlg(ctx); }
        return true;
    }
    int rh = FB_RH;
    int idx = (s_fb_off + (y - (by + 32))) / rh;
    if (idx < 0 || idx >= s_fb_count) return true;
    s_fb_sel = idx;
    ctx->needs_redraw = true;
    if (s_fb_isdir[idx]) { fb_enter_dir(idx); os_dialog_pop(ctx); open_fb_dlg(ctx); }
    return true;
}
/* 每帧: 触屏拖动滚动 (跟手) */
static void fb_poll(ui_ctx_t *ctx, os_dlg_stack_t *d, void *ud) {
    (void)d; (void)ud;
    int tx, ty;
    bool down = input_get_touch_pos(&tx, &ty);
    int rtop = FB_BY + 32, rbot = FB_BY + FB_BH - 32;
    int maxoff = s_fb_count * FB_RH - (rbot - rtop); if (maxoff < 0) maxoff = 0;
    if (down) {
        if (tx < FB_BX || ty < rtop || ty >= rbot) { if (s_fb_drag == -1) s_fb_drag = -999; return; }
        if (s_fb_drag == -1) { s_fb_drag = ty; s_fb_drag_off0 = s_fb_off; return; }
        if (s_fb_drag == -999) return;
        int no = s_fb_drag_off0 - (ty - s_fb_drag);
        if (no < 0) no = 0;
        if (no > maxoff) no = maxoff;
        if (no != s_fb_off) { s_fb_off = no; if (ctx) ctx->needs_redraw = true; }
    } else {
        s_fb_drag = -1;
    }
}
static void open_fb_dlg(ui_ctx_t *ctx) {
    fb_scan();
    if (s_fb_sel >= s_fb_count) s_fb_sel = 0;
    s_fb_off = 0;
    os_dlg_stack_t dlg;
    memset(&dlg, 0, sizeof(dlg));
    dlg.count = 0;               /* 自定义整层渲染, 全自管理 */
    dlg.on_render = fb_render;
    dlg.on_key = fb_key;
    dlg.on_touch = fb_touch;
    dlg.on_poll = fb_poll;
    dlg.no_footer = true;
    os_dialog_push(ctx, &dlg);
}

static void open_sd_dlg(ui_ctx_t *ctx) {
    static const char *const items[5] = {
        "\xe6\xb5\x8f\xe8\xa7\x88\xe6\x96\x87\xe4\xbb\xb6",     /* 浏览文件 */
        "\xe6\x8c\x82\xe8\xbd\xbd\xe5\x88\xb0\xe7\x94\xb5\xe8\x84\x91", /* 挂载到电脑 */
        "\xe6\xa0\xbc\xe5\xbc\x8f\xe5\x8c\x96TF\xe5\x8d\xa1",    /* 格式化TF卡 */
        "\xe5\xad\x98\xe5\x82\xa8\xe4\xbf\xa1\xe6\x81\xaf",     /* 存储信息 */
        "\xe5\xa4\x87\xe4\xbb\xbd\xe8\xbf\x98\xe5\x8e\x9f",      /* 备份还原 */
    };
    os_dlg_stack_t dlg;
    memset(&dlg, 0, sizeof(dlg));
    dlg.count = 5;
    for (int i = 0; i < 5; i++)
        snprintf(dlg.items[i], sizeof(dlg.items[i]), "%s", items[i]);
    dlg.sel = 0;
    dlg.on_key = sd_main_key;
    dlg.cb = sd_close;   /* 返回关闭主弹窗 → 退出存储页 */
    os_dialog_push(ctx, &dlg);
}

static void p_sto_enter(ui_ctx_t *ctx) {
    open_sd_dlg(ctx);
}

/* 退出存储页: 若仍在 USB MSC 挂载, 停止并重挂 SD (不重启, 保持串口正常) */
static void p_sto_exit(ui_ctx_t *ctx) {
    (void)ctx;
    if (usbh_msc_is_running()) {
        ESP_LOGI(TAG, "退出存储页: 停止 USB MSC 并重挂 SD (不重启)");
        usbh_msc_stop();
        sd_remount_vfs_from_card();
    }
}

static const os_module_t s_mod_storage = {
    .name       = "storage",
    .page_id    = OS_PAGE_STORAGE,
    .on_enter   = p_sto_enter,
    .on_exit    = p_sto_exit,
    .fullscreen = false,
};

void os_page_storage_register(void) { os_register(&s_mod_storage); }
