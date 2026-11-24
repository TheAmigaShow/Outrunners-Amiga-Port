/**
 *    Name:        sound_chips.c
 *    Description: the sound board's chips on the board's time: the YM3438 and the MultiPCM, their samples
 *                 frame by frame
 *    Version:     0.1.0
 *    Created:     13-10-2026
 *    Modified:    13-11-2026
 *    Author:      Sebastian Toyos
 *
 *    license:BSD-3-Clause
 *    Plugs runtime/src/ym3438.c and multipcm.c into the Z80's side of the board (s32_board.chips). The
 *    chips keep no time: before every write they make the samples that start before it, at the time the
 *    Z80's block makes its accesses (s32_board.sound_cycles, sound_cpu.c), and the timers they ask for run
 *    out where sound_cpu.c says (sound_chips_timers_due). Sample n of the YM3438 starts at n x 360 board
 *    cycles (144 of its clocks, at 8 MHz), the MultiPCM's at n x 448 (224 of its clocks, at 10 MHz).
 *    tools/core/system32/reference/sound/s32sound.cpp does the same with ymfm's YM3438 and MAME's MultiPCM: the frame's
 *    hashes compare. OutRunners uses the YM3438 for its timers only: it plays no note.
 */
#include "sound/chips.h"

#include "s32/multipcm.h"
#include "s32/ym3438.h"
#include "s32/byte_order.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FM_PERIOD           (144ull * S32_CPU_CLOCK / S32_SOUND_CLOCK)     /* board cycles a YM3438 sample takes */
#define PCM_PERIOD          (224ull * S32_CPU_CLOCK / S32_MULTIPCM_CLOCK)  /* ... and a MultiPCM one */
#define NO_TIMER            UINT64_MAX
#define STACK_START         0xFD00u                                     /* the Z80's stack, FD00-FDFF: not in the hash */
#define STACK_END           0xFE00u

/* The samples a chip has made and the frame has not taken yet: its two outputs, from sample 'first' on */
typedef struct sample_buffer
{
	int32_t *data;
	size_t count;           /* values (two a sample) */
	size_t capacity;
	uint64_t first;
} sample_buffer;

static struct
{
	s32_fm fm;
	uint64_t fm_samples;                /* made so far */
	uint64_t deadline[2];               /* when its timers run out, board cycles */
	uint64_t busy_until;
	sample_buffer fm_buffer;
	multipcm pcm;
	uint64_t pcm_samples;
	sample_buffer pcm_buffer;
	uint64_t override;                  /* while a timer runs out: its own time */
	uint64_t frame_hash;
	int32_t *frame[2];                  /* the frame's samples, for the mix */
	size_t frame_counts[2];
	size_t frame_capacity[2];
	int16_t *mix;
	size_t mix_capacity;
	size_t mix_frames;
} sound = { .override = NO_TIMER };

static bool pcm_playback;
static const int8_t *compact_bank;
static multipcm_compact compact_pcm;
static unsigned wide_channels;
static int32_t *wide_mix;
static size_t wide_capacity;
static int32_t compressor_gain;     /* Q14, linked across both speakers */
static uint64_t mixed_until;  /* output sample number at the last frame boundary */

static bool sides_together;    /* sound_chips_join_sides */

static uint64_t now(void)
{
	return sound.override != NO_TIMER ? sound.override : s32_board.sound_cycles;
}

/* board cycles 'clocks' of the YM3438 (which runs on the Z80's clock) take, rounded up */
static uint64_t fm_span(uint64_t clocks)
{
	return (clocks * S32_CPU_CLOCK + S32_SOUND_CLOCK - 1) / S32_SOUND_CLOCK;
}

static void append(sample_buffer *buffer, int32_t first, int32_t second)
{
	if (buffer->count + 2 > buffer->capacity)
	{
		buffer->capacity = buffer->capacity ? buffer->capacity * 2 : 4096;
		buffer->data = (int32_t *)realloc(buffer->data, buffer->capacity * sizeof(int32_t));
	}
	buffer->data[buffer->count++] = first;
	buffer->data[buffer->count++] = second;
}

