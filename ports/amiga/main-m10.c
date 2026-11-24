#include "backend.h"
#include "chime_trace.h"
#include "opening_trace.h"
#include "scenery.h"

#include "game/assets.h"
#include "ram.h"
#include "s32/machine.h"
#include "s32/program.h"
#include "s32/script.h"
#include "s32/video.h"
#include "sound/chips.h"
#include "sound/support.h"

#include <devices/timer.h>
#include <exec/memory.h>
#include <proto/exec.h>
#include <proto/lowlevel.h>
#include <libraries/lowlevel.h>
#include <proto/dos.h>
#include <proto/timer.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct Library *LowLevelBase;

#include "mapping.h"
static bool keys[128];
static bool audio_failed;
static int streaming;
static s32_script *benchmark_script;
enum { BENCH_COUNT = 10 };
static unsigned bench_first = 2401;
static uint64_t bench_ticks[BENCH_COUNT + 1];
static unsigned bench_presented;
static uint32_t bench_drained;

enum audio_scene { OTHER, CAR_SELECT, START_GRID, DRIVING, AUDIO_SCENES };
static struct amiga_audio_stats audio_rows[AUDIO_SCENES];
static bool audio_stats, menu30, auto_gain, trace_chime, trace_opening;
static uint32_t clock_hz, clock_remainder, block_ticks;
static unsigned recovery_frames = FX_RECOVERY_NORMAL;
static bool frame_skipped, menu_frame, fade_frame, audio_behind, fixed_menu;
static unsigned consecutive_skips;
static bool adaptive_pacing, late_draw;
static unsigned catchup_skips, timing_skips;
static unsigned play_ticks, play_presented;
static uint64_t play_start, play_end;
static bool fx_guard, last_guard_skip;
static unsigned fade_ticks, fade_skipped, consecutive_fade_skips;
static struct { unsigned ticks, skipped; } guard_stats[AUDIO_SCENES];
static uint64_t menu_tick_start;
struct menu_timing {
	uint32_t count;
	uint64_t work, elapsed;
};
static struct menu_timing menu_times[2];

static bool sync_menu(void)
{
	return menu30 && streaming > 0 && !fixed_menu && !benchmark_script && menu_frame;
}
static uint32_t menu_draws, menu_skips;

static enum audio_scene current_audio_scene(void)
{
	const struct screen *left = &game->screen_1;
	if (left->game_mode != 6 || !left->in_game) return OTHER;
	if (left->game_step == 3) return CAR_SELECT;
	if (left->game_step == 7) return left->race_started ? DRIVING : START_GRID;
	return OTHER;
}

static bool skip_race_draw(enum audio_scene scene, bool can_skip)
{
	if (scene != START_GRID && scene != DRIVING)
	{
		last_guard_skip = false;
		return false;
	}

	++guard_stats[scene].ticks;
	last_guard_skip = can_skip && !last_guard_skip && amiga_music_effects_low();
	if (last_guard_skip) ++guard_stats[scene].skipped;
	return last_guard_skip;
}

/* At VBlank, before controls and simulation advance. Sprite updates keep their
   normal cadence because the emulated board swaps and erases its own buffers. */
