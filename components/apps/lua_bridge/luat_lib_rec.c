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

static int l_rec_play(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    lua_pushinteger(L, (lua_Integer)audio_player_play_wav(path));
    return 1;
}

static int l_rec_stop_play(lua_State *L)
{
    audio_player_wav_stop();
    return 0;
}

static int l_rec_playing(lua_State *L)
{
    lua_pushboolean(L, audio_player_wav_active());
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

void lua_mod_rec_register(lua_State *L)
{
    lua_newtable(L);
    lua_pushcfunction(L, l_rec_start);          lua_setfield(L, -2, "start");
    lua_pushcfunction(L, l_rec_stop);           lua_setfield(L, -2, "stop");
    lua_pushcfunction(L, l_rec_active);         lua_setfield(L, -2, "active");
    lua_pushcfunction(L, l_rec_ms);             lua_setfield(L, -2, "ms");
    lua_pushcfunction(L, l_rec_bytes);          lua_setfield(L, -2, "bytes");
    lua_pushcfunction(L, l_rec_play);           lua_setfield(L, -2, "play");
    lua_pushcfunction(L, l_rec_stop_play);      lua_setfield(L, -2, "stop_play");
    lua_pushcfunction(L, l_rec_playing);        lua_setfield(L, -2, "playing");
    lua_pushcfunction(L, l_rec_play_ms);        lua_setfield(L, -2, "play_ms");
    lua_pushcfunction(L, l_rec_play_total_ms);  lua_setfield(L, -2, "play_total_ms");
    lua_setfield(L, -2, "rec");
}