/* ---- the YM3438 ---- */

static void fm_catch_up(uint64_t time)
{
	const uint64_t due = (time + FM_PERIOD - 1) / FM_PERIOD;
	int32_t out[2];

	if (pcm_playback)
	{
		/* Timer B uses the low four bits of the free-running sample clock. */
		if (due > sound.fm_samples)
		{
			sound.fm.total_clocks += (uint8_t)(due - sound.fm_samples);
			sound.fm_samples = due;
		}
		return;
	}
	while (sound.fm_samples < due)
	{
		s32_fm_generate(&sound.fm, out);
		append(&sound.fm_buffer, out[0], out[1]);
		sound.fm_samples++;
	}
}

uint64_t sound_next_timer = NO_TIMER;

static void set_deadline(int timer, uint64_t deadline)
{
	sound.deadline[timer] = deadline;
	sound_next_timer = deadline < sound.deadline[timer ^ 1] ? deadline : sound.deadline[timer ^ 1];
}

static void fm_set_timer(void *opaque, int timer, int32_t clocks)
{
	(void)opaque;
	set_deadline(timer, clocks < 0 ? NO_TIMER : now() + fm_span((uint64_t)clocks));
}

static void fm_update_irq(void *opaque, bool asserted)
{
	(void)opaque;
	if (asserted)
		s32_sound_signal_irq(S32_SOUND_IRQ_FM);
	else
		s32_sound_clear_irq(S32_SOUND_IRQ_FM);
}

static void fm_set_busy_end(void *opaque, uint32_t clocks)
{
	(void)opaque;
	sound.busy_until = now() + fm_span(clocks);
}

static bool fm_is_busy(void *opaque)
{
	(void)opaque;
	return now() < sound.busy_until;
}

void sound_chips_timers_due(uint64_t time)
{
	/* the timers due by 'time', in the order they run out */
	while (sound_next_timer <= time && sound_next_timer != NO_TIMER)
	{
		const int timer = sound.deadline[0] <= sound.deadline[1] ? 0 : 1;
		const uint64_t when = sound.deadline[timer];

		set_deadline(timer, NO_TIMER);
		fm_catch_up(when);
		sound.override = when;
		s32_fm_timer_expired(&sound.fm, timer);
		sound.override = NO_TIMER;
	}
}

static uint8_t fm_read(int offset)
{
	return s32_fm_read(&sound.fm, offset);
}

static void fm_write(int offset, uint8_t data)
{
	fm_catch_up(now());
	s32_fm_write(&sound.fm, offset, data);
}

/* ---- the MultiPCM ---- */

static void pcm_catch_up(uint64_t time)
{
	const uint64_t due = (time + PCM_PERIOD - 1) / PCM_PERIOD;
	int32_t first[256];
	int32_t second[256];

	while (sound.pcm_samples < due)
	{
		const int count = due - sound.pcm_samples > 256 ? 256 : (int)(due - sound.pcm_samples);

		int produced = count;
		if (compact_bank)
		{
			compact_pcm.page[2] = compact_bank + (s32_board.pcm_bank & 7u) * 0x40000;
			compact_pcm.page[3] = compact_bank + ((s32_board.pcm_bank >> 3) & 7u) * 0x40000;
			produced = multipcm_generate_compact(&sound.pcm, &compact_pcm, first, second, count);
		}
		else
			multipcm_generate(&sound.pcm, first, second, count);
		for (int index = 0; index < produced; index++)
			append(&sound.pcm_buffer, first[index], second[index]);
		sound.pcm_samples += (uint64_t)count;
	}
}

static uint8_t pcm_read(int offset)
{
	(void)offset;
	return multipcm_read(&sound.pcm);
}

void (*sound_pcm_write_seen)(uint64_t, unsigned, unsigned, uint8_t);