static void draw_game_frame(void)
{
	const bool was_menu = menu_frame;
	const bool was_fade = fade_frame;
	const enum audio_scene scene = current_audio_scene();
	if (trace_chime)
		chime_trace_tick();
	menu_frame = scene == CAR_SELECT;
	frame_skipped = false;
	/* The game's mixer clears flag 0 when the last fade step is reached.
	   Fully serviced car selection already has its own catch-up policy. */
	fade_frame = fx_guard && (game->screen_1.flags & 1u) && !(menu_frame && menu30);
	const bool adapt_draw = adaptive_pacing && !benchmark_script && !fade_frame && !(menu_frame && menu30);
	if (!adapt_draw) catchup_skips = 0;
	if (fade_frame)
	{
		++fade_ticks;
		last_guard_skip = false;
		if (was_fade && consecutive_fade_skips < 5 && (audio_behind || amiga_music_effects_low()))
		{
			++fade_skipped;
			++consecutive_fade_skips;
			if (menu_frame) ++menu_skips;
			frame_skipped = true;
			return;
		}
	}
	else
	{
		/* Four catch-up draws may be omitted. Under sustained overload the
		   next tick still draws a fresh picture and the clock debt is bounded. */
		const bool can_skip = !adapt_draw || catchup_skips < 4;
		const bool audio_skip = fx_guard && skip_race_draw(scene, can_skip);
		if (audio_skip || (adapt_draw && late_draw && can_skip))
		{
			if (adapt_draw) ++catchup_skips;
			if (!audio_skip) ++timing_skips;
			if (menu_frame) ++menu_skips;
			frame_skipped = true;
			return;
		}
		catchup_skips = 0;
	}
	consecutive_fade_skips = 0;
	if (menu_frame && menu30)
	{
		/* At most 30 draws/s. A late streamed menu can use further ticks to
		   feed effects. Keep selection feedback visible even under overload. */
		frame_skipped = was_menu && (consecutive_skips == 0 ||
			(sync_menu() && audio_behind && consecutive_skips < 5));
	}
	if (frame_skipped)
	{
		++menu_skips;
		++consecutive_skips;
		return;
	}
	consecutive_skips = 0;
	if (menu_frame) ++menu_draws;
	s32_video_draw_frame();
}

static void opening_draw(void)
{
	if (!opening_active) { draw_game_frame(); return; }
	const struct screen *left = &game->screen_1;
	opening_frame(s32_board.frame, left->game_mode, left->mode_step, left->game_step);
	const unsigned start = opening_clock();
	draw_game_frame();
	opening_work_done(OPEN_DRAW, start);
	opening_draw_result((left->flags & 1u) != 0, frame_skipped);
}

static void opening_sprites(bool erase, bool draw)
{
	if (!opening_active) { s32_video_sprites(erase, draw); return; }
	const unsigned start = opening_clock();
	s32_video_sprites(erase, draw);
	opening_work_done(OPEN_SPRITES, start);
}

static void print_audio_stats(void)
{
	static const char *const names[] = {"other", "car_select", "start_grid", "driving"};
	printf("Audio source: %s\n", streaming ? "streamed music and live effects" : "live mix");
	printf("Car select: %s, %lu frames drawn, %lu skipped\n",
		menu30 ? "up to half-rate" : "normal", (unsigned long)menu_draws, (unsigned long)menu_skips);
	if (play_end > play_start && play_ticks)
	{
		const double scale = (double)clock_hz / (play_end - play_start);
		printf("Play pacing %s | %.2f game ticks/s | %.2f presented frames/s | %lu deadline skips\n",
			adaptive_pacing ? "adaptive" : "M8", play_ticks * scale, play_presented * scale, (unsigned long)timing_skips);
	}
	if (menu30 && streaming && !benchmark_script)
		printf("Menu pacing: %s\n", fixed_menu ? "fixed half-rate" : "audio catch-up");
	for (unsigned i = 0; i < 2; ++i)
	{
		const struct menu_timing *t = &menu_times[i];
		if (t->count)
			printf("Menu %s: %lu ticks, mean work %.2f ms, including pacing %.2f ms\n",
				i ? "skipped" : "drawn", (unsigned long)t->count,
				1000.0 * t->work / clock_hz / t->count,
				1000.0 * t->elapsed / clock_hz / t->count);
	}
	if (fx_guard)
	{
		printf("FXGUARD fades: %u of %u eligible draws skipped\n", fade_skipped, fade_ticks);
		for (unsigned i = START_GRID; i <= DRIVING; ++i)
			printf("FXGUARD %s: %u of %u eligible draws skipped\n", names[i],
				guard_stats[i].skipped, guard_stats[i].ticks);
	}
	if (streaming) printf("Effects recovery threshold: %u frames\n", recovery_frames);
	printf("Automatic gain: %s\n", auto_gain && streaming ? "on" : "off");
	puts("scene,ahi_writes,output_frames,ahi_drains,fx_short_blocks,fx_missing_frames,fx_held_frames,peak_dBFS,rms_dBFS,fx_intervals,fx_mean_ms,fx_max_ms,fx_over_block");
	for (unsigned i = 0; i < AUDIO_SCENES; ++i)
	{
		const struct amiga_audio_stats *row = &audio_rows[i];
		printf("%s,%lu,%lu,%lu,", names[i], (unsigned long)row->writes,
			(unsigned long)row->output_frames, (unsigned long)row->drains);
		if (streaming)
			printf("%lu,%lu,%lu,", (unsigned long)row->short_blocks,
				(unsigned long)row->missing_fx, (unsigned long)row->held_fx);
		else fputs("n/a,n/a,n/a,", stdout);
		if (!row->output_frames)
			fputs("n/a,n/a", stdout);
		else if (!row->peak)
			fputs("-inf,-inf", stdout);
		else
		{
			const double full_scale = 32768.0;
			const double power = (double)row->sum_squares / (2.0 * row->output_frames);
			printf("%.1f,%.1f", 20.0 * log10(row->peak / full_scale),
				10.0 * log10(power / (full_scale * full_scale)));
		}
		if (row->delivery_intervals)
			printf(",%lu,%.2f,%.2f,%lu\n", (unsigned long)row->delivery_intervals,
				1000.0 * row->delivery_ticks / clock_hz / row->delivery_intervals,
				1000.0 * row->longest_delivery_ticks / clock_hz, (unsigned long)row->late_deliveries);
		else puts(streaming ? ",0,n/a,n/a,0" : ",n/a,n/a,n/a,n/a");
	}
}

