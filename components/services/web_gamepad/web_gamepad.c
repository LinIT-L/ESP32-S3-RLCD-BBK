/* V1.0.67: WiFi 网页模拟手柄 (AP 热点 + 手机浏览器虚拟手柄)
 * 移植自 tigerxu255-lgtm/esp32-s3-rlcd-gb-emulator 的 web_gamepad,
 * 但 ESP-IDF v5.5 已移除 esp_http_server 的 WebSocket, 改为普通 HTTP POST 传按键状态. */
#include "web_gamepad.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board_battery.h"
#include "esp_check.h"
#include "esp_http_server.h"   /* 只是 httpd_req_t/httpd_uri_t 类型（handler 签名） */
#include "esp_timer.h"         /* 按键状态存活时间戳 */
#include "esp_log.h"
#include "svc_nvs.h"
#include "sys/time.h"
#include "web_core.h"          /* ★ 2026-11-01: 统一 HTTP 服务 */
#include "wifi_manager.h"      /* ★ AP 热点与 STA 都由它统一管 */

static const char *TAG = "web_gamepad";

/* V1.0.67: 热点名 BBK-WIFI-handle, 无密码开放; 网关 IP 用 8.8.8.8 (简单好记),
 * 连接后 captive portal 自动弹出网页. */
#define WEB_GAMEPAD_AP_SSID        "BBK-WIFI-handle"
#define WEB_GAMEPAD_AP_PASSWORD    ""
#define WEB_GAMEPAD_AP_IP_ADDR     8, 8, 8, 8
#define WEB_GAMEPAD_AP_CHANNEL     6
#define WEB_GAMEPAD_AP_MAX_CONN    4

static bool s_started = false;
static volatile uint8_t s_joypad_state = 0xFF;

/* ★ 2026-11-28: 状态超时复位 + 请求序号, 解决两个真实缺陷 ——
 *   ① 手机松手丢包 / 切后台 / 走出热点范围 ⇒ 最后一个状态被永久保持, 游戏里角色
 *      一直朝某方向走。此前只有 web_gamepad_stop() 才清 0xFF。
 *      注意: 网页只在**状态变化**时 POST, 所以服务端不能只靠"多久没收到请求"判松手,
 *      否则长按不放会被判成松手。为此网页端在按住期间每 150ms 心跳重发同一状态
 *      (见 s_index_html 的 beatSync), 服务端超时阈值 500ms(容 3 次丢包)。
 *   ② 快速连按两键时并发 fetch 可达乱序 ⇒ 旧状态盖掉新状态(按了 A 却卡住)。
 *      网页端带 seq 递增, 服务端只接受"不比上次旧"的序号; 心跳会周期性补发,
 *      所以即使偶发丢一帧也能在 150ms 内自愈。 */
#define JOYPAD_STALE_MS   700      /* 超过此时间无任何上报 ⇒ 视为全部松手 */
/* ★ 2026-11-28: 按下态最短保持 —— 手指快速点一下时, "按下"和"松开"两个 POST 可能
 *   只隔十几 ms, 而消费侧(input.c / 引擎)是**按帧采样 + 上升沿触发**, 采样周期
 *   20~30ms, 很容易整个漏掉 ⇒ 表现正是"按多次才生效一次"。
 *   这里把"新出现的按下位"在服务端至少保持 JOYPAD_LATCH_MS, 保证一定被采到。
 *   70ms 里的重复点击会合并为一次(人手正常连点间隔 >100ms, 不会误合并)。 */
#define JOYPAD_LATCH_MS   70
static volatile int64_t  s_joypad_last_us = 0;   /* 最近一次收到 POST 的时刻, 0=尚无 */
static volatile uint32_t s_joypad_seq     = 0;   /* 已接受的最大序号 */
static volatile uint8_t  s_press_latch    = 0;   /* 需要最短保持的按下位(低有效) */
static volatile int64_t  s_latch_until_us = 0;   /* 保持截止时刻 */