static void pcm_write(int offset, uint8_t data)
{
	/* Voice and register selectors have no audible effect until a data write. */
	if (offset == 0)
	{
		const uint64_t time = now();
		pcm_catch_up(time);
		const unsigned reg = sound.pcm.address;
		if (sound_pcm_write_seen && sound.pcm.current_voice >= 0 && reg >= 1 && reg <= 5)
			sound_pcm_write_seen(time, sound.pcm.current_voice, reg, data);
	}
	multipcm_write(&sound.pcm, offset, data);
}

void sound_chips_pcm_banks_changing(void)
{
	pcm_catch_up(now());
}

/* ---- frames ---- */

static uint64_t hash_bytes(const void *data, size_t size, uint64_t hash)
{
	const uint8_t *bytes = (const uint8_t *)data;

	for (size_t index = 0; index < size; index++)
		hash = (hash ^ bytes[index]) * 0x100000001b3ull;
	return hash;
}

/* 32-bit samples hashed by their bytes low first, as the PC holds them, on a processor of either order */
static uint64_t hash_samples(const int32_t *samples, size_t count, uint64_t hash)
{
	for (size_t index = 0; index < count; index++)
		for (int shift = 0; shift < 32; shift += 8)
			hash = (hash ^ (uint8_t)((uint32_t)samples[index] >> shift)) * 0x100000001b3ull;
	return hash;
}

/* the samples before sample number 'due': hashed and moved to the frame's list 'which' */
static uint64_t take(sample_buffer *buffer, uint64_t due, uint64_t hash, int which)
{
	size_t values = (size_t)(due - buffer->first) * 2;

	if (values > buffer->count)
		values = buffer->count;
	if (values > sound.frame_capacity[which])
	{
		sound.frame_capacity[which] = values;
		sound.frame[which] = (int32_t *)realloc(sound.frame[which], values * sizeof(int32_t));
	}
	if (values)
		memcpy(sound.frame[which], buffer->data, values * sizeof(int32_t));
	sound.frame_counts[which] = values;
	memmove(buffer->data, buffer->data + values, (buffer->count - values) * sizeof(int32_t));
	buffer->count -= values;
	buffer->first += values / 2;
	return hash_samples(sound.frame[which], values, hash);
}

static int16_t clamp16(double value)
{
	return (int16_t)(value < -32768.0 ? -32768.0 : (value > 32767.0 ? 32767.0 : value));
}

/* The mix at the YM3438's rate: the MultiPCM's latest sample at each of its samples' start; each chip's
   second output goes to the left speaker, its first to the right one (MAME's routes), or both to both
   (sound_chips_join_sides: the program pans each side to one output only, so the sum is each side whole) */
static void mix(void)
{
	const size_t frames = sound.frame_counts[0] / 2;
	const size_t pcm_frames = sound.frame_counts[1] / 2;

	if (frames * 2 > sound.mix_capacity)
	{
		sound.mix_capacity = frames * 2;
		sound.mix = (int16_t *)realloc(sound.mix, sound.mix_capacity * sizeof(int16_t));
	}
	for (size_t index = 0; index < frames; index++)
	{
		size_t at = 0;

		if (pcm_frames)
		{
			at = index * pcm_frames / (frames ? frames : 1);
			if (at > pcm_frames - 1)
				at = pcm_frames - 1;
		}
		for (int side = 0; side < 2; side++)
		{
			const int output = 1 - side;
			double fm_part = 0.15 * sound.frame[0][index * 2 + (size_t)output];
			double pcm_part = pcm_frames ? 0.35 * sound.frame[1][at * 2 + (size_t)output] : 0.0;

			if (sides_together)
			{
				fm_part += 0.15 * sound.frame[0][index * 2 + (size_t)side];
				pcm_part += pcm_frames ? 0.35 * sound.frame[1][at * 2 + (size_t)side] : 0.0;
			}
			sound.mix[index * 2 + (size_t)side] = clamp16(fm_part + pcm_part);
		}
	}
	sound.mix_frames = frames;
}