static void benchmark_controls(void)
{
	s32_script_apply(benchmark_script, s32_board.frame);
}
static struct MsgPort *timer_port;
static struct timerequest *timer_io;
static bool timer_open;
struct Device *TimerBase;
static uint64_t deadline;

static uint64_t ticks(void)
{
	struct EClockVal clock;
	clock_hz = ReadEClock(&clock);
	return (uint64_t)clock.ev_hi << 32 | clock.ev_lo;
}

/* Called by the producer when a nonempty block is published. Exclude the
   first interval of each scene, which would straddle the transition. */
void amiga_audio_delivery(struct amiga_audio_stats *row)
{
	static struct amiga_audio_stats *previous;
	static uint64_t last;
	const uint64_t now = ticks();
	if (row == previous)
	{
		const uint64_t interval = now - last;
		row->delivery_intervals++;
		row->delivery_ticks += interval;
		if (interval > row->longest_delivery_ticks) row->longest_delivery_ticks = interval;
		if (interval > block_ticks) row->late_deliveries++;
	}
	previous = row;
	last = now;
}

static bool open_timer(void)
{
	timer_port = CreateMsgPort();
	if (!timer_port)
		return false;
	timer_io = (struct timerequest *)CreateIORequest(timer_port, sizeof(*timer_io));
	if (!timer_io || OpenDevice(TIMERNAME, UNIT_MICROHZ, (struct IORequest *)timer_io, 0))
		return false;
	timer_open = true;
	TimerBase = timer_io->tr_node.io_Device;
	deadline = ticks();
	block_ticks = (uint64_t)clock_hz * 512 / 22050;
	return clock_hz != 0;
}

static void close_timer(void)
{
	if (timer_open)
		CloseDevice((struct IORequest *)timer_io);
	if (timer_io)
		DeleteIORequest(timer_io);
	if (timer_port)
		DeleteMsgPort(timer_port);
}