/* 网页手柄 HTML (方向键 + A/B/Start/Select, 按键用 fetch POST /joypad?state=N 上报) */
static const char s_index_html[] =
    "<!doctype html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no,viewport-fit=cover'>"
    "<title>ESP32-\xe6\xad\xa5\xe6\xad\xa5\xe9\xab\x98 \xe6\x89\x8b\xe6\x9f\x84</title>"
    "<style>"
    "*{box-sizing:border-box;-webkit-user-select:none;user-select:none;-webkit-touch-callout:none;touch-action:none}"
    "html,body{margin:0;width:100%;height:100%;overflow:hidden;position:fixed;inset:0;background:#111;color:#eee;font-family:Arial,sans-serif;overscroll-behavior:none}"
    ".wrap{position:relative;width:100%;height:100%}"
    ".btn{border:2px solid #555;background:#222;border-radius:16px;font-size:24px;font-weight:700;display:flex;align-items:center;justify-content:center;color:#eee}"
    ".btn.down{background:#eee;color:#111;border-color:#eee}"
    ".empty{visibility:hidden}"
    ".dpad{position:absolute;left:14px;bottom:100px;display:grid;grid-template-columns:54px 54px 54px;grid-template-rows:54px 54px 54px;gap:5px}"
    ".dpad .btn{border-radius:50%;font-size:20px}"
    ".stick{position:absolute;left:18px;bottom:96px;width:160px;height:160px;border-radius:50%;border:3px solid #555;background:#181818;display:none}"
    ".stick .knob{position:absolute;left:50%;top:50%;width:64px;height:64px;border-radius:50%;transform:translate(-50%,-50%);background:#333;border:2px solid #777}"
    ".stick.down{border-color:#eee}"
    ".stick.down .knob{background:#eee;border-color:#eee}"
    ".ab{position:absolute;right:14px;bottom:100px;width:150px;height:150px}"
    ".ab .btn{position:absolute;width:68px;height:68px;border-radius:50%;font-size:24px}"
    ".ab .a{right:6px;top:6px}"
    ".ab .b{left:6px;bottom:6px}"
    ".sys{position:absolute;bottom:18px;left:50%;transform:translateX(-50%);display:flex;gap:12px}"
    ".sys .btn{height:44px;padding:0 14px;font-size:15px;border-radius:22px}"
    ".sys .toggle{background:#333;border-color:#888}"
    "#st{position:absolute;left:50%;bottom:4px;transform:translateX(-50%);font-size:12px;color:#666}"
    "@media (orientation: landscape){"
    "/* V1.1.4: 放弃 CSS 自旋 (rotate 在竖屏锁定的浏览器不生效), 改为依赖设备浏览器原生横屏. */"
    "/* 横屏时控件随系统旋转自动适配: 方向键左 / AB 右 / SELECT,START 居中顶部 */"
    ".dpad{left:6%;top:50%;bottom:auto;transform:translateY(-50%);grid-template-columns:60px 60px 60px;grid-template-rows:60px 60px 60px;gap:8px}"
    ".dpad .btn{font-size:26px}"
    ".stick{left:8%;top:50%;bottom:auto;transform:translateY(-50%);width:180px;height:180px}"
    ".ab{right:6%;top:50%;bottom:auto;transform:translateY(-50%);width:170px;height:170px}"
    ".ab .btn{width:74px;height:74px;font-size:28px}"
    ".sys{top:6%;bottom:auto;left:50%;transform:translateX(-50%)}"
    ".btn{font-size:22px}"
    "}"
    "</style>"
    "<div class='wrap'>"
    "<div class='dpad' id='dpad'>"
    "<div class='empty'></div><div class='btn' data-b='6'>&#9650;</div><div class='empty'></div>"
    "<div class='btn' data-b='5'>&#9664;</div><div class='empty'></div><div class='btn' data-b='4'>&#9654;</div>"
    "<div class='empty'></div><div class='btn' data-b='7'>&#9660;</div><div class='empty'></div>"
    "</div>"
    "<div class='stick' id='stick'><div class='knob'></div></div>"
    "<div class='ab'><div class='btn a' data-b='0'>A</div><div class='btn b' data-b='1'>B</div></div>"
    "<div class='sys'>"
    "<div class='btn' data-b='2'>SELECT</div>"
    "<div class='btn' data-b='3'>START</div>"
    "<div class='btn toggle' id='toggle'>\xf0\x9f\x95\xb9</div>"
    "</div>"
    "</div><div id='st'>\xe5\xb7\xb2\xe8\xbf\x9e\xe6\x8e\xa5</div>"
    "<script>"
    "let state=255,pressed={},stickMode=false,stickId=null,seq=0,beat=null;"
    "const dpad=document.getElementById('dpad'),stick=document.getElementById('stick'),knob=stick.querySelector('.knob'),toggle=document.getElementById('toggle');"
    "function vib(){if(navigator.vibrate)navigator.vibrate(15)}"
    "let q=[],busy=false;"
    "function pump(){if(busy||!q.length)return;busy=true;fetch(q.shift(),{method:'POST',cache:'no-store'}).catch(()=>{}).finally(()=>{busy=false;pump();});}"
    /* 串行队列: 同一时刻只有一个 POST 在飞.
     *   此前并发 fetch ⇒ 连点时多个请求同时到达、乱序覆盖(表现为"按多次才生效一次");
     *   队列化后每次按键状态都按顺序送达, 且不会把 socket 打满(AP 上 httpd 是单任务,
     *   并发请求只会互相排队, 反而更慢). 队列上限 8, 溢出丢最旧的, 保证松手态一定能发出. */
    "function post(s){seq++;q.push('joypad?state='+s+'&seq='+seq);if(q.length>8)q.shift();pump();}"
    "function beatSync(){if(beat){clearInterval(beat);beat=null;}if(state!==255)beat=setInterval(()=>post(state),150);}"
    "function send(){let s=255;for(const id in pressed)s&=~(1<<(+id));if(s!==state){state=s;post(s);beatSync();}}"
    "document.querySelectorAll('.btn[data-b]').forEach(b=>{const id=b.dataset.b;"
    "b.addEventListener('pointerdown',e=>{e.preventDefault();vib();pressed[id]=1;b.classList.add('down');send();});"
    "b.addEventListener('pointerup',e=>{e.preventDefault();delete pressed[id];b.classList.remove('down');send();});"
    "b.addEventListener('pointercancel',e=>{delete pressed[id];b.classList.remove('down');send();});});"
    "toggle.addEventListener('pointerdown',e=>{e.preventDefault();vib();stickMode=!stickMode;dpad.style.display=stickMode?'none':'grid';stick.style.display=stickMode?'block':'none';toggle.textContent=stickMode?'\xe2\x9c\x9b':'\xf0\x9f\x95\xb9';['4','5','6','7'].forEach(k=>delete pressed[k]);resetStick();send();});"
    "function resetStick(){stickId=null;stick.classList.remove('down');knob.style.left='50%';knob.style.top='50%';}"
    "function stickBits(e){let r=stick.getBoundingClientRect(),cx=r.left+r.width/2,cy=r.top+r.height/2,rad=r.width/2;let dx=(e.clientX-cx)/rad,dy=(e.clientY-cy)/rad;let len=Math.hypot(dx,dy);if(len>1){dx/=len;dy/=len;len=1}knob.style.left=(50+dx*32)+'%';knob.style.top=(50+dy*32)+'%';['4','5','6','7'].forEach(k=>delete pressed[k]);if(len>.28){if(dx>.4)pressed['4']=4;else if(dx<-.4)pressed['5']=5;if(dy>.4)pressed['7']=7;else if(dy<-.4)pressed['6']=6}stick.classList.toggle('down',len>.28);send();}"
    "stick.addEventListener('pointerdown',e=>{e.preventDefault();vib();stick.setPointerCapture(e.pointerId);stickId=e.pointerId;stickBits(e);});"
    "stick.addEventListener('pointermove',e=>{e.preventDefault();if(stickId===e.pointerId)stickBits(e);});"
    "stick.addEventListener('pointerup',e=>{e.preventDefault();if(stickId===e.pointerId){resetStick();['4','5','6','7'].forEach(k=>delete pressed[k]);send();}});"
    "stick.addEventListener('pointercancel',e=>{if(stickId===e.pointerId){resetStick();['4','5','6','7'].forEach(k=>delete pressed[k]);send();}});"
    "window.addEventListener('blur',()=>{pressed={};document.querySelectorAll('.btn').forEach(b=>b.classList.remove('down'));resetStick();send();});"
    "</script></body></html>";

