#include "chime_trace.h"
#include "backend.h"
#include "sound/chips.h"
#include "ram.h"

#include <stdio.h>
#include <stdlib.h>

/* The game task writes commands, chip writes and deliveries. The AHI worker
   has its own block array. Both finish before the report is formatted. */
struct chip_event {
	uint64_t cycles;
	uint8_t index, reg, value;
};
struct delivery {
	unsigned first, count, game_frame;
};
struct output_block {
	unsigned first, ready, used, frames;
	enum chime_phase phase;
	bool drained;
};
static struct capture {
	struct chip_event commands[256], registers[8192];
	struct delivery deliveries[2048];
	struct output_block blocks[1024];
} *capture;
#define ENTRIES(array) (sizeof(array) / sizeof((array)[0]))
static unsigned ncommands, nregisters, ndeliveries, nblocks;
static bool started, finished, aligned;
static unsigned source_offset, drive_start, frame;
volatile enum chime_phase chime_phase;

static void command(unsigned screen, uint8_t id)
{
	if (ncommands < ENTRIES(capture->commands))
		capture->commands[ncommands] = (struct chip_event){s32_board.sound_cycles, screen, 0, id};
	++ncommands;
}

static void pcm(uint64_t time, unsigned voice, unsigned reg, uint8_t value)
{
	if (nregisters < ENTRIES(capture->registers))
		capture->registers[nregisters] = (struct chip_event){time, voice, reg, value};
	++nregisters;
}

bool chime_trace_open(void)
{
	capture = calloc(1, sizeof(*capture));
	return capture != NULL;
}

void chime_trace_tick(void)
{
	if (!capture || finished) return;
	const struct screen *left = &game->screen_1;
	enum chime_phase phase = CHIME_IDLE;
	if (left->game_mode == 6 && left->in_game)
	{
		if (left->game_step == 7)
			phase = left->race_started ? CHIME_DRIVING : CHIME_GRID;
		else if (left->game_step == 23 || (left->race_running && !left->race_started))
			phase = CHIME_APPROACH;
	}
	frame = s32_board.frame;
	if (!started)
	{
		if (phase != CHIME_APPROACH && phase != CHIME_GRID) return;
		started = true;
		sound_command_seen = command;
		sound_pcm_write_seen = pcm;
	}
	if (phase == CHIME_DRIVING && chime_phase != CHIME_DRIVING)
		drive_start = frame;
	if (phase == CHIME_IDLE || (phase == CHIME_DRIVING && frame - drive_start >= RACE_START_DRIVING_TICKS))
	{
		phase = CHIME_IDLE;
		finished = true;
		sound_command_seen = NULL;
		sound_pcm_write_seen = NULL;
	}
	chime_phase = phase;
}

void chime_trace_published(unsigned end, unsigned count, uint64_t board_cycles)
{
	if (!aligned)
	{
		/* Benchmark fast-forward can skip deliveries before the capture. */
		source_offset = (unsigned)sound_pcm22_sample_at(board_cycles) - end;
		aligned = true;
	}
	if (ndeliveries < ENTRIES(capture->deliveries))
		capture->deliveries[ndeliveries] = (struct delivery){end - count, count, frame};
	++ndeliveries;
}

void chime_trace_block(enum chime_phase phase, unsigned first, unsigned ready,
	unsigned used, unsigned frames, bool drained)
{
	if (nblocks < ENTRIES(capture->blocks))
		capture->blocks[nblocks] = (struct output_block){first, ready, used, frames, phase, drained};
	++nblocks;
}

void chime_trace_report(void)
{
	if (!capture) return;
	sound_command_seen = NULL;
	sound_pcm_write_seen = NULL;
	chime_phase = CHIME_IDLE;
	printf("CHIMETRACE: %s; 22050 stereo frames/second\n",
		finished ? "finished" : started ? "stopped during capture" : "grid not reached");
	if (aligned)
	{
		puts("game_frame,fx_first,fx_delivered");
		for (unsigned i = 0; i < ndeliveries && i < ENTRIES(capture->deliveries); ++i)
		{
			const struct delivery *d = &capture->deliveries[i];
			printf("%u,%u,%u\n", d->game_frame, source_offset + d->first, d->count);
		}
		puts("phase,out_first,fx_first,fx_ready,fx_used,first_gap_out,fx_missing,fx_held,ahi_drain");
		unsigned out = 0;
		for (unsigned i = 0; i < nblocks && i < ENTRIES(capture->blocks); ++i)
		{
			const struct output_block *b = &capture->blocks[i];
			const unsigned available = b->ready < b->frames ? b->ready : b->frames;
			static const char *const names[] = {"idle", "approach", "grid", "driving"};
			printf("%s,%u,%u,%u,%u,", names[b->phase],
				out, source_offset + b->first, b->ready, b->used);
			if (b->used < b->frames) printf("%u", out + b->used);
			printf(",%u,%u,%u\n", b->frames - available, available - b->used, b->drained);
			out += b->frames;
		}
	}
	puts("fx_sample,screen,command_hex");
	for (unsigned i = 0; i < ncommands && i < ENTRIES(capture->commands); ++i)
	{
		const struct chip_event *e = &capture->commands[i];
		printf("%u,%u,%02X\n", (unsigned)sound_pcm22_sample_at(e->cycles), e->index, e->value);
	}
	puts("fx_sample,pcm_voice,register,value_hex");
	for (unsigned i = 0; i < nregisters && i < ENTRIES(capture->registers); ++i)
	{
		const struct chip_event *e = &capture->registers[i];
		printf("%u,%u,%u,%02X\n", (unsigned)sound_pcm22_sample_at(e->cycles), e->index, e->reg, e->value);
	}
	printf("Trace rows dropped: commands %u, PCM %u, deliveries %u, output %u\n",
		(unsigned)(ncommands > ENTRIES(capture->commands) ? ncommands - ENTRIES(capture->commands) : 0),
		(unsigned)(nregisters > ENTRIES(capture->registers) ? nregisters - ENTRIES(capture->registers) : 0),
		(unsigned)(ndeliveries > ENTRIES(capture->deliveries) ? ndeliveries - ENTRIES(capture->deliveries) : 0),
		(unsigned)(nblocks > ENTRIES(capture->blocks) ? nblocks - ENTRIES(capture->blocks) : 0));
	free(capture);
	capture = NULL;
}
