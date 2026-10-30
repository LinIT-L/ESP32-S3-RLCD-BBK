/**
 * diag_core.h — 两个诊断应用的**纯逻辑**核心 (无 os / 硬件 / 框架依赖)
 *
 * 为什么单独一层:
 *   page_diag.c / page_logdiag.c 直接 include os.h + st7305.h, 无法在主机上编译,
 *   导致评分、归一化、解析这些最该被测试的逻辑一直只能靠"烧机看效果"验证。
 *   这里把**与 UI 无关**的算法抽出来, 页面调用同一份实现, 主机端用 tools/test_diag_core.c 跑。
 *
 * ⛔ 本文件不得 include 任何 ESP-IDF / LinTOS 头文件, 只能用 libc。
 */
#ifndef DIAG_CORE_H
#define DIAG_CORE_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================= 评分 / 排序 ================= */

/** 归一化: pct = score[i] * 100 / total, 向下取整。
 *  @param score 分数数组 (负数按 0 参与, 不产生负百分比)
 *  @param n     个数
 *  @param out_total 可空, 返回正分总和
 *  @return score[i] 的相对百分比 0..100; total<=0 时返回 0 (全零不制造虚假确定性) */
int dc_pct_at(const int *score, int n, int i, int *out_total);

/** 批量归一化: pct[i] = dc_pct_at(...) */
void dc_normalize(const int *score, int n, int *pct);

/** 稳定降序排序: 同分保持原索引顺序 (冒泡只交换严格大于)。
 *  @param idx 输出, 长度 >= n */
void dc_sort_desc(const int *score, int n, int *idx);

/* ================= 日志采集状态解析 ================= */

/** 解析回传文本里的 "RC=<码>" 行 (每条采集链末尾写一份 rc.txt)。
 *  @param ok_n   可空, 成功(码=0)条数
 *  @param fail_n 可空, 失败(码!=0)条数
 *  @return 找到的 rc 总条数; 0 表示一条都没有 → 调用方应判为"状态未知", 不能当成功 */
int dc_parse_rc(const char *buf, int *ok_n, int *fail_n);

/** 从 wevtutil /f:txt 输出里提取事件 ID 频次。
 *  只认独立成行的 "Event ID:" 字段, 避免把正文里的数字误当 ID。
 *  @param ids/cnt 输出数组 (长度 max), ids 按首次出现顺序
 *  @param max     容量
 *  @return 不同 ID 的个数 */
int dc_parse_event_ids(const char *buf, int *ids, int *cnt, int max);

/** 统计行数 (用于异常密度) */
int dc_count_lines(const char *buf);

/* ================= 采集链分节解析 (2026-10-30) =================
 * 回传时目标机按 "##### <完整路径>" 分节送出每个 txt, 内容是文件原文。
 * 只靠 dc_parse_rc() 扫全文 RC= 有两个毛病:
 *   ① 分不清是**哪一条链**的 rc (四条链各写一份), 无法给出"整机成功/网络失败";
 *   ② 正文里恰好出现 RC= 会被算进去 (例如某个配置项)。
 * 这里按分节路径归类, 同时统计每类**真的回传到几个数据文件** —— 后者是比退出码更硬的
 * 证据: WMIC 在 Win11 24H2 起默认被移除, 命令全返回 0 也可能一个文件都没生成。 */

#define DC_CHAIN_N 4
enum { DC_CHAIN_SYS = 0, DC_CHAIN_LOG, DC_CHAIN_NET, DC_CHAIN_ACC };   /* 与卡片顺序一致 */

typedef struct {
    int files;     /* 本节回传到的数据文件数 (rc.txt 不计) */
    int empty;     /* 其中"空文件"的个数: 文件在, 但里面一个非空白字符都没有
                    * ⇒ 那条命令确实跑了却什么也没产出 (例如权限不足/对象不存在)。
                    * ⛔ 不含"本来就可能是空的"文件 (Proxy.txt / 无转储时的 Minidump.txt 等),
                    *    白名单见 dc_parse_chains() 内部, 否则会天天误报"不全" */
    int rc_n;      /* 本节 RC= 行数; 0 = 该链 rc.txt 没回传 ⇒ 状态未知, 不能当成功 */
    int rc_fail;   /* 其中非 0 的条数 */
    int expect;    /* 该链**应该**产出几个文件 (由页面按链填写; 0 = 不比较) */
} dc_chain_t;

/** 扫描回传全文, 按 Logs\Sys|Log|Net|Acc 分节归类。
 *  @param out 长度 DC_CHAIN_N, 内部先清零
 *  @return 见到的 "##### " 分节数 (0 = 没有分节头, 协议不符或数据被截断) */
int dc_parse_chains(const char *buf, dc_chain_t out[DC_CHAIN_N]);

/* 单条链的结论 —— 五种状态分清, 不许把"未知/不完整"当成功。
 * 判定以**真的回来几个文件**为主证据 (比链尾退出码硬), rc 只用来确认"这条链跑到了末尾"。 */
typedef enum {
    DC_CHAIN_OK = 0,   /* 该有的文件都到齐 */
    DC_CHAIN_FAIL,     /* rc.txt 明确报错 (链尾命令非 0) */
    DC_CHAIN_UNKNOWN,  /* rc.txt 没回传: 该链可能没跑到末尾, 不得判成功 */
    DC_CHAIN_EMPTY,    /* 一个数据文件都没有 (如 Win11 24H2 起 WMIC 被移除) */
    DC_CHAIN_PARTIAL,  /* 只回来一部分文件: 有几项没采到, 要单独说明 */
} dc_chain_st_t;

