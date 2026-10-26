#include "netscan.h"
#include <string.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/select.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/netif.h"
#include "lwip/etharp.h"
#include "lwip/ip4_addr.h"
#include "esp_log.h"

__attribute__((unused)) static const char *TAG = "NETSCAN";

/* 取"已就绪且带 IPv4 的"网络接口 (优先 STA)。
 * 只用 lwip 公开的 netif_list, 不依赖 esp_netif 私有 API (后者在不同 IDF 版本声明位置不同). */
static struct netif *pick_sta(void)
{
    struct netif *best = NULL;
    for (struct netif *n = netif_list; n; n = n->next) {
        if (!netif_is_up(n) || !n->ip_addr.u_addr.ip4.addr) continue;
        if (n->name[0] == 's' && n->name[1] == 't') return n;   /* "st" = STA 接口优先 */
        if (!best) best = n;
    }
    return best;
}

int netscan_port_probe(const char *ip, uint16_t port, int timeout_ms)
{
    if (!ip || !port) return -1;
    struct sockaddr_in sa = { 0 };
    sa.sin_family = AF_INET;
    sa.sin_port   = htons(port);
    if (inet_pton(AF_INET, ip, &sa.sin_addr) != 1) return -1;

    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) return -1;
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);

    int r = connect(fd, (struct sockaddr *)&sa, sizeof(sa));
    int open = 0;
    if (r == 0) {
        open = 1;
    } else if (errno == EINPROGRESS || errno == EALREADY) {
        fd_set wf; FD_ZERO(&wf); FD_SET(fd, &wf);
        struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
        if (select(fd + 1, NULL, &wf, NULL, &tv) > 0) {
            int err = 0; socklen_t el = sizeof(err);
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el) == 0 && err == 0)
                open = 1;
        }
    }
    close(fd);
    return open;
}

bool netscan_resolve_mac(const char *ip, uint8_t mac[6])
{
    if (!ip || !mac) return false;
    struct netif *sta = pick_sta();
    if (!sta) return false;
    ip4_addr_t a;
    if (!ip4addr_aton(ip, &a)) return false;

    struct eth_addr *m = NULL;
    const ip4_addr_t *hip = NULL;
    for (int r = 0; r < 2; r++) {
        etharp_request(sta, &a);
        for (int i = 0; i < 12; i++) {
            if (etharp_find_addr(NULL, &a, &m, &hip) == ERR_OK && m) {
                memcpy(mac, m->addr, 6); return true;
            }
            if (etharp_find_addr(sta, &a, &m, &hip) == ERR_OK && m) {
                memcpy(mac, m->addr, 6); return true;
            }
            for (struct netif *n = netif_list; n; n = n->next)
                if (etharp_find_addr(n, &a, &m, &hip) == ERR_OK && m) {
                    memcpy(mac, m->addr, 6); return true;
                }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return false;
}

int netscan_arp_dump(char (*ips)[16], uint8_t (*macs)[6], int max)
{
    if (!ips || !macs || max <= 0) return 0;
    int n = 0;
    struct eth_addr *m = NULL;
    const ip4_addr_t *hip = NULL;
    /* 无"枚举整表"公开 API; 用 etharp_find_addr(NULL, NULL) 不可行,
     * 故对常见末位做一次快速查表 (只读, 无发包) —— 仅列出已学到的条目。 */
    for (int last = 1; last <= 254 && n < max; last++) {
        ip4_addr_t a;
        struct netif *sta = pick_sta();
        if (!sta || !sta->ip_addr.u_addr.ip4.addr) break;
        uint32_t net = sta->ip_addr.u_addr.ip4.addr & sta->netmask.u_addr.ip4.addr;
        a.addr = net | htonl((uint32_t)last);
        if (etharp_find_addr(sta, &a, &m, &hip) == ERR_OK && m) {
            ip4addr_ntoa_r(&a, ips[n], 16);
            memcpy(macs[n], m->addr, 6);
            n++;
        }
    }
    return n;
}
