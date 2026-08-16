#ifndef SYSTEM_ROM_H
#define SYSTEM_ROM_H

#ifdef __cplusplus
extern "C" {
#endif

/* 从 flash system 分区读取 8.BIN + E.BIN 并写到 SD /system/gam4980/
 * 如果文件已存在且大小正确则跳过 (支持用户从读卡器覆盖)
 * 返回 0 成功, 负值失败 */
int system_rom_install_to_sd(void);

#ifdef __cplusplus
}
#endif

#endif