static void pace(uint64_t now)
{
	deadline += clock_hz / S32_FRAME_RATE;
	clock_remainder += clock_hz % S32_FRAME_RATE;
	if (clock_remainder >= S32_FRAME_RATE)
	{
		++deadline;
		clock_remainder -= S32_FRAME_RATE;
	}
	const uint32_t period = clock_hz / S32_FRAME_RATE;
	const uint64_t late = now > deadline ? now - deadline : 0;
	/* Retain a bounded timing debt. Each inexpensive catch-up tick advances
	   the real game and sound program once, producing its own PCM samples. */
	const bool catch_up = adaptive_pacing || sync_menu() || fade_frame;
	/* Ignore sub-field jitter. Revisit the decision after every real tick. */
	late_draw = adaptive_pacing && late >= period / 2;
	audio_behind = catch_up && late > period / 4;
	if (now < deadline)
	{
		timer_io->tr_node.io_Command = TR_ADDREQUEST;
		timer_io->tr_time.tv_secs = 0;
		timer_io->tr_time.tv_micro = (deadline - now) * 1000000 / clock_hz;
		DoIO((struct IORequest *)timer_io);
	}
	else if (catch_up && late > 4 * period)
		deadline = now - 4 * period;
	else if (!catch_up && late > period)
		deadline = now;
}

/* Retain transitions in RAM so input tracing never writes during play. */
struct pad_sample {
	unsigned tick;
	ULONG raw;
	unsigned char wheel, gas, brake, a, b, e;
	unsigned short buttons;
};
static struct pad_sample pad_samples[512];
static unsigned pad_count, pad_ticks;
static ULONG last_raw;
static unsigned short last_buttons;

static void record_pad(ULONG raw, const s32_inputs *input)
{
	unsigned tick = ++pad_ticks;
	unsigned short buttons = game->screen_1.buttons;
	if (pad_count < 512 && (!pad_count || raw != last_raw || buttons != last_buttons)) {
		pad_samples[pad_count++] = (struct pad_sample){tick,raw,input->analog[0],input->analog[1],
			input->analog[2],input->port_a[0],input->port_b[0],input->port_e[0],buttons};
	}
	last_raw = raw;
	last_buttons = buttons;
}

static void save_pad_report(void)
{
	BPTR f = Open("PROGDIR:Pad-Game.txt", MODE_NEWFILE);
	if (!f) return;
	char row[160];
	snprintf(row,sizeof row,"Pad 3, port 2, automatic detection, library %s, %u input polls\n",LowLevelBase?"open":"missing",pad_ticks);
	FPuts(f,row);
	FPuts(f,"tick,raw,wheel,gas,brake,portA,portB,portE,gameButtonsAtPoll\n");
	for (unsigned i=0;i<pad_count;i++) {
		const struct pad_sample *p=&pad_samples[i];
		snprintf(row,sizeof row,"%u,%08lx,%u,%u,%u,%02x,%02x,%02x,%04x\n",p->tick,
			(unsigned long)p->raw,p->wheel,p->gas,p->brake,p->a,p->b,p->e,p->buttons);
		FPuts(f,row);
	}
	Close(f);
}

static int approach(int value, int target, int step)
{
	if (value < target)
		return value + step < target ? value + step : target;
	return value - step > target ? value - step : target;
}

static void controls(void)
{
	s32_inputs *input = &s32_board.input;
	ULONG pad = LowLevelBase ? ReadJoyPort(1) : 0;
	const ULONG raw = pad;
	if ((pad & JP_TYPE_MASK) == JP_TYPE_JOYSTK)
		pad &= JP_DIRECTION_MASK | JPF_BUTTON_RED | JPF_BUTTON_BLUE;
	else if ((pad & JP_TYPE_MASK) != JP_TYPE_GAMECTLR)
		pad = 0;
	const int right = keys[0x4e] || (pad & JPF_JOY_RIGHT);
	const int left = keys[0x4f] || (pad & JPF_JOY_LEFT);
	const int steer = right - left;
	input->analog[0] = approach(input->analog[0], 0x80 + steer * 0x70, steer ? 6 : 8);
	input->analog[1] = approach(input->analog[1], (keys[0x4c] || (pad & binding[GAS])) ? 255 : 0, 32);
	input->analog[2] = approach(input->analog[2], (keys[0x4d] || (pad & binding[BRAKE])) ? 255 : 0, 32);
	input->port_a[0] = 0xff & ~((keys[0x21] || (pad & binding[UP_SHIFT])) ? 1 : 0) & ~((keys[0x22] || (pad & binding[DOWN_SHIFT])) ? 2 : 0);
	input->port_b[0] = 0xff & ~((keys[0x37] || (pad & binding[RADIO])) ? 1 : 0) & ~((keys[0x38] || (pad & JPF_JOY_UP)) ? 2 : 0) & ~((keys[0x39] || (pad & JPF_JOY_DOWN)) ? 4 : 0);
	input->port_e[0] = 0xff & ~((keys[0x05] || (pad & binding[COIN])) ? 4 : 0) & ~((keys[0x44] || (pad & binding[START_PAD])) ? 16 : 0);
	record_pad(raw,input);
}