dc_chain_st_t dc_chain_status(const dc_chain_t *c);

/** 解析回传末尾的完成标记 "##### END <成功数> [<跳过数>]" (2026-10-30 新增的轻量收尾标志)。
 *  只有收到它, 才能说"这轮传完了"; 没有标记 = 传输中途断了 (串口挂了/脚本异常/PS 被杀),
 *  此时哪怕收到了几个文件, 也不能当完整样本。
 *  @param files 可空, 返回标记里的文件数 (❌ 它只表示"脚本成功写进串口几个",
 *                **不代表设备已经落盘** —— 落盘成败看 ld_hist_append 的 s_sd_err)
 *  @return true = 找到标记 */
bool dc_parse_end(const char *buf, int *files);

/** 统计回传里 "##### SKIP <路径>" 的行数 —— 目标机**读不了/被占用**而跳过的文件。
 *  与"文件缺失"(根本没生成) 是两件事, 必须分开报:
 *    缺失  → 采集命令没产出 (checked by files < expect)
 *    跳过  → 文件在磁盘上, 但 PowerShell 读取时抛异常 (被独占打开/权限/路径过长)
 *  END 行里也带这个数, 但以本函数的实际计数为准 (END 可能因为缓冲截断丢在半路)。 */
int dc_count_skips(const char *buf);

/* ================= 日志诊断页状态机 =================
 * 页面要显示的 8 个状态全部由真实标志计算, 不做"延时切文字"式的假进度。
 * 抽成纯函数是为了能在主机上把所有路径跑一遍 (USB 未运行 / USB 起了但没连电脑 /
 * 采集失败 / 回传中 / 完成 …), 而不是只能烧机点。 */
typedef enum {
    DC_LD_BOOT = 0,   /* USB 正在后台启动 */
    DC_LD_USB_OFF,    /* USB 未启动 (可重试) */
    DC_LD_NO_PC,      /* USB 已启动, 但电脑未枚举 */
    DC_LD_RUN,        /* 正在注入命令 */
    DC_LD_SEND,       /* 命令发完, 等电脑回传 */
    DC_LD_DONE,       /* 已出分析结论 */
    DC_LD_FAIL,       /* 采集中断 (电脑掉线) */
    DC_LD_READY,      /* 就绪 / 已采集待分析 */
} dc_ld_state_t;

typedef struct {
    bool usb_busy;      /* 后台正在启动 USB */
    bool usb_running;   /* usb_hid 已启动 */
    bool pc_connected;  /* 主机已枚举 (USB 已启动 ≠ 电脑已连接) */
    bool dev_lost;      /* 采集中电脑掉线 */
    bool sending;       /* 正在回传 */
    bool collecting;    /* 正在采集 */
    bool analyzed;      /* 已分析 */
    bool xfer_fail;     /* 回传超时 (静默/总时长) —— 与"电脑掉线"同属中断, 但原因不同 */
} dc_ld_ctx_t;

dc_ld_state_t dc_ld_state(const dc_ld_ctx_t *c);

/* ================= 每条采集链"应有文件"清单 =================
 * expect 数字不允许凭感觉填 —— 这张表就是依据: 每个数字 == 该链实际写文件的命令条数,
 * 且每个文件名都必须是采集命令里真实产生的那个。tools/check_win_cmds.py 会拿这张表
 * 和 page_logdiag.c 的命令正文做**交叉校验**, 对不上就报错 (防止"加了命令忘了改 expect")。
 * 返回 0 / NULL 表示下标越界。 */
int         dc_chain_expect_len(int chain);
const char *dc_chain_expect_name(int chain, int idx);

/* ================= CDC 回传超时 =================
 * 只有"一直在收数据"才允许无限等; 一旦静默太久, 就是脚本没起来/端口选错/链路断了。
 * 判定顺序: 先看总时长上限, 再看空闲上限 (总超时优先, 避免长时间挂死)。 */
typedef enum {
    DC_XFER_OK = 0,
    DC_XFER_IDLE_TIMEOUT,     /* 超过 idle_limit_ms 没收到任何新字节 */
    DC_XFER_TOTAL_TIMEOUT,    /* 整个回传超过 total_limit_ms */
} dc_xfer_t;

dc_xfer_t dc_xfer_state(unsigned since_last_rx_ms, unsigned elapsed_ms,
                        unsigned idle_limit_ms, unsigned total_limit_ms);

/* ================= 证据强度合成 ================= */

/** 单个部件的得分合成:
 *    support 支持分, oppose 反对分 (传入正数)
 *    返回 support - oppose/2, 负数归零。
 *  反对减半的理由: 一条"不符合"的信息量弱于一条"符合"(用户可能记错),
 *  不应让单条反证抹掉多条支持。 */
int dc_combine(int support, int oppose);

/** 单部件封顶: 防止同义症状线性叠加独占全部百分比。
 *  @param cap 上限 (当前 45) */
int dc_clamp(int score, int cap);

#ifdef __cplusplus
}
#endif
#endif /* DIAG_CORE_H */
