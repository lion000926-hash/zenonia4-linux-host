#define _GNU_SOURCE
#include "demo.h"
#include <dirent.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * 音频桥 —— 背景音乐与音效分别播放：
 *
 *   if (vol == 0 && loop)   -> 停止播放（不是静音播放）
 *   音量 = vol / 100
 *   s000..s095 -> 音效：可叠加，不打断音乐
 *   s100..s115 -> 背景音乐：独占，新的先停旧的
 *
 * 引擎 ID 是排序后的资源下标。当前 109 个 Ogg 的时长已核对：
 * id 0..93 为 0.25~4.25 秒音效，id 94..108 为 14~51 秒音乐。
 */

#define AUDIO_MAX_ID 256
#define AUDIO_MUSIC_FIRST_FILE 100
#define AUDIO_MAX_CHANNELS 32
#define MIX_MAX_VOLUME 128
#define MIX_DEFAULT_FORMAT 0x8010 /* AUDIO_S16LSB */

typedef void MixChunk;
typedef void MixMusic;

static struct {
    int  (*OpenAudio)(int, int, int, int);
    void (*CloseAudio)(void);
    MixChunk *(*LoadWAV)(const char *);
    void (*FreeChunk)(MixChunk *);
    int  (*PlayChannel)(int, MixChunk *, int);
    int  (*VolumeChunk)(MixChunk *, int);
    int  (*HaltChannel)(int);
    int  (*AllocateChannels)(int);
    MixMusic *(*LoadMUS)(const char *);
    void (*FreeMusic)(MixMusic *);
    int  (*PlayMusic)(MixMusic *, int);
    int  (*HaltMusic)(void);
    int  (*VolumeMusic)(int);
    int  (*PlayingMusic)(void);
    const char *(*GetError)(void);
} M;

static void *mix_handle;
static MixChunk *chunks[AUDIO_MAX_ID];
static int channels[AUDIO_MAX_ID];
static MixMusic *music;
static int music_id = -1;
static int audio_ready;
static int logged;
static int sfx_plays;

static const char *fallback_error(void) { return "unknown"; }

/*
 * 资源 ID 是按"文件在 res/raw 里的先后顺序"连续分配的：
 *   s000 = 0x7f090002, s001 = 0x7f090003, ...
 * 但 res/raw 里存在缺号（84/89/96/97/98/99/104），所以
 *   资源 ID = 0x7f090002 + <该文件在排序后的下标>
 * 引擎传进来的 id 是这个下标，而不是文件名里的数字。必须先建表。
 */
typedef struct { int num; char name[32]; } AudioFile;
static AudioFile files[AUDIO_MAX_ID];
static int file_count;

static int by_num(const void *a, const void *b)
{
    return ((const AudioFile *)a)->num - ((const AudioFile *)b)->num;
}

static void build_file_table(void)
{
    DIR *d = opendir("res/raw");
    if (!d) {
        game_log("AUDIO: res/raw not readable, fallback to name numbering\n");
        return;
    }
    struct dirent *e;
    while ((e = readdir(d)) && file_count < AUDIO_MAX_ID) {
        int n;
        if (sscanf(e->d_name, "s%d.ogg", &n) == 1 && n >= 0 && n < 1000) {
            files[file_count].num = n;
            snprintf(files[file_count].name, sizeof(files[file_count].name),
                     "%s", e->d_name);
            file_count++;
        }
    }
    closedir(d);
    qsort(files, file_count, sizeof(AudioFile), by_num);
    game_log("AUDIO: indexed %d sound files from res/raw\n", file_count);
}