/* Ceil(time * 22050 / 20000000), with a bounded 32-bit remainder. */
uint64_t sound_pcm22_sample_at(uint64_t time)
{
	return time / 400000u * 441u + ((uint32_t)(time % 400000u) * 441u + 399999u) / 400000u;
}

void sound_compress_pair(int32_t *gain, int16_t *out, int32_t left, int32_t right)
{
	const int32_t peak = abs(left) > abs(right) ? abs(left) : abs(right);
	if (*gain == 16384 && peak <= 16384)
	{
		out[0] = (int16_t)left;
		out[1] = (int16_t)right;
		return;
	}
	int32_t target = 16384;
	if (peak > 16384)
	{
		/* Halve the amplitude excursion above half scale. Leave room for 8-bit rounding. */
		int32_t level = 16384 + (peak - 16384) / 2;
		if (level > 32512) level = 32512;
		target = (level << 14) / peak;
	}
	/* Immediate attack. Release towards unity over about 46 ms at 22.05 kHz. */
	(*gain) += (16384 - (*gain) + 1023) >> 10;
	if ((*gain) > target) (*gain) = target;
	out[0] = (int16_t)((left * (*gain)) >> 14);
	out[1] = (int16_t)((right * (*gain)) >> 14);
}

/* The board sends PCM at 35% gain. Keep the wide PCM22 sum through the compressor. */
static void mix_pcm_playback(uint64_t time)
{
	sample_buffer *buffer = &sound.pcm_buffer;
	const uint64_t pcm_due = compact_bank ? sound_pcm22_sample_at(time) : (time + PCM_PERIOD - 1) / PCM_PERIOD;
	const uint64_t out_due = compact_bank ? pcm_due : (time + FM_PERIOD - 1) / FM_PERIOD;
	const size_t frames = (size_t)(out_due - mixed_until);
	const size_t available = buffer->count / 2;
	size_t count = (size_t)(pcm_due - buffer->first);
	if (count > available)
		count = available;

	if (wide_channels && frames * wide_channels > wide_capacity)
	{
		int32_t *grown = realloc(wide_mix, frames * wide_channels * sizeof(*wide_mix));
		if (!grown) { fputs("Out of memory for the effects mix\n", stderr); exit(3); }
		wide_mix = grown;
		wide_capacity = frames * wide_channels;
	}
	if (!wide_channels && frames * 2 > sound.mix_capacity)
	{
		int16_t *samples = realloc(sound.mix, frames * 2 * sizeof(*samples));
		if (!samples)
		{
			fputs("Out of memory for the stereo mix\n", stderr);
			exit(3);
		}
		sound.mix = samples;
		sound.mix_capacity = frames * 2;
	}

	/* Same nearest-sample positions as floor(i * count / frames). */
	size_t phase = 0, at = 0;
	for (size_t i = 0; i < frames; ++i)
	{
		int32_t left = count ? buffer->data[2 * at + 1] : 0;
		int32_t right = count ? buffer->data[2 * at] : 0;
		if (wide_channels == 1)
			wide_mix[i] = (left + right) * 7 / 20;
		else if (compact_bank)
		{
			if (sides_together) left = right = left + right;
			left = left * 7 / 20;
			right = right * 7 / 20;
			if (wide_channels)
			{
				wide_mix[2*i] = left;
				wide_mix[2*i+1] = right;
			}
			else sound_compress_pair(&compressor_gain, &sound.mix[2*i], left, right);
		}
		else if (sides_together)
		{
			int16_t sample = (int16_t)((left + right) * 7 / 20);
			sound.mix[2 * i] = sound.mix[2 * i + 1] = sample;
		}
		else
		{
			sound.mix[2 * i] = (int16_t)(left * 7 / 20);
			sound.mix[2 * i + 1] = (int16_t)(right * 7 / 20);
		}
		phase += count;
		while (phase >= frames)
		{
			phase -= frames;
			++at;
		}
	}

	buffer->count -= 2 * count;
	if (buffer->count)
		memmove(buffer->data, buffer->data + 2 * count, buffer->count * sizeof(*buffer->data));
	buffer->first += count;
	sound.mix_frames = frames;
	mixed_until = out_due;
}

