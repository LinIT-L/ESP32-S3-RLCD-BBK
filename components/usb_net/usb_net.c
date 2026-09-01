/**
 * @file usb_net.c
 * @brief USB 网卡共享 (USB-RNDIS 有线网卡) 模块.
 *
 * 场景: 电脑插 USB → 识别为 RNDIS 网卡 → 拿到 IP → 借设备联的 Wi-Fi 上网.
 *
 * 入口用 NVS 一次性标记 + 重启 (同 USB-BT, 共享 USB PHY 只能一个身份):
 *   - 启动: 写标记 -> esp_restart() -> 开机进 RNDIS 网卡共享模式
 *   - 关闭: 清标记 -> esp_restart() -> 回正常串口模式
 * 重启后 main 检测 usb_net_is_active() 调 usb_net_start():
 *   进入 RNDIS 网卡设备 + 提供 tud_network_* 回调, 保证设备枚举为网卡且桌面正常显示.
 * 一次性模式: 进入即清 NVS 标记, 断电/异常重启自动回正常串口模式.
 *
 * 数据共享 (参考官方 usb_dongle): 设备进网卡模式后, 通过 esp_wifi 内部桥接把
 * USB 网卡帧 <-> WiFi STA 帧 双向转发 (L2 桥). 主机经标的 WiFi 路由器 DHCP 获取
 * 该子网 IP, 即可借用设备 WiFi 上网. "连接WiFi" 用设置页同款流程连接路由器.
 */
#include "usb_net.h"
#include <string.h>
#include "esp_log.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_private/wifi.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tusb.h"
#include "class/net/net_device.h"
#include "usb_hid.h"
#include "wifi_manager.h"

/* main.c 提供: 挂载/网卡共享等占用 USB 期间暂停串口控制台 (只写文件日志, 避免写已卸载串口崩溃) */
extern void log_console_suspend(bool suspend);

#define TAG "USB_NET"
#define NS      "usb_net"

#define KEY_ACTIVE "active"
#define KEY_GW     "gateway"
#define KEY_NM     "netmask"
#define KEY_POOL   "pool_start"

/* 默认值 (与官方 SoftAP 雷同): 网关 .1 / 掩码 255.255.255.0 / 池起始 .2 */
#define DFT_GW   ((uint32_t)0xC0A80401)   /* 192.168.4.1 */
#define DFT_NM   ((uint32_t)0xFFFFFF00)   /* 255.255.255.0 */
#define DFT_POOL ((uint32_t)0xC0A80402)   /* 192.168.4.2 */

/* 读取 int32 (IPv4 存 u32). 失败返回默认. */
static int32_t usbnet_nvs_get_u32(const char *key, uint32_t dft)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return (int32_t)dft;
    uint32_t v = 0;
    esp_err_t e = nvs_get_u32(h, key, &v);
    nvs_close(h);
    return (e == ESP_OK) ? (int32_t)v : (int32_t)dft;
}

static void usbnet_nvs_set_u32(const char *key, uint32_t v)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u32(h, key, v);
    nvs_commit(h);
    nvs_close(h);
}

bool usb_net_is_active(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;
    uint32_t v = 0;
    esp_err_t e = nvs_get_u32(h, KEY_ACTIVE, &v);
    nvs_close(h);
    return (e == ESP_OK) && (v != 0);
}

void usb_net_set_active(bool active)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    if (active) nvs_set_u32(h, KEY_ACTIVE, 1);
    else        nvs_erase_key(h, KEY_ACTIVE);
    nvs_commit(h);
    nvs_close(h);
}

/* 正运行在网卡共享模式 (RAM 标志, 开机默认 false; 进入网卡模式后为 true) */
static bool s_net_running = false;
bool usb_net_is_running(void) { return s_net_running; }

esp_err_t usb_net_activate(void)
{
    usb_net_set_active(true);
    vTaskDelay(pdMS_TO_TICKS(600));   /* 提示可见后重启 */
    esp_restart();
    return ESP_OK;   /* 不返回 */
}

void usb_net_deactivate(void)
{
    usb_net_set_active(false);
    /* 先强制 USB 断开让主机感知设备脱离 → 重启后重新枚举出串口.
     * (macOS 在运行时 TinyUSB↔串口切换后常不自动重枚举, 不强制断开则串口不恢复) */
    if (tud_mounted()) tud_disconnect();
    vTaskDelay(pdMS_TO_TICKS(50));
    usb_composite_exit();             /* 卸 TinyUSB + 装回串口 (同卸载TF恢复串口) */
    log_console_suspend(false);       /* 恢复串口控制台 */
    vTaskDelay(pdMS_TO_TICKS(600));
    esp_restart();   /* 不返回 */
}

