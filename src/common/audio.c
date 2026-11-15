/* Software mixer: original MP3/WAV files decoded at runtime (minimp3 / dr_wav), no pre-resampling.
 * The two music streams (Stage: Intro->Main, Thumbnail: Options intro->Options select) are decoded in
 * lockstep into the same output buffer, so they can never drift apart. */
#include "audio.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_NO_SIMD_IF_NEEDED
#include "minimp3_ex.h"
#define DR_WAV_IMPLEMENTATION
#define DR_WAV_NO_STDIO
#include "dr_wav.h"

typedef struct {
    int is_mp3;
    int16_t *pcm; long frames; int rate;  /* WAV: fully decoded mono */
    void *file; long size;                /* MP3: compressed data, streamed */
} Sound;

typedef struct {
    int active, snd, group;
    double pos, step;                     /* WAV */
    mp3dec_ex_t dec; int dec_open;        /* MP3 */
    int16_t buf[2304 * 2]; int buf_len, buf_pos, mp3_ch;
    double frac; int16_t prev, cur;
} Voice;

enum { TR_INTRO, TR_MAIN, TR_OINTRO, TR_OSELECT, TR_COUNT };
static const char *track_files[TR_COUNT] = { "intro.mp3", "main.mp3", "options_intro.mp3", "options_select.mp3" };
static const char *snd_files[SND_COUNT] = { "pop.wav", "pop_stage.wav", "crunch.wav", "hit.wav", "lose.mp3", "easter_egg.mp3" };

static Sound sounds[SND_COUNT];
static struct { void *data; long size; } tracks[TR_COUNT];
#define MAX_VOICES 16
static Voice voices[MAX_VOICES];
static struct {
    int playing, intro;
    mp3dec_ex_t a, b; int open;
    float *tmp_a, *tmp_b;
} music;
static float group_target[GRP_COUNT] = { 1, 1, 1 }, group_cur[GRP_COUNT] = { 1, 1, 1 };
static float master = 1.0f;
static int ok;

static int load_wav(Sound *s, void *data, long size) {
    drwav w;
    if (!drwav_init_memory(&w, data, (size_t)size, NULL)) return 0;
    s->frames = (long)w.totalPCMFrameCount;
    s->rate = (int)w.sampleRate;
    int16_t *tmp = malloc(sizeof(int16_t) * s->frames * w.channels);
    drwav_read_pcm_frames_s16(&w, (drwav_uint64)s->frames, tmp);
    s->pcm = malloc(sizeof(int16_t) * s->frames);
    for (long i = 0; i < s->frames; i++) {
        int acc = 0;
        for (unsigned c = 0; c < w.channels; c++) acc += tmp[i * w.channels + c];
        s->pcm[i] = (int16_t)(acc / (int)w.channels);
    }
    free(tmp);
    drwav_uninit(&w);
    return 1;
}

int audio_init(void) {
    for (int i = 0; i < SND_COUNT; i++) {
        long size; void *d = plat_read_file(snd_files[i], &size);
        if (!d) return 0;
        if (strstr(snd_files[i], ".mp3")) { sounds[i].is_mp3 = 1; sounds[i].file = d; sounds[i].size = size; }
        else { if (!load_wav(&sounds[i], d, size)) return 0; free(d); }
    }
    for (int i = 0; i < TR_COUNT; i++) {
        tracks[i].data = plat_read_file(track_files[i], &tracks[i].size);
        if (!tracks[i].data) return 0;
    }
    music.tmp_a = malloc(sizeof(float) * 4096);
    music.tmp_b = malloc(sizeof(float) * 4096);
    ok = 1;
    return 1;
}

static void voice_stop(Voice *v) {
    if (v->dec_open) { mp3dec_ex_close(&v->dec); v->dec_open = 0; }
    v->active = 0;
}

void audio_play(int snd, int group) {
    if (!ok) return;
    plat_audio_lock();
    Voice *v = NULL;
    for (int i = 0; i < MAX_VOICES; i++) if (!voices[i].active) { v = &voices[i]; break; }
    if (!v) { v = &voices[0]; voice_stop(v); }
    /* Scratch restarts a sound that is already playing on the same target */
    for (int i = 0; i < MAX_VOICES; i++)
        if (voices[i].active && voices[i].snd == snd && voices[i].group == group) { v = &voices[i]; voice_stop(v); break; }
    memset(v, 0, sizeof *v);
    v->snd = snd; v->group = group;
    Sound *s = &sounds[snd];
    if (s->is_mp3) {
        if (mp3dec_ex_open_buf(&v->dec, s->file, (size_t)s->size, MP3D_SEEK_TO_SAMPLE) == 0) {
            v->dec_open = 1; v->mp3_ch = v->dec.info.channels;
            v->step = (double)v->dec.info.hz / AUDIO_RATE;
            v->active = 1;
        }
    } else {
        v->step = (double)s->rate / AUDIO_RATE;
        v->active = 1;
    }
    plat_audio_unlock();
}

int audio_sound_playing(int snd) {
    int r = 0;
    plat_audio_lock();
    for (int i = 0; i < MAX_VOICES; i++) if (voices[i].active && voices[i].snd == snd) r = 1;
    plat_audio_unlock();
    return r;
}

static void music_close(void) {
    if (music.open) { mp3dec_ex_close(&music.a); mp3dec_ex_close(&music.b); music.open = 0; }
    music.playing = 0;
}