/* Only the scripted benchmark installs these callbacks. */
struct frame_cost { unsigned draw, sprites, audio, present; };
static struct frame_cost cost, measured[BENCH_COUNT + 1];

static void bench_draw(void)
{
	const unsigned start = opening_clock();
	if (menu30 || audio_stats) draw_game_frame();
	else s32_video_draw_frame();
	cost.draw += opening_clock() - start;
}

static void bench_sprites(bool erase, bool draw)
{
	const unsigned start = opening_clock();
	s32_video_sprites(erase, draw);
	cost.sprites += opening_clock() - start;
}

static void frame_done(void)
{
	int width;
	size_t count;
	if (!amiga_display_events(keys))
	{
		program_stop();
		return;
	}
	const bool profile = opening_active;
	const unsigned sound_start = profile || benchmark_script ? opening_clock() : 0;
	if (s32_board.time_passes)
		s32_board.time_passes(s32_board.cycles);
	sound_chips_end_frame(s32_board.cycles);
	if (profile) opening_work_done(OPEN_SOUND_END, sound_start);
	const unsigned number = benchmark_script ? (unsigned)(program_frames() - 1) : 0;
	if (benchmark_script && number < bench_first - 7)
	{
		if (number == bench_first - 8)
		{
			s32_board.frame_shown = bench_draw;
			s32_board.sprites_due = bench_sprites;
		}
		return;
	}
	if (audio_stats)
		amiga_audio_stats_row = &audio_rows[current_audio_scene()];
	bool queued;
	if (streaming)
	{
		const int32_t *mix = sound_chips_wide_mix(&count);
		queued = amiga_music_effects(mix, count);
	}
	else
	{
		const int16_t *mix = sound_chips_frame_mix(&count);
		queued = amiga_audio_queue(mix, count, 0, 0);
	}
	if (benchmark_script) cost.audio += opening_clock() - sound_start;
	if (!queued)
	{
		audio_failed = true;
		program_stop();
		return;
	}
	bool presented = false;
	if (!frame_skipped)
	{
		const unsigned start = profile || benchmark_script ? opening_clock() : 0;
		const uint32_t *frame = s32_video_frame(0, &width);
		presented = width == 320 && amiga_display_frame(frame);
		if (benchmark_script) cost.present += opening_clock() - start;
		if (profile) opening_work_done(OPEN_PRESENT, start);
	}
	if (!benchmark_script)
	{
		const uint64_t work_end = ticks();
		pace(work_end);
		if (audio_stats)
		{
			++play_ticks;
			play_presented += presented;
			const uint64_t end = ticks();
			if (menu_frame && menu_tick_start)
			{
				struct menu_timing *t = &menu_times[frame_skipped];
				t->count++;
				t->work += work_end - menu_tick_start;
				t->elapsed += end - menu_tick_start;
			}
			menu_tick_start = end;
		}
	}
	else if (number >= bench_first - 1)
	{
		const unsigned index = number - (bench_first - 1);
		bench_ticks[index] = ticks();
		measured[index] = cost;
		cost = (struct frame_cost){0};
		if (!index) bench_drained = amiga_audio_drained();
		if (index && presented)
			bench_presented++;
		if (index == BENCH_COUNT)
		{
			bench_drained = amiga_audio_drained() - bench_drained;
			program_stop();
		}
	}
}

static bool loading_window, workbench_launch, log_loading;

