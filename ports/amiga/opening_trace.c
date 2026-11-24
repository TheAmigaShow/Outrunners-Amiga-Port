#include "opening_trace.h"
#include "backend.h"
#include <proto/timer.h>
#include <stdio.h>
#include <stdlib.h>

/* 1292 stereo blocks at 22050 Hz cover the first 30 seconds of output.
   The game and AHI worker each own their table. Join the worker before reporting. */
enum { MAX_BLOCKS = 1292, MAX_FRAMES = 2048 };
struct opening_frame {
	unsigned at, board, mode, mode_step, game_step, work[OPEN_WORK_COUNT];
	bool fading, skipped;
};
struct opening_block {
	unsigned start, mixed, queued, first, ready, used, frames;
	bool drained, music;
	uint8_t peak8;
};
static struct {
	struct opening_frame frames[MAX_FRAMES];
	struct opening_block blocks[MAX_BLOCKS];
} *record;
volatile bool opening_active;
static unsigned frequency, origin, nframes, nblocks;

bool opening_trace_open(void)
{
	struct EClockVal e;
	record = calloc(1, sizeof(*record));
	if (!record) return false;
	frequency = ReadEClock(&e);
	origin = e.ev_lo;
	opening_active = true;
	return true;
}
uint32_t opening_clock(void)
{
	struct EClockVal e;
	ReadEClock(&e);
	return e.ev_lo;
}
void opening_frame(unsigned board_frame, unsigned mode, unsigned mode_step, unsigned game_step)
{
	if (++nframes > MAX_FRAMES) return;
	record->frames[nframes-1] = (struct opening_frame){
		.at = opening_clock(), .board = board_frame, .mode = mode,
		.mode_step = mode_step, .game_step = game_step
	};
}
void opening_draw_result(bool fading, bool skipped)
{
	if (nframes && nframes <= MAX_FRAMES)
	{
		record->frames[nframes-1].fading = fading;
		record->frames[nframes-1].skipped = skipped;
	}
}
void opening_work_done(enum opening_work part, uint32_t start)
{
	if (nframes && nframes <= MAX_FRAMES)
		record->frames[nframes-1].work[part] += opening_clock() - start;
}
void opening_block(unsigned first, unsigned ready, unsigned used, unsigned frames,
	bool drained, bool music, uint32_t start, uint32_t mixed)
{
	record->blocks[nblocks++] = (struct opening_block){
		start, mixed, opening_clock(), first, ready, used, frames, drained, music, amiga_audio_last_peak8()
	};
	if (nblocks == MAX_BLOCKS) opening_active = false;
}
static unsigned us(unsigned ticks)
{
	return (uint64_t)ticks * 1000000 / frequency;
}
void opening_report(void)
{
	if (!record) return;
	opening_active = false;
	printf("OPENINGTRACE: %u blocks, %u game frames, %u frame rows omitted\n",
		nblocks, nframes, nframes > MAX_FRAMES ? nframes - MAX_FRAMES : 0);
	puts("block,start_us,prepare_us,queue_us,music_selected,fx_published,fx_ready,fx_used,fx_missing,fx_held,ahi_drain,output_peak8");
	for (unsigned i = 0; i < nblocks; ++i)
	{
		const struct opening_block *b = &record->blocks[i];
		const unsigned available = b->ready < b->frames ? b->ready : b->frames;
		printf("%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u\n", i, us(b->start-origin),
			us(b->mixed-b->start), us(b->queued-b->mixed), b->music,
			b->first+b->ready, b->ready, b->used, b->frames-available, available-b->used, b->drained, b->peak8);
	}
	puts("game_frame,start_us,game_mode,mode_step,game_step,draw_us,sprites_us,sound_end_us,present_us,fade_active,draw_skipped");
	for (unsigned i = 0; i < nframes && i < MAX_FRAMES; ++i)
	{
		const struct opening_frame *f = &record->frames[i];
		printf("%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u\n", f->board, us(f->at-origin), f->mode,
			f->mode_step, f->game_step, us(f->work[OPEN_DRAW]), us(f->work[OPEN_SPRITES]),
			us(f->work[OPEN_SOUND_END]), us(f->work[OPEN_PRESENT]), f->fading, f->skipped);
	}
	free(record);
	record = NULL;
}
