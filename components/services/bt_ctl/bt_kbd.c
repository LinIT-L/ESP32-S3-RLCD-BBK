#include "bt_kbd.h"
#include <string.h>
#include "esp_log.h"

#define BTK_TAG "BTK"

static bt_kbd_cb_t s_kbd_cb = NULL;
static bt_kbd_key_cb_t s_key_cb = NULL;   /* 逐键边沿 (1:1 输入) */
static bool        s_is_kbd = false;     /* 当前设备是否为键盘 */
static bt_kbd_state_t s_prev;            /* 上一帧(用于边沿发射) */

static void kbd_state_init(bt_kbd_state_t *s) {
    memset(s, 0, sizeof(*s));
}

/* 扫描描述符: 含 Usage Page 0x07(键盘/小键盘) + Usage 0x06(Keyboard). */
bool bt_kbd_from_descriptor(const uint8_t *desc, uint16_t len)
{
    s_is_kbd = false;
    s_prev   = (bt_kbd_state_t){0};
    if (!desc || len == 0) return false;
    bool has_page07 = false, has_usage06 = false;
    for (uint16_t i = 0; i + 1 < len; i++) {
        /* Usage Page (0x05) = 0x07: 键盘/小键盘 页 */
        if (desc[i] == 0x05 && desc[i + 1] == 0x07) has_page07 = true;
        /* Usage (0x09) = 0x06: Keyboard 用途 */
        if (desc[i] == 0x09 && desc[i + 1] == 0x06) has_usage06 = true;
    }
    s_is_kbd = has_page07 && has_usage06;
    if (s_is_kbd) ESP_LOGI(BTK_TAG, "识别为 BLE 键盘 (Usage Page 0x07 + Usage 0x06)");
    return s_is_kbd;
}

/* HID 键盘报告(Boot 协议, 8 字节): mod|rsv|k0..k5 */
void bt_kbd_feed(const uint8_t *data, uint16_t len)
{
    if (!s_is_kbd || !data || len == 0) return;

    bt_kbd_state_t cur;
    kbd_state_init(&cur);
    cur.modifiers = data[0];
    uint8_t n = (len >= 8) ? 6 : (len >= 2 ? len - 2 : 0);
    for (uint8_t i = 0; i < n && cur.key_count < 6; i++) {
        uint8_t k = data[2 + i];
        if (k == 0) continue;
        cur.keys[cur.key_count++] = k;
    }
    cur.pressed = (cur.modifiers != 0) || (cur.key_count != 0);

    /* ★ 2026-11-22: 逐键边沿发射 (1:1 输入通道)。
     *   必须在下面 s_prev = cur 之前做 —— 那时 s_prev 还是上一帧。
     *   只对"非 0 的键"发边沿: 松开时 HID 报告对应位置是 0x00, 无法从 0 反推
     *   是哪个键松开的, 所以**释放沿按上一帧的键集合逐个补发**。 */
    if (s_key_cb) {
        for (uint8_t i = 0; i < s_prev.key_count; i++) {
            uint8_t k = s_prev.keys[i];
            if (!k) continue;
            bool still = false;
            for (uint8_t j = 0; j < cur.key_count; j++)
                if (cur.keys[j] == k) { still = true; break; }
            if (!still) s_key_cb(k, cur.modifiers, false);   /* 松开 */
        }
        for (uint8_t i = 0; i < cur.key_count; i++) {
            uint8_t k = cur.keys[i];
            if (!k) continue;
            bool was = false;
            for (uint8_t j = 0; j < s_prev.key_count; j++)
                if (s_prev.keys[j] == k) { was = true; break; }
            if (!was) s_key_cb(k, cur.modifiers, true);      /* 按下 */
        }
    }

    /* 变化才发射: 对比上一帧(键集合/修饰). */
    bool changed = (cur.modifiers != s_prev.modifiers || cur.key_count != s_prev.key_count);
    if (!changed) {
        for (uint8_t i = 0; i < cur.key_count; i++) {
            bool found = false;
            for (uint8_t j = 0; j < s_prev.key_count; j++) {
                if (s_prev.keys[j] == cur.keys[i]) { found = true; break; }
            }
            if (!found) { changed = true; break; }
        }
    }
    s_prev = cur;

    if (changed && s_kbd_cb) s_kbd_cb(&cur);
}

