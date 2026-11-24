/* Desktop preparation tool. Run the actual sound board in isolation, producing
   a stereo arrangement of a single cabinet's music without car or FX commands. */
#include "game/assets.h"
#include "sound/chips.h"
#include "s32/machine.h"
#include <stdio.h>
#include <stdlib.h>

static uint64_t origin, end_sample, loop_sample;
static bool started, looping;
static struct { unsigned next, song; uint64_t sample; } visits[512];
static unsigned visit_count;

static uint64_t sample_time(uint64_t ticks)
{
	return (ticks * 441 + 399999) / 400000;
}

static void song_started(unsigned player, uint8_t song, uint16_t next)
{
	if (player != 0xe900 || end_sample) return;
	const uint64_t sample = sample_time(s32_board.sound_cycles);
	if (!started) { started = true; origin = sample; }
	for (unsigned i = 0; i < visit_count; i++)
		if (visits[i].next == next && visits[i].song == song)
		{
			loop_sample = visits[i].sample - origin;
			end_sample = sample;
			looping = true;
			return;
		}
	if (visit_count < 512)
	{
		visits[visit_count].next = next;
		visits[visit_count].song = song;
		visits[visit_count++].sample = sample;
	}
}

int main(int argc, char **argv)
{
	if (argc != 4) { fputs("Usage: orunners_amiga_music assets command output.s16le\n", stderr); return 1; }
	int command = atoi(argv[2]);
	if ((command < 1 || command > 18) && (command < 0xe0 || command > 0xf4)) return 1;
	const char *error = assets_load(argv[1]);
	if (error) { fprintf(stderr, "%s\n", error); return 1; }
	int8_t *bank = assets_load_pcm22(argv[1]);
	if (!bank) { fputs("Run prepare_pcm22.py first\n", stderr); return 1; }
	FILE *out = fopen(argv[3], "wb");
	if (!out) return 1;
	s32_init(NULL);
	s32_board.sound_at = assets_sound_data;
	s32_board.sound_byte_at = assets_sound_byte;
	s32_board.pcm_at = assets_pcm_byte;
	sound_chips_attach_pcm22(bank);
	sound_cpu_attach();
	sound_chips_join_sides(false);
	sound_chips_wide_output(true, true);
	s32_board.sound_reset_changed(false);
	uint64_t produced = 0, written = 0;
	for (unsigned frame = 0; frame < 60 * 60 * 10; frame++)
	{
		if (frame == 30)
		{
			sound_song_started = song_started;
			s32_z80_write(0xe700, command);
		}
		s32_board.cycles = s32_frame_start(frame+1);
		s32_board.time_passes(s32_board.cycles);
		sound_chips_end_frame(s32_board.cycles);
		if (started && !end_sample && !(s32_z80_read(0xe900) & 0x80))
			end_sample = sample_time(s32_board.cycles) + 5513;
		size_t count;
		const int32_t *mix = sound_chips_wide_mix(&count);
		uint8_t block[4 * 512];
		size_t bytes = 0;
		for (size_t i = 0; i < count; i++, produced++)
		{
			if (!started || produced < origin || (end_sample && produced >= end_sample)) continue;
			for (int side = 0; side < 2; side++)
			{
				int value = mix[2*i+side] / 4;
				if (value < -32768 || value > 32767) { fputs("Music exceeds encoding headroom\n", stderr); return 2; }
				block[bytes++] = (uint8_t)value;
				block[bytes++] = (uint16_t)value >> 8;
			}
			written++;
		}
		if (fwrite(block, 1, bytes, out) != bytes) return 2;
		if (end_sample && produced >= end_sample) break;
	}
	if (fclose(out) || !written || !end_sample) { fputs("No complete song or loop found\n", stderr); return 2; }
	printf("%llu %lld\n", (unsigned long long)written, looping ? (long long)loop_sample : -1LL);
	free(bank);
	return 0;
}
