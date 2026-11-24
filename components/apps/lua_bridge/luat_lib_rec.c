/* luat_lib_rec.c — app.rec.*: 录音机（WAV 录制 / 播放）
 *
 * 契约: 只通过 lua_setfield(L, -2, "名字") 往**栈顶的 app 表**写字段;
 *       需要子表时自己 lua_newtable(L) → 填 → lua_setfield(L, -2, "子表名")。
 *
 * 底层是 audio_player 的录音任务（CPU1 / 优先级 8），这里只做薄封装：
 *   app.rec.start(path[, rate])  0 成功, <0 失败 (录音在独立任务里跑)
 *   app.rec.stop()               停止并收尾, 返回录了多久 (ms)
 *   app.rec.active()             是否正在录音
 *   app.rec.ms()                 已录时长 (ms)
 *   app.rec.play(path)           异步播放 WAV, 0 成功
 *   app.rec.stop_play()          停止播放
 *   app.rec.playing()            是否正在播放
 *   app.rec.play_ms()            已播 (ms)
 *   app.rec.play_total_ms()      当前/最后一条总长 (ms)
 *   app.rec.preload(bin)         把 WAV 字节预载进 PSRAM (0 成功) — 短音效用这个
 *   app.rec.play_buf()           播放预载的缓存 (异步, 0 成功) — 零 IO, 不碰 flash
 *   app.rec.buf_ready()          缓存是否就绪
 *   app.rec.unload()             释放预载缓存 (应用退出务必调用)
 */
#include "lua_bridge.h"
#include "audio_player.h"

static int l_rec_start(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    int rate = (int)luaL_optinteger(L, 2, 16000);
    lua_pushinteger(L, (lua_Integer)audio_player_rec_start(path, rate));
    return 1;
}

static int l_rec_stop(lua_State *L)
{
    audio_player_rec_stop();
    lua_pushinteger(L, (lua_Integer)audio_player_rec_ms());
    return 1;
}

static int l_rec_active(lua_State *L)
{
    lua_pushboolean(L, audio_player_rec_active());
    return 1;
}

static int l_rec_ms(lua_State *L)
{
    lua_pushinteger(L, (lua_Integer)audio_player_rec_ms());
    return 1;
}

static int l_rec_bytes(lua_State *L)
{
    lua_pushinteger(L, (lua_Integer)audio_player_rec_bytes());
    return 1;
}

/* app.rec.spec(bin) -> 0~100  第 bin 段能量 (录音时才有值) */
static int l_rec_spec(lua_State *L)
{
    int bin = (int)luaL_checkinteger(L, 1);
    lua_pushinteger(L, (lua_Integer)audio_player_rec_spec(bin - 1));   /* Lua 习惯 1 起 */
    return 1;
}

static int l_rec_spec_bins(lua_State *L)
{
    lua_pushinteger(L, (lua_Integer)audio_player_rec_spec_bins());
    return 1;
}

static int l_rec_mic_available(lua_State *L)
{
    lua_pushboolean(L, audio_player_mic_available());
    return 1;
}

static int l_rec_peak(lua_State *L)
{
    lua_pushinteger(L, (lua_Integer)audio_player_rec_peak());
    return 1;
}

/* ★ 2026-11-08 统一播放 API: 应用只传路径, 格式分流(WAV/MP3)与全部音频状态
 *   (初始化/音量/静音/功放/I2S/codec/采样率) 由底层 audio_player_play_file() 负责。
 *   play 与 mplay 现在是同一个入口; playing/mplaying、stop_play/mstop 也是统一视图。
 *   保留旧函数名只为不破坏已上架应用(录音机/电子木鱼), 语义已对齐。 */
static int l_rec_mplay(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    lua_pushinteger(L, (lua_Integer)audio_player_play_file(path));
    return 1;
}
static int l_rec_mplaying(lua_State *L)
{
    lua_pushboolean(L, audio_player_any_playing());
    return 1;
}
static int l_rec_mstop(lua_State *L)
{
    audio_player_stop_all();
    return 0;
}

static int l_rec_play(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    lua_pushinteger(L, (lua_Integer)audio_player_play_file(path));
    return 1;
}

static int l_rec_stop_play(lua_State *L)
{
    audio_player_stop_all();
    return 0;
}