static void startup_message(const char *stage)
{
	amiga_loading_line(stage);
	if (log_loading)
	{
		puts(stage);
		fflush(stdout);
	}
}

static int startup_error(const char *message)
{
	amiga_loading_line(message);
	if (!workbench_launch || !loading_window)
		fprintf(stderr, "%s\n", message);
	if (!workbench_launch && Output() && !IsInteractive(Output()))
	{
		/* The loading window closes when a Shell launch returns. Keep
		   the reason in the same redirected file as its progress. */
		printf("OutRunners stopped: %s\n", message);
		fflush(stdout);
	}
	if (workbench_launch && loading_window)
	{
		amiga_loading_line("Press Return to close this window.");
		amiga_loading_wait_return();
	}
	amiga_loading_close();
	loading_window = false;
	return 1;
}

static int run_game(int argc, char **argv)
{
	enum amiga_display_mode display = AMIGA_AGA;
	const bool display_test = argc == 2 && (!strcmp(argv[1], "AGATEST") || !strcmp(argv[1], "P96TEST"));
	if (display_test)
	{
		if (!strcmp(argv[1], "P96TEST")) display = AMIGA_P96;
		argc = 1;
	}
	const char *path = "PROGDIR:assets";
	const char *error;
	char stack_probe;
	static char data_probe;

	bool pair_scenery = true, half = false, live = false, require_pcm22 = false, use_pcm16 = false;
	int8_t *bank = NULL;
	while (argc > 1)
	{
		const char *option = argv[argc - 1];
		if (!strcmp(option, "HALF")) half = true;
		else if (!strcmp(option, "ADAPTIVE")) adaptive_pacing = true;
		else if (!strcmp(option, "FIXEDPACE")) adaptive_pacing = false;
		else if (!strcmp(option, "FINE")) pair_scenery = false;
		else if (!strcmp(option, "OPENINGTRACE")) trace_opening = audio_stats = true;
		else if (!strcmp(option, "CHIMETRACE")) trace_chime = audio_stats = true;
		else if (!strcmp(option, "AUDIOSTATS")) audio_stats = true;
		else if (!strcmp(option, "AGC")) auto_gain = true;
		else if (!strcmp(option, "FXGUARD")) fx_guard = true;
		else if (!strcmp(option, "FX512")) recovery_frames = FX_RECOVERY_SHORT;
		else if (!strcmp(option, "FIXEDMENU")) fixed_menu = menu30 = true;
		else if (!strcmp(option, "MENU30")) menu30 = true;
		else if (!strcmp(option, "LIVE")) live = true;
		else if (!strcmp(option, "PCM22")) require_pcm22 = true;
		else if (!strcmp(option, "PCM16")) use_pcm16 = true;
		else break;
		--argc;
	}
	if ((require_pcm22 && use_pcm16) || argc > 5 || (argc > 1 && strcmp(argv[1], "AGA") && strcmp(argv[1], "RTG") && strcmp(argv[1], "P96")))
	{
		fprintf(stderr, "Usage: OutRunners [AGA|RTG|P96] [assets-directory [benchmark-script [first-frame]]] [HALF] [FINE] [ADAPTIVE|FIXEDPACE] [LIVE] [PCM22|PCM16] [AUDIOSTATS] [CHIMETRACE] [OPENINGTRACE] [FXGUARD] [MENU30|FIXEDMENU] [FX512] [AGC]\nDisplay check: OutRunners AGATEST or P96TEST\n");
		return 1;
	}
	if (argc > 1 && !strcmp(argv[1], "RTG")) display = AMIGA_RTG;
	if (argc > 1 && !strcmp(argv[1], "P96")) display = AMIGA_P96;
	if (argc > 2)
		path = argv[2];

	if (argc >= 4) fx_guard = false;
	if (argc == 5)
	{
		char *end;
		errno = 0;
		const unsigned long first = strtoul(argv[4], &end, 10);
		if (*argv[4] < '0' || *argv[4] > '9' || *end || errno || first < 8 || first > UINT_MAX - BENCH_COUNT)
		{
			fputs("First benchmark frame must be a decimal frame number of 8 or greater.\n", stderr);
			return 1;
		}
		bench_first = (unsigned)first;
	}

	workbench_launch = argc == 0 || !Cli();
	if (!display_test)
		loading_window = amiga_loading_open();
	log_loading = !workbench_launch && (!loading_window || !Output() || !IsInteractive(Output()));

	if (!(TypeOfMem((APTR)run_game) & MEMF_FAST) || !(TypeOfMem(&data_probe) & MEMF_FAST) ||
		!(TypeOfMem(&stack_probe) & MEMF_FAST))
	{
		return startup_error("Requires Fast RAM for code, data and stack.");
	}

	if (display_test)
	{
		error = amiga_display_open(display);
		if (!error) error = amiga_display_test();
		amiga_display_close();
		if (error) fprintf(stderr, "%s\n", error);
		else puts(display == AMIGA_AGA ? "PAL AGA presentation and pixel readback passed."
			: "Picasso96 presentation and RGB565 readback passed.");
		return error != NULL;
	}

	/* Exec stacks grow down. Check the active task stack before starting
	   the asset parsers, AmigaDOS and the game on a small Shell stack. */
	const struct Task *task = FindTask(NULL);
	const uintptr_t lower = (uintptr_t)task->tc_SPLower;
	const uintptr_t here = (uintptr_t)&stack_probe;
	if (here >= lower && here < (uintptr_t)task->tc_SPUpper && here - lower < 16 * 1024u)
		return startup_error("Too little stack for loading. In this Shell run Stack 65536, then start OutRunners again. For Workbench, set the icon Stack to 65536.");
	if (log_loading)
		printf("OutRunners assets: %s\n", path);
	error = assets_load_with_progress(path, startup_message);
	if (error) return startup_error(error);

	if (argc >= 4)
	{
		char message[256];
		benchmark_script = s32_script_load(argv[3], message, sizeof message);
		if (!benchmark_script)
			return startup_error(message);
	}
	if (!use_pcm16) startup_message("Loading PCM22 effects bank");
	if (!use_pcm16 && !(bank = assets_load_pcm22(path)) && (require_pcm22 || errno != ENOENT))
	{
		s32_script_free(benchmark_script);
		return startup_error("Could not load sound/pcm22.bin (requires 2 MiB free Fast RAM). Run prepare_pcm22.py on the assets first.");
	}
	startup_message(display == AMIGA_AGA ? "Opening AGA screen" : "Opening RTG screen");
	error = amiga_display_open(display);
	if (error) goto cleanup;
	if (trace_opening)
	{
		if (live || !bank || benchmark_script)
			error = "OPENINGTRACE requires interactive play with streamed music and PCM22 effects.";
		else if (!open_timer()) error = "Could not open timer.device";
		else if (!opening_trace_open()) error = "Insufficient Fast RAM for the opening audio trace";
		if (error) goto cleanup;
	}
	startup_message("Starting audio on AHI Unit 0");
	if (bank && !live && (streaming = amiga_music_start(path, recovery_frames, auto_gain)) < 0)
		error = "Could not open the music bank or start the AHI streaming process";
	else if (trace_opening && !streaming)
		error = "OPENINGTRACE requires a streamed music bank";
	else if (fx_guard && !streaming)
		error = "FXGUARD requires streamed music and PCM22 effects";
	else if (trace_chime && !streaming)
		error = "CHIMETRACE requires streamed music and PCM22 effects";
	else if (trace_chime && !chime_trace_open())
		error = "Insufficient Fast RAM for the race-start trace";
	else if (auto_gain && !streaming)
		error = "AGC needs streamed music and the PCM22 effects bank";
	else if (!streaming && !amiga_audio_open(bank != NULL))
		error = "Could not open ahi.device unit 0";
	else if (!timer_open && !open_timer())
		error = "Could not open timer.device";
	else
	{
		s32_board.program_at = data_byte_at;
		s32_board.irq_changed = program_irq_line;
		if (!benchmark_script)
			LowLevelBase = OpenLibrary("lowlevel.library", 40);
		if (!benchmark_script)
			startup_message(LowLevelBase ? "Pad v3: Port 2 automatic detection" : "Pad v3: keyboard only (lowlevel.library missing)");
		if (!benchmark_script) load_mapping();
		s32_board.controls_update = benchmark_script ? benchmark_controls : controls;
		s32_board.frame_timing = true;
		s32_init(NULL);
		s32_video_init_graphics(assets_graphics());
		s32_video_set_scenery_filter(roadside_frame);
		s32_video_pair_scenery_rows(pair_scenery);
		s32_video_show_screens(1);
		amiga_display_target(half);
		if (benchmark_script)
		{
			s32_board.frame_shown = NULL;
			s32_board.sprites_due = NULL;
		}
		else if (trace_opening)
		{
			s32_board.frame_shown = opening_draw;
			s32_board.sprites_due = opening_sprites;
		}
		else if (adaptive_pacing || menu30 || audio_stats || fx_guard)
			s32_board.frame_shown = draw_game_frame;
		s32_board.sound_at = assets_sound_data;
		s32_board.sound_byte_at = assets_sound_byte;
		s32_board.pcm_at = assets_pcm_byte;
		if (bank)
			sound_chips_attach_pcm22(bank);
		else
			sound_chips_attach_pcm_playback();
		if (streaming) sound_chips_mono_effects();
		sound_cpu_attach();
		sound_chips_join_sides(true);
		startup_message("Initialising the game");
		program_reset();
		program_frame_done = frame_done;
		startup_message("Running");
		amiga_loading_close();
		loading_window = false;
		play_start = deadline = ticks();
		program_run();
		if (audio_stats) play_end = ticks();
		amiga_audio_stats_row = NULL;
		if (audio_failed)
			error = streaming ? "Music read, decode or AHI playback failed" : "AHI stream write failed";
	}

	if (bench_ticks[BENCH_COUNT])
	{
		puts("frame,elapsed_us,sprite_us,draw_us,audio_service_us,present_us");
		for (unsigned i = 1; i <= BENCH_COUNT; ++i)
		{
			const struct frame_cost *c = &measured[i];
			printf("%u,%lu,%lu,%lu,%lu,%lu\n", bench_first + i - 1,
				(unsigned long)((bench_ticks[i] - bench_ticks[i-1]) * 1000000 / clock_hz),
				(unsigned long)((uint64_t)c->sprites * 1000000 / clock_hz),
				(unsigned long)((uint64_t)c->draw * 1000000 / clock_hz),
				(unsigned long)((uint64_t)c->audio * 1000000 / clock_hz),
				(unsigned long)((uint64_t)c->present * 1000000 / clock_hz));
		}
		const uint64_t elapsed = bench_ticks[BENCH_COUNT] - bench_ticks[0];
		if (elapsed) printf("%.2f game ticks/s, %.2f presented frames/s\n",
			(double)BENCH_COUNT * clock_hz / elapsed, (double)bench_presented * clock_hz / elapsed);
		printf("%lu drained AHI queue events during timed frames\n", (unsigned long)bench_drained);
		printf("%u of %u frames presented\n", bench_presented, BENCH_COUNT);
	}
cleanup:
	if (!benchmark_script) save_pad_report();
	if (LowLevelBase) { CloseLibrary(LowLevelBase); LowLevelBase = NULL; }
	if (benchmark_script)
		s32_script_free(benchmark_script);
	if (streaming > 0) amiga_music_close();
	if (!streaming) amiga_audio_close();
	close_timer();
	amiga_display_close();
	if (audio_stats && clock_hz) print_audio_stats();
	if (trace_chime) chime_trace_report();
	if (trace_opening) opening_report();
	free(bank);
	if (error) return startup_error(error);
	return 0;
}

int main(int argc, char **argv)
{
	if (!amiga_heap_open())
	{
		fputs("Could not create the game heap in Fast RAM.\n", stderr);
		return 1;
	}
	const int result = run_game(argc, argv);
	amiga_heap_close();
	return result;
}