static esp_err_t web_gamepad_index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, s_index_html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t web_gamepad_joypad_handler(httpd_req_t *req)
{
    /* 读取 query 参数 state (0..255, 低电平有效 joypad 掩码) 与 seq (客户端递增序号) */
    char qs[64] = {0};
    if (httpd_req_get_url_query_str(req, qs, sizeof(qs)) == ESP_OK) {
        char val[8] = {0};
        if (httpd_query_key_value(qs, "state", val, sizeof(val)) == ESP_OK) {
            int s = atoi(val);
            char sv[12] = {0};
            bool has_seq = (httpd_query_key_value(qs, "seq", sv, sizeof(sv)) == ESP_OK);
            uint32_t seq = has_seq ? (uint32_t)strtoul(sv, NULL, 10) : 0u;
            /* 收到任何上报都刷新存活时间戳(含心跳), 交给 web_gamepad_get_joypad_state() 判超时 */
            s_joypad_last_us = esp_timer_get_time();
            if (s >= 0 && s <= 255) {
                /* 无 seq 的旧页面照旧接受; 有 seq 则丢弃"比已接受序号更旧"的包。
                 * 用有符号差比较, 天然处理 32 位回绕。 */
                if (!has_seq || (int32_t)(seq - s_joypad_seq) >= 0) {
                    uint8_t old = s_joypad_state;
                    uint8_t nw  = (uint8_t)s;
                    uint8_t newly = (uint8_t)(old & ~nw);   /* 本次新按下的位(1→0) */
                    if (newly) {
                        s_press_latch    |= newly;
                        s_latch_until_us  = esp_timer_get_time() + (int64_t)JOYPAD_LATCH_MS * 1000;
                    }
                    s_joypad_state = nw;
                    if (has_seq) s_joypad_seq = seq;
                } else {
                    ESP_LOGD(TAG, "丢弃乱序按键包 seq=%" PRIu32 " (已接受 %" PRIu32 ")",
                             seq, (uint32_t)s_joypad_seq);
                }
            }
        }
    }
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr(req, "ok");
    return ESP_OK;
}