static int l_rec_playing(lua_State *L)
{
    lua_pushboolean(L, audio_player_any_playing());
    return 1;
}

static int l_rec_play_ms(lua_State *L)
{
    lua_pushinteger(L, (lua_Integer)audio_player_wav_ms());
    return 1;
}

static int l_rec_play_total_ms(lua_State *L)
{
    lua_pushinteger(L, (lua_Integer)audio_player_wav_total_ms());
    return 1;
}

/* 预载短音效到 PSRAM。C 侧持有一份独立拷贝 ⇒ 传进来的 bin 可以立刻释放/被 GC。
 * 预载 WAV 字节到 PSRAM, 0=就绪; 之后用 play_buf 播放零 IO, 不碰 flash、不关 cache */
static int l_rec_preload(lua_State *L)
{
    size_t len = 0;
    const char *data = luaL_checklstring(L, 1, &len);
    lua_pushinteger(L, (lua_Integer)audio_player_wav_preload(data, len));
    return 1;
}

/* 播放已预载的音效缓存 (异步)。与 play 同一条单通道语义: 正在响时返回 <0。
 * 播放预载的缓存, 0=已开始播放; 未预载返回 -5 */
static int l_rec_play_buf(lua_State *L)
{
    lua_pushinteger(L, (lua_Integer)audio_player_play_wav_cache());
    return 1;
}

/* 释放预载缓存 (会先停掉正在播的那一声)。用完不释放 = PSRAM 被白占。
 * 释放 PSRAM 里的音效缓存, 无返回值 */
static int l_rec_unload(lua_State *L)
{
    audio_player_wav_unload();
    return 0;
}

/* 缓存是否已就绪, 用于启动时检查"预载有没有成功"。
 * 预载缓存是否可用 */
static int l_rec_buf_ready(lua_State *L)
{
    lua_pushboolean(L, audio_player_wav_cache_ready());
    return 1;
}

void lua_mod_rec_register(lua_State *L)
{
    lua_newtable(L);
    lua_pushcfunction(L, l_rec_start);          lua_setfield(L, -2, "start");
    lua_pushcfunction(L, l_rec_stop);           lua_setfield(L, -2, "stop");
    lua_pushcfunction(L, l_rec_active);         lua_setfield(L, -2, "active");
    lua_pushcfunction(L, l_rec_ms);             lua_setfield(L, -2, "ms");
    lua_pushcfunction(L, l_rec_bytes);          lua_setfield(L, -2, "bytes");
    lua_pushcfunction(L, l_rec_peak);           lua_setfield(L, -2, "peak");
    lua_pushcfunction(L, l_rec_mic_available);  lua_setfield(L, -2, "mic_available");
    lua_pushcfunction(L, l_rec_spec);           lua_setfield(L, -2, "spec");
    lua_pushcfunction(L, l_rec_spec_bins);      lua_setfield(L, -2, "spec_bins");
    lua_pushcfunction(L, l_rec_play);           lua_setfield(L, -2, "play");
    lua_pushcfunction(L, l_rec_mplay);          lua_setfield(L, -2, "mplay");
    lua_pushcfunction(L, l_rec_mplaying);       lua_setfield(L, -2, "mplaying");
    lua_pushcfunction(L, l_rec_mstop);          lua_setfield(L, -2, "mstop");
    lua_pushcfunction(L, l_rec_stop_play);      lua_setfield(L, -2, "stop_play");
    lua_pushcfunction(L, l_rec_playing);        lua_setfield(L, -2, "playing");
    lua_pushcfunction(L, l_rec_play_ms);        lua_setfield(L, -2, "play_ms");
    lua_pushcfunction(L, l_rec_play_total_ms);  lua_setfield(L, -2, "play_total_ms");
    /* 预载缓存四件套 (2026-11-23): 短音效读一次, 之后每次播放零 IO */
    lua_pushcfunction(L, l_rec_preload);        lua_setfield(L, -2, "preload");
    lua_pushcfunction(L, l_rec_play_buf);       lua_setfield(L, -2, "play_buf");
    lua_pushcfunction(L, l_rec_unload);         lua_setfield(L, -2, "unload");
    lua_pushcfunction(L, l_rec_buf_ready);      lua_setfield(L, -2, "buf_ready");
    lua_setfield(L, -2, "rec");
}
