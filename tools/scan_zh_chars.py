"""自动扫描所有 C/H 源文件中的中文字符, 生成完整字库列表
输出: 更新 components/menu/font_zh_data.inc
"""
import os
import re
import subprocess
import sys

# 扫描目录
SRC_DIRS = [
    "main",
    "components/menu",
    "components/bbk",
    "components/usbh_msc",
]

# 匹配中文字符 (3-byte UTF-8)
ZH_RE = re.compile(r'[\u4e00-\u9fff]')

# 基础常用字 (确保常用字都有)
COMMON_CHARS = (
    # 基础核心
    "我你他她它们的是在有不了这那中大来去到上"
    # 主菜单
    "步步高游戏模拟器选择按键设置蓝牙设备手柄配置音量卡管理系统信息返回文曲星伏魔记三国霸业武林奇侠雷电关于"
    # 状态/动作/提示
    "状态已连接未连接扫描断开映射校准振动对比度恢复默认上下左右确认退出"
    "帮助搜索开始刷新模式全屏固件版本内存使用电池主菜单"
    # 操作相关
    "格式化已配对清空所有数据完成失败启动卸载暴露给电脑"
    "卡信息总剩读扫描中按键映射中已断开已建"
    "开关于重试成功警告错误正在执行请稍候先"
    "并且完成提示自动引擎实现主菜单菜"
    # 弹窗/通知
    "退出取消提示暴露标签订单读取"
    # 数字/单位
    "一二三四五六七八九十百千万亿"
    # 设备/连接
    "蓝牙手柄未连接电量充电中TF卡"
    # 其它常用
    "然后如果因为所以但是而且需要可以"
    # 颜色
    "红绿蓝黄黑白紫橙"
    # 声音
    "音静音开关"
    # 时间
    "今天明天昨天现在时间小时分钟秒年月日"
    # 方位
    "上下左右前后中内外"
    # 其它
    "网无线连接信号强度模式"
)

# 从源码中扫描的额外字符
extra_chars = set()

for src_dir in SRC_DIRS:
    if not os.path.isdir(src_dir):
        continue
    for root, dirs, files in os.walk(src_dir):
        # 跳过 .git 等
        dirs[:] = [d for d in dirs if not d.startswith('.')]
        for f in files:
            if not (f.endswith('.c') or f.endswith('.h')):
                continue
            path = os.path.join(root, f)
            try:
                with open(path, 'r', encoding='utf-8') as fp:
                    content = fp.read()
            except Exception:
                continue
            for m in ZH_RE.finditer(content):
                extra_chars.add(m.group())

# 合并去重
all_chars = COMMON_CHARS + ''.join(sorted(extra_chars - set(COMMON_CHARS)))
unique = ""
for c in all_chars:
    if c not in unique:
        unique += c

print(f"基础字: {len(COMMON_CHARS)}")
print(f"源码扫描到: {len(extra_chars)}")
print(f"去重后总: {len(unique)}")
print(f"字: {unique}")