static int music_open(int intro) {
    music_close();
    int ta = intro ? TR_INTRO : TR_MAIN, tb = intro ? TR_OINTRO : TR_OSELECT;
    if (mp3dec_ex_open_buf(&music.a, tracks[ta].data, (size_t)tracks[ta].size, MP3D_SEEK_TO_SAMPLE)) return 0;
    if (mp3dec_ex_open_buf(&music.b, tracks[tb].data, (size_t)tracks[tb].size, MP3D_SEEK_TO_SAMPLE)) { mp3dec_ex_close(&music.a); return 0; }
    music.open = 1; music.intro = intro; music.playing = 1;
    return 1;
}

void audio_stop_all(void) {
    plat_audio_lock();
    for (int i = 0; i < MAX_VOICES; i++) voice_stop(&voices[i]);
    music_close();
    plat_audio_unlock();
}

void audio_music_start(int with_intro) {
    if (!ok) return;
    plat_audio_lock();
    music_open(with_intro);
    plat_audio_unlock();
}

int audio_music_playing(void) { return music.playing; }
void audio_set_group_volume(int g, float v) { group_target[g] = v < 0 ? 0 : (v > 1 ? 1 : v); }
void audio_set_master(float v) { master = v; }

/* next mono sample of an MP3 voice at its native rate */
static int mp3_next(Voice *v, int16_t *out) {
    if (v->buf_pos >= v->buf_len) {
        mp3d_sample_t tmp[1152 * 2];
        size_t got = mp3dec_ex_read(&v->dec, tmp, 1152 * (size_t)v->mp3_ch);
        if (got == 0) return 0;
        v->buf_len = (int)(got / v->mp3_ch);
        for (int i = 0; i < v->buf_len; i++) {
            float s = 0;
            for (int c = 0; c < v->mp3_ch; c++) s += tmp[i * v->mp3_ch + c];
#ifdef MINIMP3_FLOAT_OUTPUT
            s *= 32767.0f;
#endif
            v->buf[i] = (int16_t)(s / v->mp3_ch);
        }
        v->buf_pos = 0;
    }
    *out = v->buf[v->buf_pos++];
    return 1;
}

static size_t music_read(mp3dec_ex_t *d, float *dst, size_t n) {
    mp3d_sample_t tmp[1024];
    size_t total = 0;
    while (total < n) {
        size_t want = n - total; if (want > 1024) want = 1024;
        size_t got = mp3dec_ex_read(d, tmp, want);
        for (size_t i = 0; i < got; i++) dst[total + i] = (float)tmp[i];
        total += got;
        if (got < want) break;
    }
    return total;
}

void audio_mix(int16_t *out, int frames) {
    plat_audio_lock();
    static float acc[4096];
    while (frames > 0) {
        int n = frames > 1024 ? 1024 : frames;
        for (int i = 0; i < n; i++) acc[i] = 0;
        /* ramp group volumes over this block */
        float g0[GRP_COUNT], gstep[GRP_COUNT];
        for (int g = 0; g < GRP_COUNT; g++) {
            float d = group_target[g] - group_cur[g];
            float maxd = 0.05f * n / 1024.0f * 8;   /* limit slope: no zipper noise */
            if (d > maxd) d = maxd;
            if (d < -maxd) d = -maxd;
            g0[g] = group_cur[g]; gstep[g] = d / n; group_cur[g] += d;
        }
        /* music: both streams read the same number of samples per block */
        if (music.playing && music.open) {
            int done = 0;
            while (done < n && music.playing) {
                size_t want = (size_t)(n - done);
                size_t ga = music_read(&music.a, music.tmp_a, want);
                size_t gb = music_read(&music.b, music.tmp_b, ga);
                for (size_t i = gb; i < ga; i++) music.tmp_b[i] = 0;
                for (size_t i = 0; i < ga; i++) {
                    int k = done + (int)i;
                    float va = g0[GRP_STAGE] + gstep[GRP_STAGE] * k, vb = g0[GRP_THUMB] + gstep[GRP_THUMB] * k;
                    acc[k] += music.tmp_a[i] * va + music.tmp_b[i] * vb;
                }
                done += (int)ga;
                if (ga < want) {
                    /* Stage stream ended: "Music looped" -> Main + Options select start together */
                    if (!music_open(0)) break;
                }
            }
        }
        /* sfx voices */
        for (int vi = 0; vi < MAX_VOICES; vi++) {
            Voice *v = &voices[vi];
            if (!v->active) continue;
            Sound *s = &sounds[v->snd];
            for (int i = 0; i < n; i++) {
                float smp;
                if (s->is_mp3) {
                    /* linear interpolation at native rate */
                    while (v->frac >= 0) {
                        v->prev = v->cur;
                        if (!mp3_next(v, &v->cur)) { voice_stop(v); break; }
                        v->frac -= 1.0;
                    }
                    if (!v->active) break;
                    double t = v->frac + 1.0;
                    smp = (float)(v->prev + (v->cur - v->prev) * t);
                    v->frac += v->step;
                } else {
                    long p = (long)v->pos;
                    if (p >= s->frames) { v->active = 0; break; }
                    double t = v->pos - p;
                    int16_t a = s->pcm[p], b = p + 1 < s->frames ? s->pcm[p + 1] : 0;
                    smp = (float)(a + (b - a) * t);
                    v->pos += v->step;
                }
                acc[i] += smp * (g0[v->group] + gstep[v->group] * i);
            }
        }
        for (int i = 0; i < n; i++) {
            float s = acc[i] * master;
            int x = (int)lrintf(s);
            if (x > 32767) x = 32767;
            if (x < -32768) x = -32768;
            out[2 * i] = out[2 * i + 1] = (int16_t)x;
        }
        out += 2 * n; frames -= n;
    }
    plat_audio_unlock();
}