void sound_chips_end_frame(uint64_t time)
{
	uint64_t hash = 0xcbf29ce484222325ull;

	fm_catch_up(time);
	pcm_catch_up(time);
	if (pcm_playback)
	{
		mix_pcm_playback(time);
		return;
	}
	hash = take(&sound.fm_buffer, (time + FM_PERIOD - 1) / FM_PERIOD, hash, 0);
	sound.frame_hash = take(&sound.pcm_buffer, (time + PCM_PERIOD - 1) / PCM_PERIOD, hash, 1);
	mix();
}

uint64_t sound_chips_frame_hash(void)
{
	return sound.frame_hash;
}

uint64_t sound_board_state_hash(void)
{
	const uint8_t *ram = s32_order_v70(s32_board.shared_ram);    /* (as the Z80 holds it: s32/byte_order.h) */
	uint64_t hash = 0xcbf29ce484222325ull;

	hash = hash_bytes(ram, STACK_START - 0xE000u, hash);
	return hash_bytes(ram + (STACK_END - 0xE000u), 0x10000u - STACK_END, hash);
}

const int32_t *sound_chips_frame_samples(int chip, size_t *count)
{
	if (count)
		*count = sound.frame_counts[chip & 1] / 2;
	return sound.frame[chip & 1];
}

const int16_t *sound_chips_frame_mix(size_t *frames)
{
	if (frames)
		*frames = wide_channels ? 0 : sound.mix_frames;
	return wide_channels ? NULL : sound.mix;
}

uint32_t sound_chips_mix_rate(void)
{
	return compact_bank ? MULTIPCM_COMPACT_RATE : S32_SOUND_CLOCK / 144;
}

void sound_chips_join_sides(bool together)
{
	sides_together = together;
}

void sound_chips_attach(void)
{
	pcm_playback = false;
	compact_bank = NULL;
	wide_channels = 0;
	compact_pcm.arrange_stereo = false;
	sound.fm_buffer.count = sound.pcm_buffer.count = sound.mix_frames = 0;
	memset(&sound.fm, 0, sizeof(sound.fm));
	sound_next_timer = sound.deadline[0] = sound.deadline[1] = NO_TIMER;
	sound.fm.host = NULL;
	sound.fm.set_timer = fm_set_timer;
	sound.fm.update_irq = fm_update_irq;
	sound.fm.set_busy_end = fm_set_busy_end;
	sound.fm.is_busy = fm_is_busy;
	s32_fm_init(&sound.fm);
	multipcm_init(&sound.pcm, S32_MULTIPCM_CLOCK, s32_pcm_sample_read);
	multipcm_reset(&sound.pcm);
	sound.fm_samples = sound.pcm_samples = 0;
	sound.fm_buffer.first = sound.pcm_buffer.first = 0;
	s32_board.chips.fm_read = fm_read;
	s32_board.chips.fm_write = fm_write;
	s32_board.chips.pcm_read = pcm_read;
	s32_board.chips.pcm_write = pcm_write;
}

void sound_chips_attach_pcm_playback(void)
{
	sound_chips_attach();
	mixed_until = 0;
	pcm_playback = true;
}

void sound_chips_attach_pcm22(const int8_t *bank)
{
	sound_chips_attach_pcm_playback();
	compact_bank = bank;
	compact_pcm.phase = 0;
	compact_pcm.page[0] = bank;
	compact_pcm.page[1] = bank + 0x40000;
	compressor_gain = 16384;
}

void sound_chips_wide_output(bool enable, bool arrange_music)
{
	wide_channels = enable && compact_bank ? 2 : 0;
	compact_pcm.arrange_stereo = arrange_music;
}

void sound_chips_mono_effects(void)
{
	wide_channels = compact_bank ? 1 : 0;
	compact_pcm.arrange_stereo = false;
}

const int32_t *sound_chips_wide_mix(size_t *frames)
{
	*frames = wide_channels ? sound.mix_frames : 0;
	return wide_mix;
}