static esp_err_t web_gamepad_battery_handler(httpd_req_t *req)
{
    board_battery_status_t status = {0};
    esp_err_t ret = board_battery_read_cached(&status);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    char response[80] = {0};
    if (ret != ESP_OK) {
        snprintf(response, sizeof(response), "{\"ok\":false}");
        return httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN);
    }
    snprintf(response, sizeof(response), "{\"ok\":true,\"mv\":%" PRIu32 ",\"percent\":%u}",
             status.voltage_mv, status.percent);
    return httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN);
}

/* ---------- 向 web_core 注册本应用的页面与接口 ----------
 * URI 写**相对路径**，由 web_core 拼成 /pad/...（不依赖前缀通配，见 web_core.h）。
 * is_default=true：访问 http://8.8.8.8/ 会被 302 到 /pad/，captive 门户照旧弹网页。 */
static const httpd_uri_t s_pad_uris[] = {
    { .uri = "/",        .method = HTTP_GET,  .handler = web_gamepad_index_handler,   .user_ctx = NULL },
    { .uri = "/joypad",  .method = HTTP_POST, .handler = web_gamepad_joypad_handler,  .user_ctx = NULL },
    { .uri = "/battery", .method = HTTP_GET,  .handler = web_gamepad_battery_handler, .user_ctx = NULL },
};
static const web_app_t s_pad_app = {
    .name       = "pad",
    .prefix     = "/pad",
    .uris       = s_pad_uris,
    .n_uris     = sizeof(s_pad_uris) / sizeof(s_pad_uris[0]),
    .need_auth  = false,   /* 手柄页保持"连上就能玩"，不加 PIN（与原体验一致） */
    .is_default = true,
};

/* 网页服务模式 (NVS "web_svc_mode"): 0=AP直连(自建热点) 1=局域网(WiFi共享).
 * 默认 1 (联网/局域网), 用户不需手动切; 仅当显式设 0 才走 AP. */
int web_gamepad_svc_mode(void) {
    int m = 1;   /* 默认联网模式 (局域网共享) */
    nvs_handle_t h;
    if (svc_nvs_open("web_svc", NVS_READONLY, &h) == ESP_OK) {
        int32_t v = 0;
        if (svc_nvs_get_i32(h, "mode", &v) == ESP_OK && v == 0) m = 0;
        svc_nvs_close(h);
    }
    return m;
}
void web_gamepad_svc_mode_set(int lan) {
    nvs_handle_t h;
    if (svc_nvs_open("web_svc", NVS_READWRITE, &h) == ESP_OK) {
        svc_nvs_set_i32(h, "mode", lan ? 1 : 0);
        svc_nvs_commit(h);
        svc_nvs_close(h);
    }
}

static esp_err_t web_gamepad_init_nvs(void)
{
    esp_err_t ret = svc_nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(svc_nvs_flash_erase(), TAG, "erase nvs failed");
        ret = svc_nvs_flash_init();
    }
    return ret;
}

static esp_err_t begin_ap(void);

