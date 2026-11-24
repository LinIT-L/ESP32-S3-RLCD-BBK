/**
 * svc_backup.h — 通用「备份策略」服务
 *
 * 定位（2026-11-22 linit 定）:
 *   本模块**不认识任何业务数据**，也不做打包。它只提供一套"备份该怎么做"的
 *   策略: 存到哪个目录 / 文件怎么命名 / 保留几份 / 怎么写才不写坏 / 写坏了
 *   怎么校验 / 份数满了覆盖哪一份。
 *
 *   业务模块自己把数据序列化成字节流，调 bkp_save() 落盘；恢复时调 bkp_load()
 *   拿回**原始字节**，自己反序列化。
 *
 *   ⛔ 本模块不做的事:
 *      · 不把不同业务的数据塞进同一个备份文件（那是"打包"，不是策略）
 *      · 不解析、不校验业务字段的语义（只校验字节完整性）
 *      · 不管业务数据怎么来（谁能备份什么，由业务自己决定并调用）
 *
 *   这样每类数据各自一个 id，各自备份 / 各自恢复 / 互不影响；
 *   新增一种要备份的数据 = 业务侧加一句 bkp_save()，本模块零改动。
 *
 * ── 为什么不加密（2026-11-22 linit 决策，慎重评估后定） ──
 *   备份的内容是**配置类数据**（设置项、书签、列表、进度），本身没有保密价值；
 *   真正敏感的东西（设备 PIN）归 keyvault，本来就不随备份迁移。而加密的实际代价:
 *      ① 用户必须记住口令 —— 忘了就**永久读不出来**，这是真实的数据丢失风险，
 *         比"被同卡的人看到配置项"这个风险大得多；
 *      ② 每次恢复要跑 PBKDF2 两万轮，S3 上数百毫秒，恢复体验变差；
 *      ③ 额外依赖 mbedtls/PSA，代码体积和攻击面都变大。
 *   所以本模块**只做完整性校验，不做加密**:
 *      CRC32 能挡住真正的破坏场景（写到一半拔卡、文件被截断、扇区坏块），
 *      这才是备份最常见的失效原因。
 *   历史包袱说明: page_fav 的 FAVENCC1 加密包继续保留可读（有用户旧备份在卡上），
 *      但**新代码不再往加密方向走**，两者格式独立、互不干扰。
 *
 * 典型用法:
 *      size_t cap = my_max_size();
 *      uint8_t *buf = bkp_alloc(cap);              // PSRAM，不占内部 RAM
 *      size_t len = my_serialize(buf, cap);
 *      bkp_save("bookmark", buf, len);
 *      bkp_free(buf);
 *
 *      // 恢复
 *      uint8_t *rb = bkp_alloc(cap);
 *      size_t rlen = 0;
 *      if (bkp_load("bookmark", 0, rb, cap, &rlen) == ESP_OK)
 *          my_deserialize(rb, rlen);
 *      bkp_free(rb);
 *
 * 文件布局（15 字节定长头 + 原始数据）:
 *      "BKRP1"(5) | ver(1) | flags(1) | len(u32 LE) | crc32(u32 LE) | payload
 *      —— len 必须与实际文件长度吻合，crc32 覆盖 payload。
 *         两个都过才算完整，任一项不符一律拒绝（绝不回灌半截数据）。
 */
#ifndef LIN_TOS_SVC_BACKUP_H
#define LIN_TOS_SVC_BACKUP_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BKP_DIR_DEFAULT  "/sdcard/backup"   /* 与收藏页既有备份同目录，用户只需记一处 */
#define BKP_ID_MAX       16                 /* id 上限（同时决定文件名长度） */
#define BKP_SLOT_DEFAULT 4                  /* 未声明策略时保留 4 份 */
#define BKP_SLOT_MAX     16
#define BKP_PAYLOAD_MAX  (1u << 20)         /* 单份 1MB 上限: 挡住坏头导致的巨额分配 */

/* 备份策略: 声明"这类数据怎么备"
 * ⚠ id/dir 指向的字符串必须**静态常驻**（字面量或全局），本模块只存指针不复制。 */
typedef struct {
    const char *id;        /* 唯一标识，同时是文件名前缀，如 "settings"/"bookmark" */
    const char *dir;       /* 目录; NULL = BKP_DIR_DEFAULT */
    uint8_t     slots;     /* 保留份数 1..BKP_SLOT_MAX; 0 = BKP_SLOT_DEFAULT */
} bkp_policy_t;

/* 枚举结果项（供"恢复哪个备份"的列表用） */
typedef struct {
    int      slot;         /* 1..slots，对应文件名里的序号 */
    char     name[28];     /* 文件名 */
    uint32_t size;         /* 文件字节数 */
    uint32_t mtime;        /* 修改时间（秒） */
} bkp_entry_t;

/* ===== 策略 ===== */
/* 注册/更新一条策略。同一 id 重复注册即更新。未注册的 id 走默认策略。 */
esp_err_t bkp_policy_set(const bkp_policy_t *p);
/* 取生效策略（未注册则返回默认策略，永不为 NULL） */
const bkp_policy_t *bkp_policy_get(const char *id);

/* ===== 备份 / 恢复 ===== */
/* 保存一份备份: 采集字节 → 加头 → 原子写 → 份数满则覆盖最旧。
 * data/len 为业务序列化后的原始字节，本模块原样保存。
 * 返回: ESP_OK / ESP_ERR_INVALID_ARG / ESP_ERR_INVALID_SIZE(len 超上限)
 *       / ESP_ERR_NO_MEM / ESP_FAIL(写盘失败: 卡不在或写满) */
esp_err_t bkp_save(const char *id, const void *data, size_t len);

/* 读取一份备份到 out（原始字节，与 bkp_save 传进去的逐字节一致）。
 * slot: 0 = 最新一份; 1..N = 指定序号。
 * 返回: ESP_OK / ESP_ERR_NOT_FOUND(没这份) / ESP_ERR_INVALID_SIZE(缓冲不够)
 *       / ESP_FAIL(文件损坏: 头不符/长度不符/crc 不过) */
esp_err_t bkp_load(const char *id, int slot, void *out, size_t cap, size_t *out_len);

/* 列出某 id 现有备份（按 mtime 新→旧）。返回条数，负数为错误。 */
int       bkp_list(const char *id, bkp_entry_t *out, int max);
/* 是否至少有一份 */
bool      bkp_has_any(const char *id);
/* 删除指定份（slot 同 bkp_load 语义，0 = 最新一份） */
esp_err_t bkp_remove(const char *id, int slot);

/* 便捷分配: 备份数据一律走 PSRAM，不占内部 RAM。失败返回 NULL。 */
void     *bkp_alloc(size_t n);
void      bkp_free(void *p);

/* ===== 可选: 整个 NVS 命名空间备份 =====
 * 适用"设置项都散在 NVS 键里、不想逐个列举"的场景。
 * 内部按 TLV 导出该 ns 下全部键（u8/i16/u16/i32/u32/i64/u64/str/blob），
 * 回灌时按同样类型写回。ns 不存在时导出 0 条（仍算成功）。 */
esp_err_t bkp_save_nvs(const char *id, const char *nvs_ns);
esp_err_t bkp_load_nvs(const char *id, int slot, const char *nvs_ns);

#ifdef __cplusplus
}
#endif
#endif /* LIN_TOS_SVC_BACKUP_H */
