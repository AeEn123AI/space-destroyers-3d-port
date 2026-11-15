/* Software mixer shared by both platforms: SFX voices + two lockstep music streams. */
#ifndef AUDIO_H
#define AUDIO_H
#include <stdint.h>

#define AUDIO_RATE 44100

enum { SND_POP, SND_POP_STAGE, SND_CRUNCH, SND_HIT, SND_LOSE, SND_EASTER, SND_COUNT };
enum { GRP_STAGE, GRP_THUMB, GRP_SPRITE, GRP_COUNT };

/* platform hooks (implemented per backend) */
void *plat_read_file(const char *name, long *size);   /* asset by file name, malloc'd */
void plat_audio_lock(void);
void plat_audio_unlock(void);

int  audio_init(void);                 /* loads + prepares all sounds; 0 on failure */
void audio_mix(int16_t *out, int frames);  /* stereo interleaved, AUDIO_RATE; call from audio thread */

/* game-side API (thread safe) */
void audio_play(int snd, int group);
void audio_stop_all(void);
void audio_music_start(int with_intro);
int  audio_music_playing(void);
int  audio_sound_playing(int snd);
void audio_set_group_volume(int group, float vol);   /* 0..1, ramped in the mixer */
void audio_set_master(float vol);

#endif