int audio_init(void)
{
    mix_handle = dlopen("libSDL2_mixer-2.0.so.0", RTLD_NOW | RTLD_GLOBAL);
    if (!mix_handle) {
        game_log("AUDIO: mixer unavailable (%s), running silent\n", dlerror());
        return -1;
    }
#define ALOAD(F)                                                        \
    M.F = (void *)dlsym(mix_handle, "Mix_" #F);                          \
    if (!M.F) {                                                         \
        game_log("AUDIO: missing Mix_" #F "\n");                        \
        return -1;                                                      \
    }
    ALOAD(OpenAudio) ALOAD(CloseAudio) ALOAD(LoadWAV) ALOAD(FreeChunk)
    ALOAD(PlayChannel) ALOAD(VolumeChunk) ALOAD(HaltChannel)
    ALOAD(AllocateChannels) ALOAD(LoadMUS) ALOAD(FreeMusic)
    ALOAD(PlayMusic) ALOAD(HaltMusic) ALOAD(VolumeMusic) ALOAD(PlayingMusic)
#undef ALOAD
    /* SDL_mixer 2.6+ 把 Mix_GetError 定义成 SDL_GetError 的宏，没有独立符号 */
    M.GetError = (void *)dlsym(mix_handle, "Mix_GetError");
    if (!M.GetError)
        M.GetError = (void *)dlsym(RTLD_DEFAULT, "SDL_GetError");
    if (!M.GetError)
        M.GetError = fallback_error;

    if (M.OpenAudio(44100, MIX_DEFAULT_FORMAT, 2, 1024) < 0) {
        game_log("AUDIO: open failed %s\n", M.GetError());
        return -1;
    }
    M.AllocateChannels(AUDIO_MAX_CHANNELS);
    for (int i = 0; i < AUDIO_MAX_ID; i++)
        channels[i] = -1;
    build_file_table();
    audio_ready = 1;
    game_log("AUDIO: ready 44100Hz s16 stereo, %d sfx channels\n",
             AUDIO_MAX_CHANNELS);
    return 0;
}

static void path_for(int id, char *out, size_t cap)
{
    if (id >= 0 && id < file_count) {
        snprintf(out, cap, "res/raw/%s", files[id].name);
        return;
    }
    snprintf(out, cap, "res/raw/s%03d.ogg", id);
}

static int is_music(int id)
{
    if (id >= 0 && id < file_count)
        return files[id].num >= AUDIO_MUSIC_FIRST_FILE;
    return id >= 94 && id <= 108;
}

static MixChunk *chunk_for(int id)
{
    if (id < 0 || id >= AUDIO_MAX_ID)
        return NULL;
    if (chunks[id])
        return chunks[id];
    char path[128];
    path_for(id, path, sizeof(path));
    if (access(path, R_OK)) {
        if (logged++ < 12)
            game_log("AUDIO: no file %s\n", path);
        return NULL;
    }
    chunks[id] = M.LoadWAV(path);
    if (!chunks[id])
        game_log("AUDIO: load failed %s: %s\n", path, M.GetError());
    return chunks[id];
}

/* 音效：走通道，可叠加，不打断音乐 */
static void play_sfx(int id, int volume, int loop)
{
    MixChunk *c = chunk_for(id);
    if (!c)
        return;
    M.VolumeChunk(c, volume * MIX_MAX_VOLUME / 100);
    int ch = M.PlayChannel(-1, c, loop ? -1 : 0);
    if (ch >= 0 && id < AUDIO_MAX_ID)
        channels[id] = ch;
    if (sfx_plays++ < 10)
        game_log("AUDIO sfx id=%d vol=%d loop=%d ch=%d\n", id, volume, loop, ch);
}

/* 音乐：独占单实例，播新的先停旧的，等价 MediaPlayer.create + setLooping */
static void play_music(int id, int volume, int loop)
{
    if (music_id == id && music && M.PlayingMusic()) {
        M.VolumeMusic(volume * MIX_MAX_VOLUME / 100);
        return;
    }
    if (music) {
        M.HaltMusic();
        M.FreeMusic(music);
        music = NULL;
        music_id = -1;
    }
    char path[128];
    path_for(id, path, sizeof(path));
    if (access(path, R_OK)) {
        game_log("AUDIO: no music %s\n", path);
        return;
    }
    music = M.LoadMUS(path);
    if (!music) {
        game_log("AUDIO: music load failed %s: %s\n", path, M.GetError());
        return;
    }
    M.VolumeMusic(volume * MIX_MAX_VOLUME / 100);
    int rc = M.PlayMusic(music, loop ? -1 : 0);
    music_id = id;
    game_log("AUDIO music id=%d vol=%d loop=%d rc=%d\n", id, volume, loop, rc);
}

void audio_play(int id, int volume, int loop)
{
    if (!audio_ready)
        return;

    /* 原版：vol==0 且 loop 为真时不是"静音播放"，而是停止 */
    if (volume == 0 && loop) {
        audio_stop(-1);
        return;
    }

    int v = volume < 0 ? 0 : (volume > 100 ? 100 : volume);

    if (is_music(id))
        play_music(id, v, loop);
    else
        play_sfx(id, v, loop);
}

void audio_stop(int id)
{
    if (!audio_ready)
        return;
    if (id < 0) {
        M.HaltChannel(-1);
        if (music) {
            M.HaltMusic();
            M.FreeMusic(music);
            music = NULL;
            music_id = -1;
        }
        for (int i = 0; i < AUDIO_MAX_ID; i++)
            channels[i] = -1;
        return;
    }
    if (id < AUDIO_MAX_ID && channels[id] >= 0) {
        M.HaltChannel(channels[id]);
        channels[id] = -1;
    }
}

void audio_shutdown(void)
{
    if (!audio_ready)
        return;
    audio_stop(-1);
    for (int i = 0; i < AUDIO_MAX_ID; i++)
        if (chunks[i]) {
            M.FreeChunk(chunks[i]);
            chunks[i] = NULL;
        }
    M.CloseAudio();
    audio_ready = 0;
}
