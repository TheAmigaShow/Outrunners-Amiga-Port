#ifndef ORUNNERS_AMIGA_BACKEND_H
#define ORUNNERS_AMIGA_BACKEND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool amiga_loading_open(void);
void amiga_loading_line(const char *text);
void amiga_loading_wait_return(void);
void amiga_loading_close(void);

enum amiga_display_mode { AMIGA_AGA, AMIGA_RTG, AMIGA_P96 };
const char *amiga_display_open(enum amiga_display_mode requested);
const char *amiga_display_test(void);
void amiga_display_target(bool half);
void amiga_display_close(void);
bool amiga_display_frame(const uint32_t *pixels);
bool amiga_display_events(bool keys[128]);

enum { FX_RECOVERY_NORMAL = 768, FX_RECOVERY_SHORT = 512 };
/* Duration of the detailed race-start chime trace. */
enum { RACE_START_DRIVING_TICKS = 300 };
bool amiga_music_effects_low(void);
int amiga_music_start(const char *assets, unsigned recovery_frames, bool auto_gain);
bool amiga_music_effects(const int32_t *samples, size_t frames);
void amiga_music_close(void);

struct amiga_audio_stats {
	uint32_t writes, output_frames, drains, short_blocks, missing_fx, held_fx;
	unsigned peak;
	uint64_t sum_squares;
	/* Only the producer updates delivery timing. */
	uint32_t delivery_intervals, late_deliveries;
	uint64_t delivery_ticks, longest_delivery_ticks;
};
void amiga_audio_delivery(struct amiga_audio_stats *row);
/* Selected by the game task. The AHI writer owns the output counters. */
extern struct amiga_audio_stats *volatile amiga_audio_stats_row;

bool amiga_audio_open(bool compact);
bool amiga_audio_queue(const int16_t *samples, size_t frames, unsigned missing_fx, unsigned held_fx);
uint32_t amiga_audio_drained(void);
unsigned amiga_audio_last_peak8(void);
void amiga_audio_close(void);

#endif