void bt_kbd_set_key_cb(bt_kbd_key_cb_t cb)
{
    s_key_cb = cb;
    if (cb) ESP_LOGI(BTK_TAG, "蓝牙键盘逐键回调已注册 (1:1 输入)");
}

bool bt_kbd_is_keyboard(void)
{
    return s_is_kbd;
}

int bt_kbd_hid_to_action(uint8_t u)
{
    switch (u) {
        case 0x28: return BTK_ACT_ENTER;   /* Enter */
        case 0x29: return BTK_ACT_ESC;     /* Esc */
        case 0x2A: return BTK_ACT_BKSP;    /* Backspace */
        case 0x2B: return BTK_ACT_TAB;     /* Tab */
        case 0x2C: return BTK_ACT_CHAR;    /* Space: 当普通字符走 */
        case 0x4F: return BTK_ACT_RIGHT;   /* → */
        case 0x50: return BTK_ACT_LEFT;    /* ← */
        case 0x51: return BTK_ACT_DOWN;    /* ↓ */
        case 0x52: return BTK_ACT_UP;      /* ↑ */
        case 0x4A: return BTK_ACT_HOME;
        case 0x4D: return BTK_ACT_END;
        case 0x4B: return BTK_ACT_PGUP;
        case 0x4E: return BTK_ACT_PGDN;
        case 0x4C: return BTK_ACT_DEL;     /* Delete */
        default: break;
    }
    /* 修饰键 (0xE0..0xE7): 本身不产生动作 */
    if (u >= 0xE0 && u <= 0xE7) return BTK_ACT_IGNORE;
    /* 其余落在可打印键区 → 字符 */
    if ((u >= 0x04 && u <= 0x38) || u == 0x64) return BTK_ACT_CHAR;   /* 字母/数字/符号/小键盘非ASCII */
    return BTK_ACT_IGNORE;
}

void bt_kbd_set_cb(bt_kbd_cb_t cb)
{
    s_kbd_cb = cb;
    if (cb) ESP_LOGI(BTK_TAG, "蓝牙键盘回调已注册");
}

/* 键盘 HID Usage → ASCII (基本). shift 影响字母大小写/符号. 返回 0 表无对应. */
char bt_kbd_hid_to_ascii(uint8_t u, uint8_t mod)
{
    static const char *base = "abcdefghijklmnopqrstuvwxyz1234567890-=[]\\;',./`";
    static const char *shft = "ABCDEFGHIJKLMNOPQRSTUVWXYZ!@#$%^&*()_+{}|:\"<>?~";
    bool shift = (mod & 0x02) != 0;   /* 右 Shift(左右都算) */
    shift = shift || (mod & 0x01) != 0;
    if (u >= 0x04 && u <= 0x1D) {           /* a..z (0x04..0x1D) */
        (void)base; (void)shft;
        char c = (char)('a' + (u - 0x04));
        return shift ? (char)(c - 'a' + 'A') : c;
    }
    if (u >= 0x1E && u <= 0x27) {           /* 1..0 */
        static const char d[] = "1234567890";
        static const char s[] = "!@#$%^&*()";
        return shift ? s[u - 0x1E] : d[u - 0x1E];
    }
    switch (u) {
        case 0x28: return '\n';  /* Enter */
        case 0x2A: return '\b';  /* Backspace */
        case 0x2B: return '\t';  /* Tab */
        case 0x2C: return ' ';   /* Space */
        case 0x2D: return shift ? '_' : '-';
        case 0x2E: return shift ? '+' : '=';
        case 0x2F: return shift ? '{' : '[';
        case 0x30: return shift ? '}' : ']';
        case 0x31: return shift ? '|' : '\\';
        case 0x33: return shift ? ':' : ';';
        case 0x34: return shift ? '"' : '\'';
        case 0x36: return shift ? '<' : ',';
        case 0x37: return shift ? '>' : '.';
        case 0x38: return shift ? '?' : '/';
        case 0x35: return shift ? '~' : '`';
        case 0x52: return 0x1B;  /* Up → ESC? 数值类不映射字符, 返回0 */
        default: break;
    }
    return 0;
}