esp_err_t web_gamepad_start(void)
{
    if (s_started) return ESP_OK;

    ESP_RETURN_ON_ERROR(web_gamepad_init_nvs(), TAG, "init nvs failed");

    /* ① 局域网模式：复用 wifi_manager 已连接的 STA，不碰射频 */
    if (web_gamepad_svc_mode() == 1) {
        if (!wifi_manager_is_connected()) {
            ESP_LOGD(TAG, "局域网模式需要设备先连接 WiFi (STA)");
            return ESP_ERR_INVALID_STATE;
        }
        char ip[20] = "-";
        wifi_manager_get_ip(ip, sizeof(ip));
        ESP_RETURN_ON_ERROR(board_battery_init(), TAG, "init battery monitor failed");
        ESP_RETURN_ON_ERROR(web_core_start(0), TAG, "web_core start failed");
        ESP_RETURN_ON_ERROR(web_core_register(&s_pad_app), TAG, "register pad failed");
        s_joypad_state = 0xFF;
        s_joypad_last_us = 0;
        s_joypad_seq = 0;
        s_press_latch = 0;
        s_latch_until_us = 0;
        s_started = true;
        ESP_LOGI(TAG, "网页手柄[局域网模式] http://%s/pad/ (同网设备访问)", ip);
        return ESP_OK;
    }

    /* ② AP 直连：热点交给 wifi_manager（内部 APSTA，不会踢掉已连接的 STA） */
    ESP_RETURN_ON_ERROR(wifi_manager_ap_start(WEB_GAMEPAD_AP_SSID, WEB_GAMEPAD_AP_PASSWORD,
                                              WEB_GAMEPAD_AP_CHANNEL, WEB_GAMEPAD_AP_MAX_CONN,
                                              true), TAG, "ap start failed");
    ESP_RETURN_ON_ERROR(board_battery_init(), TAG, "init battery monitor failed");

    esp_err_t e = web_core_start(0);            /* 0 = 默认端口 80 */
    if (e != ESP_OK) { wifi_manager_ap_stop(); return e; }
    e = web_core_register(&s_pad_app);
    if (e != ESP_OK) { web_core_stop(); wifi_manager_ap_stop(); return e; }

    s_joypad_state = 0xFF;
    s_joypad_last_us = 0;
    s_joypad_seq = 0;
    s_press_latch = 0;
    s_latch_until_us = 0;
    s_started = true;
    ESP_LOGI(TAG, "网页手柄已就绪: 热点 '%s'（无密码），连上后浏览器打开 http://8.8.8.8/pad/",
             WEB_GAMEPAD_AP_SSID);
    return ESP_OK;
}

void web_gamepad_stop(void)
{
    if (!s_started) return;
    s_started = false;
    s_joypad_state = 0xFF;
    s_joypad_last_us = 0;
    s_joypad_seq = 0;
    s_press_latch = 0;
    s_latch_until_us = 0;
    /* 只注销自己、只减自己的引用 —— 别人还在用同一个 server 时不会被关掉 */
    web_core_unregister(&s_pad_app);
    web_core_stop();
    /* 仅 AP 直连模式才关热点（局域网模式下射频归 wifi_manager 的 STA，动不得） */
    if (web_gamepad_svc_mode() == 0) wifi_manager_ap_stop();
    ESP_LOGI(TAG, "网页手柄已停止");
}

bool web_gamepad_is_running(void)
{
    return s_started;
}

uint8_t web_gamepad_get_joypad_state(void)
{
    /* ★ 2026-11-28: 超时复位 —— 网页端在按住期间每 150ms 心跳重发同一状态,
     *   静默超过 JOYPAD_STALE_MS 只可能是松手丢包 / 切后台 / 掉线 ⇒ 全部按"松手"
     *   返回 0xFF, 避免游戏角色一直朝一个方向走。读-改存在极小竞态(HTTP 任务
     *   同时写), 但下一心跳就会补上正确值, 不值得为它加锁(本函数在游戏帧里高频调用)。 */
    uint8_t st = s_joypad_state;
    int64_t t  = s_joypad_last_us;
    if (st != 0xFF && t != 0 && (esp_timer_get_time() - t) > (int64_t)JOYPAD_STALE_MS * 1000) {
        s_joypad_state = 0xFF;
        s_press_latch  = 0;          /* 已判离线, 未过期的按下保持一并作废 */
        ESP_LOGW(TAG, "按键状态超过 %dms 无上报 ⇒ 视为全部松手(防卡键)", JOYPAD_STALE_MS);
        st = 0xFF;
    }
    /* 新按下位在 JOYPAD_LATCH_MS 内强制保持为低, 保证按帧采样的消费侧一定读到一次 */
    uint8_t latch = s_press_latch;
    if (latch) {
        if (esp_timer_get_time() < s_latch_until_us) st &= (uint8_t)~latch;
        else                                          s_press_latch = 0;
    }
    return st;
}
