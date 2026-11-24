/**
 * test_diag_core.c — diag_core 主机端单元测试
 *
 * 编译运行 (不需要 ESP-IDF):
 *   cc -I components/apps/pages tools/test_diag_core.c \
 *      components/apps/pages/diag_core.c -o /tmp/test_diag_core && /tmp/test_diag_core
 *
 * 覆盖: 归一化 / 全零 / 稳定排序 / 退出码解析 / 事件 ID 提取 / 正文误判 / 证据合成 / 钳制
 */
#include "diag_core.h"
#include <stdio.h>
#include <string.h>

static int g_pass = 0, g_fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; printf("  FAIL: %s  (line %d)\n", msg, __LINE__); } \
} while (0)

int main(void)
{
    int pct[8], idx[8];

    printf("[归一化]\n");
    {
        int sc[6] = {30, 20, 10, 0, 0, 0};
        dc_normalize(sc, 6, pct);
        CHECK(pct[0] == 50, "30/60 = 50%");
        CHECK(pct[1] == 33, "20/60 = 33% (向下取整)");
        CHECK(pct[2] == 16, "10/60 = 16%");
        CHECK(pct[3] == 0,  "0 分 = 0%");
        int sum = pct[0] + pct[1] + pct[2];
        CHECK(sum == 99, "舍入后合计 99 (不是 100, 属预期)");
    }

    printf("[全零]\n");
    {
        int z[6] = {0, 0, 0, 0, 0, 0};
        dc_normalize(z, 6, pct);
        int sum = 0;
        for (int i = 0; i < 6; i++) sum += pct[i];
        CHECK(sum == 0, "全零不制造虚假百分比");
    }

    printf("[负分]\n");
    {
        int sc[3] = {-5, 10, 0};
        dc_normalize(sc, 3, pct);
        CHECK(pct[0] == 0, "负分按 0 参与");
        CHECK(pct[1] == 100, "唯一正分 = 100%");
    }

    printf("[稳定排序]\n");
    {
        int sc[4] = {10, 10, 5, 5};
        dc_sort_desc(sc, 4, idx);
        CHECK(idx[0] == 0 && idx[1] == 1, "同分保持原索引顺序");
        CHECK(idx[2] == 2 && idx[3] == 3, "低分同样稳定");
    }

    printf("[退出码解析]\n");
    {
        int ok = 0, bad = 0;
        CHECK(dc_parse_rc("RC=0\nRC=0\n", &ok, &bad) == 2 && ok == 2 && bad == 0, "全成功");
        CHECK(dc_parse_rc("RC=0\nRC=1\n", &ok, &bad) == 2 && ok == 1 && bad == 1, "一成功一失败");
        CHECK(dc_parse_rc("no marker here", &ok, &bad) == 0, "无 RC → 0 条 (应判未知)");
        CHECK(dc_parse_rc("XRC=5", &ok, &bad) == 0, "非行首的 RC= 不误匹配");
        CHECK(dc_parse_rc(NULL, &ok, &bad) == 0, "空指针安全");
    }

    printf("[事件 ID 提取]\n");
    {
        int ids[8], cnt[8];
        const char *ev =
            "Event[0]:\n"
            "  Event ID: 6008\n"
            "  Provider: EventLog\n"
            "\n"
            "Event[1]:\n"
            "  Event ID: 6008\n"
            "\n"
            "Event[2]:\n"
            "  Event ID: 41\n";
        int n = dc_parse_event_ids(ev, ids, cnt, 8);
        CHECK(n == 2, "两个不同 ID");
        CHECK(ids[0] == 6008 && cnt[0] == 2, "6008 出现 2 次");
        CHECK(ids[1] == 41 && cnt[1] == 1, "41 出现 1 次");
    }

    printf("[正文数字不误判]\n");
    {
        int ids[8], cnt[8];
        const char *bad = "Error text says Event ID: 12345 inside a sentence\n";
        int n = dc_parse_event_ids(bad, ids, cnt, 8);
        CHECK(n == 0, "正文中间出现的字样不计为事件字段");
    }

    printf("[截断/大文本]\n");
    {
        CHECK(dc_count_lines("") == 1, "空串算 1 行");
        CHECK(dc_count_lines("a\nb\nc") == 3, "3 行");
        CHECK(dc_count_lines("a\nb\nc\n") == 4, "结尾换行也算一行 (与页面口径一致)");
    }

    printf("[证据合成]\n");
    {
        CHECK(dc_combine(20, 0) == 20, "纯支持");
        CHECK(dc_combine(20, 10) == 15, "反对减半");
        CHECK(dc_combine(10, 40) == 0, "反对超过支持 → 归零, 不出现负分");
    }

    printf("[钳制]\n");
    {
        CHECK(dc_clamp(60, 45) == 45, "超上限封顶");
        CHECK(dc_clamp(-3, 45) == 0, "负分归零");
        CHECK(dc_clamp(20, 45) == 20, "未超限不动");
    }

    printf("[采集链分节解析]\n");
    {
        dc_chain_t c[DC_CHAIN_N];

        /* 正常: 四条链各回传一份 rc.txt, 其中 Net 有非 0 码 */
        const char *ok_all =
            "##### C:\\Users\\u\\Desktop\\Logs\\Sys\\CPU.txt\r\nName Intel\r\n"
            "##### C:\\Users\\u\\Desktop\\Logs\\Sys\\Disk.txt\r\nCaption C:\r\n"
            "##### C:\\Users\\u\\Desktop\\Logs\\Sys\\rc.txt\r\nRC=0 \r\n"
            "##### C:\\Users\\u\\Desktop\\Logs\\Log\\rc.txt\r\nRC=0 \r\n"
            "##### C:\\Users\\u\\Desktop\\Logs\\Net\\rc.txt\r\nRC=1 \r\n"
            "##### C:\\Users\\u\\Desktop\\Logs\\Acc\\rc.txt\r\nRC=0 \r\n";
        int sec = dc_parse_chains(ok_all, c);
        CHECK(sec == 6, "分节数 = 6");
        CHECK(c[DC_CHAIN_SYS].files == 2, "Sys 数据文件 2 个 (rc.txt 不计)");
        CHECK(c[DC_CHAIN_SYS].rc_n == 1 && c[DC_CHAIN_SYS].rc_fail == 0, "Sys rc=0");
        CHECK(c[DC_CHAIN_NET].rc_n == 1 && c[DC_CHAIN_NET].rc_fail == 1, "Net rc!=0 → 记为失败");
        CHECK(c[DC_CHAIN_LOG].files == 0 && c[DC_CHAIN_LOG].rc_n == 1, "Log 只有 rc.txt, 无数据文件");

        /* rc.txt 缺失 → 该链 rc_n==0, 调用方必须判"未知"而不是成功 */
        const char *no_rc =
            "##### C:\\Users\\u\\Desktop\\Logs\\Sys\\CPU.txt\r\nName Intel\r\n";
        dc_parse_chains(no_rc, c);
        CHECK(c[DC_CHAIN_SYS].files == 1 && c[DC_CHAIN_SYS].rc_n == 0, "无 rc.txt → 状态未知");
        CHECK(c[DC_CHAIN_LOG].rc_n == 0 && c[DC_CHAIN_NET].rc_n == 0, "没出现的链保持 0");

        /* 正文里出现 RC= 不能被算进来 (只在分节内的行首才算) */
        const char *body_rc =
            "##### C:\\Users\\u\\Desktop\\Logs\\Sys\\Boot.ini.txt\r\n"
            "option XRC=5\r\n  RC=9\r\n";
        dc_parse_chains(body_rc, c);
        CHECK(c[DC_CHAIN_SYS].rc_n == 1 && c[DC_CHAIN_SYS].rc_fail == 1, "只认行首 RC= (行中间不算)");

        /* 大小写/无分节头/空输入 */
        dc_parse_chains("##### C:\\USERS\\U\\DESKTOP\\LOGS\\ACC\\users.TXT\r\n", c);
        CHECK(c[DC_CHAIN_ACC].files == 1, "路径大小写无关");
        CHECK(dc_parse_chains("RC=0\r\nRC=0\r\n", c) == 0, "无分节头 → 0 节, RC 不归类");
        CHECK(c[DC_CHAIN_SYS].rc_n == 0, "无分节头时不误记到 Sys");
        CHECK(dc_parse_chains("", c) == 0, "空串安全");
        CHECK(dc_parse_chains(NULL, c) == 0, "NULL 安全");

        /* truncation: 末尾半行不能让解析越界 */
        dc_parse_chains("##### C:\\Users\\u\\Desktop\\Logs\\Sys\\CPU.tx", c);
        CHECK(c[DC_CHAIN_SYS].files == 0, "半截路径不计为文件");
    }

    printf("[采集链结论]\n");
    {
        dc_chain_t c;
        memset(&c, 0, sizeof(c));
        CHECK(dc_chain_status(&c) == DC_CHAIN_UNKNOWN, "无 rc → 未知 (不能默认成功)");
        c.rc_n = 1; c.rc_fail = 0; c.files = 0;
        CHECK(dc_chain_status(&c) == DC_CHAIN_EMPTY, "rc=0 但无数据文件 → 无数据");
        c.files = 2;
        CHECK(dc_chain_status(&c) == DC_CHAIN_OK, "rc=0 且有文件 → 成功");
        c.rc_fail = 1;
        CHECK(dc_chain_status(&c) == DC_CHAIN_FAIL, "rc 非 0 → 失败 (未给预期文件数时)");
        CHECK(dc_chain_status(NULL) == DC_CHAIN_UNKNOWN, "空指针安全");

        /* 给了"应有几个文件"之后, 以文件数为准 */
        c.rc_fail = 0; c.files = 7; c.expect = 9;
        CHECK(dc_chain_status(&c) == DC_CHAIN_PARTIAL, "7/9 文件 → 不全");
        c.files = 9;
        CHECK(dc_chain_status(&c) == DC_CHAIN_OK, "9/9 → 成功");
        c.rc_fail = 1;
        CHECK(dc_chain_status(&c) == DC_CHAIN_OK, "文件齐但链尾码非 0 (findstr 未命中) 不误判失败");
        c.files = 0;
        CHECK(dc_chain_status(&c) == DC_CHAIN_EMPTY, "0 个文件优先判无数据");
    }

    printf("[日志诊断状态机]\n");
    {
        dc_ld_ctx_t c;

        /* ① USB 未运行 → 进页面后应显示"未启动"(并给重试入口), 不能显示就绪 */
        memset(&c, 0, sizeof(c));
        CHECK(dc_ld_state(&c) == DC_LD_USB_OFF, "USB 未运行 → USB_OFF");

        /* ② 后台启动中 → 启动态优先于其它一切 */
        c.usb_busy = true;
        CHECK(dc_ld_state(&c) == DC_LD_BOOT, "启动中 → BOOT");
        c.usb_busy = false;

        /* ③ USB 已运行但电脑没枚举 (插充电器) → 不能当"已连接" */
        c.usb_running = true;
        CHECK(dc_ld_state(&c) == DC_LD_NO_PC, "USB 起着但未枚举 → NO_PC");
        c.pc_connected = true;
        CHECK(dc_ld_state(&c) == DC_LD_READY, "已连接且空闲 → READY");

        /* ④ 采集中 / 回传中 */
        c.collecting = true;
        CHECK(dc_ld_state(&c) == DC_LD_RUN, "采集中 → RUN");
        c.collecting = false; c.sending = true;
        CHECK(dc_ld_state(&c) == DC_LD_SEND, "回传中 → SEND");
        c.sending = false;

        /* ⑤ 电脑掉线: 即使 USB 还起着, 也要报中断而不是"就绪" */
        c.dev_lost = true;
        CHECK(dc_ld_state(&c) == DC_LD_FAIL, "掉线 → FAIL");
        c.usb_running = false; c.pc_connected = false;
        CHECK(dc_ld_state(&c) == DC_LD_USB_OFF, "USB 关了优先报 USB_OFF");
        c.dev_lost = false; c.usb_running = true; c.pc_connected = true;

        /* ⑥ 已分析 → DONE; 再次采集(analyzed 清掉) → READY */
        c.analyzed = true;
        CHECK(dc_ld_state(&c) == DC_LD_DONE, "已分析 → DONE");
        c.analyzed = false;
        CHECK(dc_ld_state(&c) == DC_LD_READY, "新一轮 → READY");

        /* ⑦ 空指针 */
        CHECK(dc_ld_state(NULL) == DC_LD_USB_OFF, "空指针安全");
    }

    printf("[回传完成标记]\n");
    {
        int n = 0;
        CHECK(!dc_parse_end("##### C:\\Logs\\Sys\\CPU.txt\r\nName x\r\n", &n), "无 END 行 → 未完成");
        CHECK(dc_parse_end("##### END 12\r\n", &n) && n == 12, "END 12 → 完整, 文件数 12");
        CHECK(dc_parse_end("abc\r\n##### END 0\r\n", &n) && n == 0, "END 0 (一个文件都没有) 也算收到了标记");
        CHECK(dc_parse_end("##### END 3", &n) && n == 3, "末尾没有换行也能认");
        /* 正文里偶然出现同样字样 → 以最后一条为准 */
        CHECK(dc_parse_end("##### END 2\r\n...body...\r\n##### END 7\r\n", &n) && n == 7, "取最后一次出现");
        CHECK(!dc_parse_end(NULL, &n), "NULL 安全");
        CHECK(!dc_parse_end("", &n), "空串安全");
    }


    printf("[应有文件清单]\n");
    {
        /* expect 数字必须有依据: 数一遍清单, 并校验每项都是 .txt 且不重名 */
        int tot = 0;
        CHECK(dc_chain_expect_len(0) == 9, "Sys 应有 9 个文件");
        CHECK(dc_chain_expect_len(1) == 6, "Log 应有 6 个文件");
        CHECK(dc_chain_expect_len(2) == 9, "Net 应有 9 个文件");
        CHECK(dc_chain_expect_len(3) == 5, "Acc 应有 5 个文件");
        CHECK(dc_chain_expect_len(-1) == 0 && dc_chain_expect_len(4) == 0, "越界返回 0");
        CHECK(dc_chain_expect_name(0, 0) != NULL && dc_chain_expect_name(0, 9) == NULL, "名字越界返回 NULL");
        for (int c = 0; c < DC_CHAIN_N; c++) {
            int n = dc_chain_expect_len(c);
            tot += n;
            for (int i = 0; i < n; i++) {
                const char *nm = dc_chain_expect_name(c, i);
                CHECK(nm != NULL, "名字非空");
                CHECK(nm && strlen(nm) > 4 && strstr(nm, ".txt") != NULL, "都以 .txt 结尾");
                for (int j = i + 1; j < n; j++)          /* 链内不重名 */
                    CHECK(strcmp(nm, dc_chain_expect_name(c, j)) != 0, "链内文件名不重复");
            }
        }
        CHECK(tot == 29, "四链合计 29 个文件 (与页面汇总口径一致)");
    }

    printf("[空文件识别]\n");
    {
        dc_chain_t c[DC_CHAIN_N];
        /* RAM.txt 只有空行 → 空文件; Proxy.txt 空是正常的 (白名单) */
        const char *buf =
            "##### C:\\u\\Logs\\Sys\\CPU.txt\r\nName Intel\r\n"
            "##### C:\\u\\Logs\\Sys\\RAM.txt\r\n\r\n"
            "##### C:\\u\\Logs\\Net\\Proxy.txt\r\n\r\n"
            "##### C:\\u\\Logs\\Net\\IP.txt\r\n192.168.1.5\r\n";
        dc_parse_chains(buf, c);
        CHECK(c[DC_CHAIN_SYS].files == 2 && c[DC_CHAIN_SYS].empty == 1, "RAM 空 → 计入 empty");
        CHECK(c[DC_CHAIN_NET].files == 2 && c[DC_CHAIN_NET].empty == 0, "Proxy 空不算 (白名单)");
        c[DC_CHAIN_SYS].rc_n = 1; c[DC_CHAIN_SYS].expect = 2;
        CHECK(dc_chain_status(&c[DC_CHAIN_SYS]) == DC_CHAIN_PARTIAL, "2 个文件里 1 个空 → 不全");
        c[DC_CHAIN_SYS].empty = 0;
        CHECK(dc_chain_status(&c[DC_CHAIN_SYS]) == DC_CHAIN_OK, "都有内容 → 成功");
    }

    printf("[UTF-8 多字节/分包]\n");
    {
        dc_chain_t c[DC_CHAIN_N];
        /* 内容里含中文: 解析必须按字节走, 不能因为多字节字符而算错行/截断 */
        const char *u = "##### C:\\u\\Logs\\Sys\\OS.txt\r\n"
                        "Caption : Microsoft Windows 11 \xe5\xae\xb6\xe5\xba\xad\xe4\xb8\xad\xe6\x96\x87\xe7\x89\x88\r\n"
                        "BuildNumber : 26100\r\n";
        dc_parse_chains(u, c);
        CHECK(c[DC_CHAIN_SYS].files == 1 && c[DC_CHAIN_SYS].empty == 0, "含中文的文件不算空");
        /* 模拟"多字节字符被 CDC 分包切开": 两半拼接后应与整体完全等价 */
        char joined[512];
        size_t half = strlen(u) / 2;                 /* 故意切在一个汉字中间 */
        memcpy(joined, u, half);
        memcpy(joined + half, u + half, strlen(u) - half + 1);
        dc_chain_t d[DC_CHAIN_N];
        dc_parse_chains(joined, d);
        CHECK(memcmp(c, d, sizeof(c)) == 0, "分包拼接后解析结果一致");
        CHECK(dc_parse_end("##### END 29\r\n", NULL), "END 行含中文场景下仍可识别");
    }

    printf("[回传超时判定]\n");
    {
        CHECK(dc_xfer_state(0, 0, 20000, 150000) == DC_XFER_OK, "刚开始 → 正常");
        CHECK(dc_xfer_state(1000, 30000, 20000, 150000) == DC_XFER_OK, "1s 前有数据 → 正常");
        CHECK(dc_xfer_state(20000, 30000, 20000, 150000) == DC_XFER_IDLE_TIMEOUT, "静默 20s → 空闲超时");
        CHECK(dc_xfer_state(1000, 150000, 20000, 150000) == DC_XFER_TOTAL_TIMEOUT, "总时长到 → 总超时");
        CHECK(dc_xfer_state(200000, 200000, 20000, 150000) == DC_XFER_TOTAL_TIMEOUT, "两者都超 → 报总超时优先");
        CHECK(dc_xfer_state(0, 0, 0, 0) == DC_XFER_OK, "限值为 0 = 不超时");
    }


    printf("[BOM/空白 空文件判定]\n");
    {
        /* 每一行 = 一份 "CPU.txt" 的正文; 期望它算不算"空"。
         * ★ 判据: 剥掉 UTF-8 BOM 后, 一个非空白字节都没有 → 空。
         * ⛔ 汉字三字节都 >= 0x80, 不会被空白判定吃掉 —— 下面两条中文用例就是钉这个。 */
        struct { const char *name; const char *body; bool want_empty; } cs[] = {
            { "0 字节",           "",                            true  },
            { "仅 BOM",           "\xEF\xBB\xBF",                true  },
            { "BOM+空格+CRLF",    "\xEF\xBB\xBF" " \r\n",        true  },
            { "仅 CR/LF/TAB",     "\r\n\t\r\n",                  true  },
            { "纯空格行",         "   \r\n",                     true  },
            { "BOM+中文",         "\xEF\xBB\xBF" "\xE4\xB8\xAD\xE6\x96\x87", false },
            { "纯 ASCII",         "Name Intel\r\n",              false },
            { "中文+行尾空格",    "\xE4\xB8\xAD \r\n",           false },
        };
        for (unsigned i = 0; i < sizeof(cs) / sizeof(cs[0]); i++) {
            char buf[256];
            snprintf(buf, sizeof(buf),
                     "##### C:\\u\\Logs\\Sys\\CPU.txt\r\n%s\r\n"
                     "##### C:\\u\\Logs\\Sys\\OS.txt\r\nX\r\n", cs[i].body);
            dc_chain_t ch[DC_CHAIN_N];
            dc_parse_chains(buf, ch);
            CHECK(ch[DC_CHAIN_SYS].files == 2, cs[i].name);
            CHECK((ch[DC_CHAIN_SYS].empty == 1) == cs[i].want_empty, cs[i].name);
        }

        /* BOM 与正文"被 CDC 分包切开"不影响判定: 解析只看拼好的整块 */
        const char *whole =
            "##### C:\\u\\Logs\\Sys\\CPU.txt\r\n\xEF\xBB\xBF" "\xE4\xB8\xAD\r\n"
            "##### C:\\u\\Logs\\Sys\\OS.txt\r\n\r\n";
        char joined[256];
        size_t half = strlen(whole) / 2;          /* 故意切点落在随机位置 */
        memcpy(joined, whole, half);
        memcpy(joined + half, whole + half, strlen(whole) - half + 1);
        dc_chain_t a[DC_CHAIN_N], b[DC_CHAIN_N];
        dc_parse_chains(whole, a);
        dc_parse_chains(joined, b);
        CHECK(memcmp(a, b, sizeof(a)) == 0, "分包拼接与整体等价");
        CHECK(a[DC_CHAIN_SYS].files == 2 && a[DC_CHAIN_SYS].empty == 1, "BOM+中文 非空 / 空文件 1 个");
    }

    printf("[回传跳过 / END 计数]\n");
    {
        int n = -1;
        CHECK(dc_count_skips(NULL) == 0, "空指针安全");
        CHECK(dc_count_skips("##### a\r\n##### SKIP C:\\x\\RAM.txt\r\n##### SKIP C:\\y\\Z.txt\r\n") == 2,
              "数 SKIP 行");
        CHECK(dc_count_skips("##### C:\\x\\SKIP.txt\r\n") == 0, "文件名里含 SKIP 不算");
        CHECK(dc_parse_end("x\r\n##### END 29 2\r\n", &n) && n == 29, "END 带两个数: 取成功数");
        CHECK(dc_parse_end("x\r\n##### END 0 0\r\n", &n) && n == 0, "全失败也认 END");
        CHECK(dc_parse_end("x\r\n##### END\r\n", &n) && n == 0, "END 不带数仍算收到标记");
        CHECK(dc_parse_end("##### END 1 0\r\n##### END 9 0\r\n", &n) && n == 9, "多条 END 取最后一条");

        /* 只有 ENC 行、没有任何文件: 不能当成"回传格式异常", 但文件数必须是 0 */
        dc_chain_t ch[DC_CHAIN_N];
        int secs = dc_parse_chains("##### ENC 936 936\r\n##### END 0 0\r\n", ch);
        CHECK(secs == 2, "ENC/END 也算分节头");
        int f = 0;
        for (int i = 0; i < DC_CHAIN_N; i++) f += ch[i].files;
        CHECK(f == 0, "没有文件时 files 合计 0");
    }

    printf("\n==== %d passed, %d failed ====\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
