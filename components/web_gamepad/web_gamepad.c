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
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "dns_server.h"
#include "esp_wifi.h"
#include "lwip/ip4_addr.h"
#include "nvs_flash.h"
#include "sys/time.h"

static const char *TAG = "web_gamepad";

/* V1.0.67: 热点名 BBK-WIFI-handle, 无密码开放; 网关 IP 用 8.8.8.8 (简单好记),
 * 连接后 captive portal 自动弹出网页. */
#define WEB_GAMEPAD_AP_SSID        "BBK-WIFI-handle"
#define WEB_GAMEPAD_AP_PASSWORD    ""
#define WEB_GAMEPAD_AP_IP_ADDR     8, 8, 8, 8
#define WEB_GAMEPAD_AP_CHANNEL     6
#define WEB_GAMEPAD_AP_MAX_CONN    4

static httpd_handle_t s_server = NULL;
static dns_server_handle_t s_dns = NULL;
static bool s_started = false;
static volatile uint8_t s_joypad_state = 0xFF;

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
    "let state=255,pressed={},stickMode=false,stickId=null;"
    "const dpad=document.getElementById('dpad'),stick=document.getElementById('stick'),knob=stick.querySelector('.knob'),toggle=document.getElementById('toggle');"
    "function vib(){if(navigator.vibrate)navigator.vibrate(15)}"
    "function send(){let s=255;for(const id in pressed)s&=~(1<<(+id));if(s!==state){state=s;fetch('/joypad?state='+state,{method:'POST',cache:'no-store'}).catch(()=>{});}}"
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
    /* 读取 query 参数 state (0..255, 低电平有效 joypad 掩码) */
    char qs[64] = {0};
    if (httpd_req_get_url_query_str(req, qs, sizeof(qs)) == ESP_OK) {
        char val[8] = {0};
        if (httpd_query_key_value(qs, "state", val, sizeof(val)) == ESP_OK) {
            int s = atoi(val);
            if (s >= 0 && s <= 255) {
                s_joypad_state = (uint8_t)s;
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
    esp_err_t ret = board_battery_read(&status);

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

/* 捕获门户: 未匹配请求 302 重定向到手柄页 (绝对路径, iOS/Android 都认) */
static esp_err_t web_gamepad_captive_redirect(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://8.8.8.8/");
    httpd_resp_send(req, "Redirect to BBK gamepad", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t web_gamepad_start_http_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;
    config.stack_size = 4096;
    config.task_priority = 3;
    config.core_id = 0;
    /* V1.0.67: 关键! 默认 uri_match_fn=NULL 是精确匹配, catch-all 通配路径
     * 必须用 wildcard 匹配器才能命中任意路径, 否则 captive portal 永远 404 不弹网页. */
    config.uri_match_fn = httpd_uri_match_wildcard;

    ESP_RETURN_ON_ERROR(httpd_start(&s_server, &config), TAG, "start http server failed");

    const httpd_uri_t index_uri = {
        .uri = "/", .method = HTTP_GET, .handler = web_gamepad_index_handler, .user_ctx = NULL,
    };
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_server, &index_uri), TAG, "register index uri failed");

    const httpd_uri_t joypad_uri = {
        .uri = "/joypad", .method = HTTP_POST, .handler = web_gamepad_joypad_handler, .user_ctx = NULL,
    };
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_server, &joypad_uri), TAG, "register joypad uri failed");

    const httpd_uri_t battery_uri = {
        .uri = "/battery", .method = HTTP_GET, .handler = web_gamepad_battery_handler, .user_ctx = NULL,
    };
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_server, &battery_uri), TAG, "register battery uri failed");

    /* 捕获门户: 兜底所有未匹配的 GET 请求, 302 重定向到手柄页 */
    const httpd_uri_t catch_uri = {
        .uri = "/*", .method = HTTP_GET, .handler = web_gamepad_captive_redirect, .user_ctx = NULL,
    };
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_server, &catch_uri), TAG, "register catch uri failed");

    return ESP_OK;
}

