/**
 *    Name:        chips.h
 *    Description: the sound board's chips and the Z80 program on the board's time (sound_chips.c, sound_cpu.c)
 *    Version:     0.1.0
 *    Created:     13-10-2026
 *    Modified:    29-10-2026
 *    Author:      Sebastian Toyos
 *
 *    license:BSD-3-Clause
 *    A port sets s32_board.sound_rom and s32_board.pcm_rom, calls sound_chips_attach() and
 *    sound_cpu_attach(), and at the end of every frame sound_chips_end_frame() with the board's time: the
 *    frame's samples are then there to play, each chip at its own rate (the YM3438 every 360 board cycles,
 *    the MultiPCM every 448).
 */
#ifndef SOUND_CHIPS_H
#define SOUND_CHIPS_H

#include "s32/machine.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The YM3438 and the MultiPCM on s32_board.chips */
void sound_chips_attach(void);

/* Playback for OutRunners, which uses the YM3438 only for timing. Produces
   the stereo frame mix; use the regular attach for chip samples and hashes. */
void sound_chips_attach_pcm_playback(void);
/* The caller owns the 2 MiB signed, half-rate PCM bank for the playback session. */
void sound_chips_attach_pcm22(const int8_t *bank);
/* The streaming backend combines these wide effects with decoded music before limiting. */
void sound_chips_wide_output(bool enable, bool arrange_music);
/* Select a single summed PCM22 effects channel for the stereo streaming mixer.
   wide_mix returns one value per frame in this mode, two for wide_output. */
void sound_chips_mono_effects(void);
const int32_t *sound_chips_wide_mix(size_t *frames);
void sound_compress_pair(int32_t *gain, int16_t *out, int32_t left, int32_t right);

/* A music player can be replaced by an external recording. The sequencer still
   handles timers, patches and playlist control. Other players own their notes. */
extern bool (*sound_music_command)(unsigned screen, uint8_t command);
extern void (*sound_music_fade)(unsigned screen, uint8_t volume);
/* Optional diagnostic observers. PCM writes cover sample, pitch, key and level. */
extern void (*sound_command_seen)(unsigned screen, uint8_t command);
extern void (*sound_pcm_write_seen)(uint64_t time, unsigned voice, unsigned reg, uint8_t value);
uint64_t sound_pcm22_sample_at(uint64_t board_cycles);
extern void (*sound_song_started)(unsigned player, uint8_t song, uint16_t next);

/* The decompiled sound program on the board's time (sound_cpu.c) */
void sound_cpu_attach(void);

/* The YM3438 timers due by 'time' run out (sound_cpu.c calls it where a block starts) */
extern uint64_t sound_next_timer; /* earliest board-cycle deadline, UINT64_MAX when idle */
void sound_chips_timers_due(uint64_t time);

/* The MultiPCM makes the samples due by now, before the board changes its banks */
void sound_chips_pcm_banks_changing(void);

/* The frame ends at 'time': the samples that start before it are the frame's */
void sound_chips_end_frame(uint64_t time);

/* The frame's samples of chip 0 (the YM3438) or 1 (the MultiPCM), its two outputs interleaved */
const int32_t *sound_chips_frame_samples(int chip, size_t *count);

/* The frame's sound, mixed as tools/core/system32/reference/sound mixes it: stereo at the YM3438's rate */
const int16_t *sound_chips_frame_mix(size_t *frames);
uint32_t sound_chips_mix_rate(void);

/* The mix's speakers: apart (each chip's second output to the left speaker, its first to the right one,
   as the cabinet's sides have them: the arcade's), or together (both outputs to both speakers: one sound
   for the two sides, a port's console build) */
void sound_chips_join_sides(bool together);

/* FNV-1a hashes, as tools/core/system32/reference/s32ref writes them: the frame's samples of the two chips, and the
   sound board's memory (the shared RAM but the Z80's stack, FD00-FDFF) */
uint64_t sound_chips_frame_hash(void);
uint64_t sound_board_state_hash(void);

#ifdef __cplusplus
}
#endif

#endif