void usb_net_get_gateway(uint32_t *ip)  { if (ip)  *ip  = (uint32_t)usbnet_nvs_get_u32(KEY_GW, DFT_GW); }
void usb_net_set_gateway(uint32_t ip)   { usbnet_nvs_set_u32(KEY_GW, ip); }
void usb_net_get_netmask(uint32_t *mask){ if (mask)*mask= (uint32_t)usbnet_nvs_get_u32(KEY_NM, DFT_NM); }
void usb_net_set_netmask(uint32_t mask) { usbnet_nvs_set_u32(KEY_NM, mask); }
/* 地址池起始 = 网关 + 1 (自动, 无需单独存储) */
void usb_net_get_pool_start(uint32_t *ip)
{
    if (!ip) return;
    uint32_t gw = (uint32_t)usbnet_nvs_get_u32(KEY_GW, DFT_GW);
    *ip = gw + 1;
}

/* ============================================================================
 * USB-RNDIS 数据通路 (TinyUSB net 类应用回调 + WiFi STA 桥接)
 * 设备枚举为网卡 (不建 esp_netif, 避免开机主任务阻塞).
 * 数据共享: 参考官方 usb_dongle, 用 esp_wifi 内部通道在 USB 网卡 与 WiFi STA 间
 * 双向转发以太网帧 (L2 桥), 主机借标的 WiFi 路由器 DHCP 上网.
 * ========================================================================== */

/* 48-bit MAC, tinyusb 的 RNDIS 类会用它在 OID 查询里回报给主机 (参考官方样板). */
uint8_t tud_network_mac_address[6] = {0x02, 0x84, 0x6A, 0x96, 0x00, 0x01};

/* WiFi STA 收帧 → 转发给 USB 主机 (tud_network_xmit 同步拷贝进 tinyusb 缓冲) */
static esp_err_t usbnet_wifi_rx_to_usb(void *buffer, uint16_t len, void *eb)
{
    if (tud_ready() && tud_network_can_xmit(len)) {
        tud_network_xmit(buffer, len);   /* xmit_cb 同步拷贝, 返回后可释放 eb */
    }
    esp_wifi_internal_free_rx_buffer(eb);
    return ESP_OK;
}

/* WiFi STA 状态变化: 连接后挂 RX 桥, 断开摘除 */
static void usbnet_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)data;
    if (base != WIFI_EVENT) return;
    if (id == WIFI_EVENT_STA_CONNECTED) {
        esp_wifi_internal_reg_rxcb(WIFI_IF_STA, usbnet_wifi_rx_to_usb);
        ESP_LOGI(TAG, "WiFi STA 已连接: 启用 网卡<->WiFi 共享");
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_wifi_internal_reg_rxcb(WIFI_IF_STA, NULL);
        ESP_LOGI(TAG, "WiFi STA 断开: 关闭桥接");
    }
}

/* --- TinyUSB net 类要求实现的应用回调 (ECM_RNDIS 模式下由应用提供) --- */

/* 主机枚举网络接口时调用 */
void tud_network_init_cb(void)
{
    ESP_LOGI(TAG, "RNDIS netif init (主机已枚举接口)");
}

/* USB IN: 电脑发来一帧 → 转发到 WiFi STA 上行 */
bool tud_network_recv_cb(const uint8_t *src, uint16_t size)
{
    if (size && wifi_manager_is_connected()) {
        esp_wifi_internal_tx(ESP_IF_WIFI_STA, (void *)src, size);
    }
    tud_network_recv_renew();
    return true;
}

/* USB OUT: 复制上层交给 tud_network_xmit 的帧到 tinyusb 缓冲, 返回长度 */
uint16_t tud_network_xmit_cb(uint8_t *dst, void *ref, uint16_t arg)
{
    memcpy(dst, ref, arg);
    return arg;
}

esp_err_t usb_net_start(void)
{
    /* 一次性模式: 进入即清 NVS 标记, 此后断电/异常重启自动回正常串口模式, 不永久卡死 */
    usb_net_set_active(false);

    /* 进入 RNDIS 网卡设备 (USB 层, 卸载 Serial/JTAG + 装 TinyUSB net) */
    esp_err_t ret = usb_composite_enter_net();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "进入 USB RNDIS 网卡失败: %s, 已回串口模式", esp_err_to_name(ret));
        return ret;
    }
    s_net_running = true;   /* 状态栏据此显示 NET 标记 */
    /* 网卡共享无 CDC 串口: 暂停串口控制台 (只写文件日志), 避免日志写已卸载串口崩溃 */
    log_console_suspend(true);

    /* 监听 WiFi STA 连接, 连接后自动建立 网卡<->WiFi 桥. WiFi 由用户在菜单"连接WiFi"连接.
     * 不在此 init esp_netif/wifi, 避免开机主任务阻塞导致黑屏. */
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, usbnet_wifi_event, NULL);

    ESP_LOGI(TAG, "USB-RNDIS 网卡已枚举, 等待连接 WiFi 后共享上网");
    return ESP_OK;
}