static esp_err_t web_gamepad_init_nvs(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "erase nvs failed");
        ret = nvs_flash_init();
    }
    return ret;
}

esp_err_t web_gamepad_start(void)
{
    if (s_started) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(web_gamepad_init_nvs(), TAG, "init nvs failed");
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "init netif failed");

    esp_err_t ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_RETURN_ON_ERROR(ret, TAG, "create event loop failed");
    }

    /* V1.0.67: 防重复创建 (STA 可能已先初始化过 esp_wifi/esp_netif) */
    esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (ap_netif == NULL) {
        ap_netif = esp_netif_create_default_wifi_ap();
    }

    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    init_config.wifi_task_core_id = 0;
    esp_err_t wifi_init_ret = esp_wifi_init(&init_config);
    if (wifi_init_ret != ESP_OK && wifi_init_ret != ESP_ERR_INVALID_STATE) {
        ESP_RETURN_ON_ERROR(wifi_init_ret, TAG, "init wifi failed");
    }

    wifi_config_t ap_config = {
        .ap = {
            .ssid = WEB_GAMEPAD_AP_SSID,
            .password = WEB_GAMEPAD_AP_PASSWORD,
            .ssid_len = strlen(WEB_GAMEPAD_AP_SSID),
            .channel = WEB_GAMEPAD_AP_CHANNEL,
            .max_connection = WEB_GAMEPAD_AP_MAX_CONN,
            .authmode = WIFI_AUTH_OPEN,
            .pmf_cfg = { .required = false },
        },
    };

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_AP), TAG, "set wifi mode failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap_config), TAG, "set ap config failed");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start wifi failed");

    /* AP 网关 IP 设为 8.8.8.8 (简单好记), 浏览器访问 http://8.8.8.8 即打开手柄页 */
    esp_netif_ip_info_t ip_info = { 0 };
    IP4_ADDR(&ip_info.ip, 8, 8, 8, 8);
    IP4_ADDR(&ip_info.gw, 8, 8, 8, 8);
    IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);
    esp_netif_dhcps_stop(ap_netif);
    ESP_RETURN_ON_ERROR(esp_netif_set_ip_info(ap_netif, &ip_info), TAG, "set ap ip failed");

    /* DHCP captive portal URL: 手机连接后据此自动弹网页 */
    esp_netif_dhcps_option(ap_netif, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI,
                           "http://8.8.8.8/", strlen("http://8.8.8.8/"));
    esp_netif_dhcps_start(ap_netif);

    /* DNS 劫持: 所有域名解析到 8.8.8.8 */
    dns_server_config_t dns_cfg = DNS_SERVER_CONFIG_SINGLE("*", "WIFI_AP_DEF");
    s_dns = start_dns_server(&dns_cfg);

    ESP_RETURN_ON_ERROR(board_battery_init(), TAG, "init battery monitor failed");
    ESP_RETURN_ON_ERROR(web_gamepad_start_http_server(), TAG, "start web gamepad server failed");

    s_joypad_state = 0xFF;
    s_started = true;

    ESP_LOGI(TAG, "网页手柄已就绪: 热点 '%s' (无密码), 连上后浏览器自动打开 http://8.8.8.8",
             WEB_GAMEPAD_AP_SSID);
    return ESP_OK;
}

void web_gamepad_stop(void)
{
    if (!s_started) return;
    s_started = false;
    s_joypad_state = 0xFF;
    if (s_dns) {
        stop_dns_server(s_dns);
        s_dns = NULL;
    }
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }
    esp_wifi_stop();
    ESP_LOGI(TAG, "网页手柄已停止");
}

bool web_gamepad_is_running(void)
{
    return s_started;
}

uint8_t web_gamepad_get_joypad_state(void)
{
    return s_joypad_state;
}
