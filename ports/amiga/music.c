#include "backend.h"
#include "chime_trace.h"
#include "opening_trace.h"
#include "ima.h"
#include "sound/chips.h"

#include <dos/dosextens.h>
#include <dos/dostags.h>
#include <exec/memory.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { PAGE = 8192, AUDIO_FRAMES = 512, RING = 2048, FADE = 32 };
typedef struct { ULONG offset, frames, loop; } Track;
static Track tracks[MUSIC_TRACKS];
static char filename[1024];
static struct Task *parent;
static BYTE notify_bit;
static volatile enum { STARTING, PLAYING, FINISHED, FAILED } state;
static volatile bool stopping;
static volatile unsigned selection, generation, volume;
static int32_t effects[RING];
static volatile unsigned produced, consumed;

/* Only the higher-priority audio process uses the following state. One DOS
   packet reads ahead while AHI plays and the current page is decoded. */
static struct MsgPort *replies;
static struct StandardPacket packet;
static BPTR file;
static uint8_t pages[2][PAGE];
static unsigned current, position, available, pending;
static ULONG unread;
static int16_t decoded[IMA_FRAMES][2], output[AUDIO_FRAMES * 2];
static unsigned sample_in_block;
static ULONG sample_number;
static const Track *playing;
static int32_t gain;
enum { GAIN_UNITY = 256, GAIN_MAX = 362 }; /* Q8, up to +3 dB. */
static bool auto_gain_enabled;
static int makeup_gain = GAIN_UNITY;
static unsigned restart_frames = FX_RECOVERY_NORMAL;
static volatile bool recovering, producer_waiting;
static int32_t last_effect, fade_from;
static unsigned fade_left;

/* Normal contiguous blocks are mixed straight from the ring. Only wraparound
   and starvation need a temporary block. Ramp the effects alone across a gap. */
static const int32_t *effect_block(unsigned first, unsigned take, int32_t *scratch)
{
	unsigned at = first & (RING - 1);
	const int32_t *samples = effects + at;
	if (take == AUDIO_FRAMES && at + take <= RING && !recovering)
	{
		last_effect = samples[take - 1];
		return samples;
	}
	unsigned first_part = RING - at;
	if (first_part > take) first_part = take;
	memcpy(scratch, samples, first_part * sizeof(*scratch));
	memcpy(scratch + first_part, effects, (take - first_part) * sizeof(*scratch));
	if (recovering && take)
	{
		for (unsigned i = 0; i < FADE && i < take; ++i)
			scratch[i] = last_effect + (scratch[i] - last_effect) * (int32_t)(i + 1) / FADE;
	}
	if (take)
	{
		last_effect = scratch[take - 1];
		fade_from = last_effect;
		fade_left = FADE;
	}
	else if (!recovering)
	{
		fade_from = last_effect;
		fade_left = FADE;
	}
	for (unsigned i = take; i < AUDIO_FRAMES; ++i)
		scratch[i] = fade_left ? fade_from * (int32_t)--fade_left / FADE : 0;
	last_effect = scratch[AUDIO_FRAMES - 1];
	return scratch;
}

static ULONG big32(const uint8_t *p)
{
	return (ULONG)p[0] << 24 | (ULONG)p[1] << 16 | (ULONG)p[2] << 8 | p[3];
}

static bool load_index(void)
{
	uint8_t header[8 + 12 * MUSIC_TRACKS];
	if (Read(file, header, sizeof(header)) != sizeof(header) || memcmp(header, "ORMUSIC\1", 8))
		return false;
	if (Seek(file, 0, OFFSET_END) < 0) return false;
	const LONG size = Seek(file, 0, OFFSET_BEGINNING);
	if (size < (LONG)sizeof(header)) return false;
	for (int i = 0; i < MUSIC_TRACKS; i++)
	{
		const uint8_t *p = header + 8 + i * 12;
		Track *t = tracks + i;
		t->offset = big32(p); t->frames = big32(p+4); t->loop = big32(p+8);
		if (t->offset < sizeof(header) || t->offset > (ULONG)size || !t->frames ||
			(t->frames - 1) / IMA_FRAMES >= ((ULONG)size - t->offset) / IMA_BYTES ||
			(t->loop != 0xffffffffUL && t->loop >= t->frames)) return false;
	}
	return true;
}

/* Finish exactly one read before seeking or releasing its buffer. */
static bool finish_read(void)
{
	if (!pending) return true;
	WaitPort(replies);
	GetMsg(replies);
	const unsigned expected = pending;
	pending = 0;
	return packet.sp_Pkt.dp_Res1 == (LONG)expected;
}

static void read_ahead(void)
{
	pending = unread > PAGE ? PAGE : unread;
	if (!pending) return;
	struct FileHandle *fh = BADDR(file);
	packet.sp_Msg.mn_Node.ln_Name = (char *)&packet.sp_Pkt;
	packet.sp_Pkt.dp_Link = &packet.sp_Msg;
	packet.sp_Pkt.dp_Type = ACTION_READ;
	packet.sp_Pkt.dp_Arg1 = fh->fh_Arg1;
	packet.sp_Pkt.dp_Arg2 = (LONG)pages[current ^ 1];
	packet.sp_Pkt.dp_Arg3 = pending;
	unread -= pending;
	packet.sp_Pkt.dp_Port = replies;
	packet.sp_Msg.mn_ReplyPort = replies;
	packet.sp_Msg.mn_Length = sizeof(packet);
	PutMsg(fh->fh_Type, &packet.sp_Msg);
}

static bool decode_next(void)
{
	if (position == available)
	{
		unsigned bytes = pending;
		if (!bytes || !finish_read()) return false;
		current ^= 1;
		position = 0;
		available = bytes;
		read_ahead();
	}
	if (!amiga_ima_decode(pages[current] + position, decoded)) return false;
	position += IMA_BYTES;
	sample_in_block = 0;
	return true;
}

static bool seek_sample(ULONG frame)
{
	const ULONG block = frame / IMA_FRAMES;
	if (!finish_read() || Seek(file, playing->offset + block * IMA_BYTES, OFFSET_BEGINNING) < 0)
		return false;
	unread = (((playing->frames - 1) / IMA_FRAMES + 1) - block) * IMA_BYTES;
	position = available = 0;
	read_ahead();
	if (!decode_next()) return false;
	sample_number = frame;
	sample_in_block = frame % IMA_FRAMES;
	return true;
}

/* Use the previous output block as feedback. Freeze upward gain during
   fades, near-silence and effects starvation so gaps do not drive the level. */
static void update_makeup_gain(const int16_t *samples, bool allow_rise)
{
	unsigned sum = 0;
	for (unsigned i = 0; i < 2 * AUDIO_FRAMES; ++i)
		sum += abs((int)samples[i]);
	const unsigned mean = sum / (2 * AUDIO_FRAMES);
	if (mean > 5600 && makeup_gain > GAIN_UNITY)
	{
		makeup_gain -= 4;
		if (makeup_gain < GAIN_UNITY) makeup_gain = GAIN_UNITY;
	}
	else if (allow_rise && mean >= 256 && mean < 4800 && makeup_gain < GAIN_MAX)
		++makeup_gain;
}

static bool fill_audio(void)
{
	static unsigned applied;
	const bool trace = opening_active;
	const unsigned start = trace ? opening_clock() : 0;
	if (applied != generation)
	{
		Forbid();
		unsigned id = selection;
		applied = generation;
		Permit();
		playing = id ? &tracks[id-1] : NULL;
		if (!finish_read() || (playing && !seek_sample(0))) return false;
	}
	const bool music_selected = playing != NULL;
	const int level = volume;
	const int block_gain = makeup_gain;
	const unsigned first = consumed;
	const unsigned ready = produced - first;
	const enum chime_phase phase = chime_phase;
	unsigned take = ready;
	/* Rebuild a small lead after an underrun. Let a blocked producer drain
	   even when it has requested more room than the usual game frame. */
	const unsigned available_fx = take < AUDIO_FRAMES ? take : AUDIO_FRAMES;
	if (recovering && take < restart_frames && !producer_waiting)
		take = 0;
	else
		take = available_fx;
	int32_t scratch[AUDIO_FRAMES];
	const int32_t *block = effect_block(first, take, scratch);
	recovering = take < AUDIO_FRAMES;
	for (unsigned i = 0; i < AUDIO_FRAMES; i++)
	{
		int32_t left = block[i];
		int32_t right = left;
		if (playing && sample_number == playing->frames)
		{
			if (playing->loop == 0xffffffffUL) playing = NULL;
			else if (!seek_sample(playing->loop)) return false;
		}
		if (playing)
		{
			if (sample_in_block == IMA_FRAMES && !decode_next()) return false;
			/* The encoder reserves two bits of headroom for summing voices. */
			if (level == 128)
			{
				left += decoded[sample_in_block][0] * 4;
				right += decoded[sample_in_block][1] * 4;
			}
			else
			{
				left += (decoded[sample_in_block][0] * level) >> 5;
				right += (decoded[sample_in_block][1] * level) >> 5;
			}
			sample_in_block++;
			sample_number++;
		}
		if (block_gain != GAIN_UNITY)
		{
			left = (left * block_gain) >> 8;
			right = (right * block_gain) >> 8;
		}
		sound_compress_pair(&gain, output + 2*i, left, right);
	}
	if (auto_gain_enabled)
		update_makeup_gain(output, !recovering && (!playing || level >= 128));
	consumed = first + take;
	if (producer_waiting && take)
		Signal(parent, 1UL << notify_bit);
	const unsigned drains = (phase || trace) ? amiga_audio_drained() : 0;
	const unsigned mixed = trace ? opening_clock() : 0;
	const bool ok = amiga_audio_queue(output, AUDIO_FRAMES, AUDIO_FRAMES - available_fx, available_fx - take);
	if (ok && trace)
		opening_block(first, ready, take, AUDIO_FRAMES, amiga_audio_drained() != drains, music_selected, start, mixed);
	if (ok && phase)
		chime_trace_block(phase, first, ready, take, AUDIO_FRAMES, amiga_audio_drained() != drains);
	return ok;
}

static LONG music_process(void)
{
	amiga_ima_init();
	replies = CreateMsgPort();
	file = Open(filename, MODE_OLDFILE);
	char stack_probe;
	bool ok = (TypeOfMem(&stack_probe) & MEMF_FAST) && (TypeOfMem(&packet) & MEMF_FAST) && replies && file && load_index() && amiga_audio_open(true);
	if (ok)
	{
		gain = 16384;
		state = PLAYING;
		Signal(parent, 1UL << notify_bit);
		while (!stopping && ok) ok = fill_audio();
	}
	finish_read();
	amiga_audio_close();
	if (file) Close(file);
	if (replies) DeleteMsgPort(replies);
	/* Return into DOS before the parent can unload the executable. */
	Forbid();
	state = ok ? FINISHED : FAILED;
	Signal(parent, 1UL << notify_bit);
	return 0;
}

static bool select_music(unsigned screen, uint8_t command)
{
	static const uint8_t aliases[] = {3,5,6,7,8,9,10,11,12,13,11};
	unsigned id = 0;
	if (command <= 18) id = command ? command : 1;
	else if (command <= 0x1d) id = aliases[command - 0x13];
	else if (command >= 0xe0 && command <= 0xfb)
		id = 19 + (command >= 0xf5 ? command - 0x10 : command) - 0xe0;
	/* The single-player port follows the left cabinet's radio. Its duplicate
	   music on the unused cabinet must also leave the live effects mix. */
	if (screen) return id != 0;
	Forbid();
	selection = id;
	volume = 128;
	generation++;
	Permit();
	return id != 0;
}

static void fade_music(unsigned screen, uint8_t value)
{
	if (!screen) volume = value;
}

/* Returns 0 for an installation without streamed recordings. */
int amiga_music_start(const char *assets, unsigned recovery_frames, bool auto_gain)
{
	char path[1024];
	if (snprintf(path, sizeof(path), "%s", assets) >= (int)sizeof(path) ||
		!AddPart(path, "sound/music.ima", sizeof(path))) return -1;
	BPTR lock = Lock(path, ACCESS_READ);
	if (!lock) return IoErr() == ERROR_OBJECT_NOT_FOUND || IoErr() == ERROR_DIR_NOT_FOUND ? 0 : -1;
	const bool named = NameFromLock(lock, filename, sizeof(filename));
	UnLock(lock);
	if (!named) return -1;
	parent = FindTask(NULL);
	notify_bit = AllocSignal(-1);
	if (notify_bit < 0) return -1;
	restart_frames = recovery_frames;
	auto_gain_enabled = auto_gain;
	makeup_gain = GAIN_UNITY;
	state = STARTING;
	LONG priority = parent->tc_Node.ln_Pri;
	priority = priority < 5 ? 5 : (priority < 127 ? priority + 1 : 127);
	if (!CreateNewProcTags(NP_Entry, (ULONG)music_process, NP_Name, (ULONG)"OutRunners audio",
		NP_StackSize, 16384, NP_Priority, priority, TAG_DONE))
	{
		FreeSignal(notify_bit);
		return -1;
	}
	while (state == STARTING) Wait(1UL << notify_bit);
	if (state != PLAYING) { FreeSignal(notify_bit); return -1; }
	sound_music_command = select_music;
	sound_music_fade = fade_music;
	return 1;
}

/* Called by the sole producer, before drawing. Only the AHI worker can
   advance consumed during this snapshot on the single-CPU Amiga. */
bool amiga_music_effects_low(void)
{
	return produced - consumed < AUDIO_FRAMES;
}

bool amiga_music_effects(const int32_t *mix, size_t count)
{
	if (count > RING) return false;
	Forbid();
	while (state == PLAYING && produced - consumed + count > RING)
	{
		producer_waiting = true;
		Permit();
		Wait(1UL << notify_bit);
		Forbid();
	}
	producer_waiting = false;
	Permit();
	if (state != PLAYING) return false;
	/* Publish only after the complete block is present. One producer and
	   one consumer on the same CPU; only the producer advances produced. */
	unsigned start = produced;
	if (!count) return true;
	unsigned at = start & (RING - 1);
	size_t first = RING - at;
	if (first > count) first = count;
	memcpy(effects + at, mix, first * sizeof(*mix));
	memcpy(effects, mix + first, (count - first) * sizeof(*mix));
	Forbid();
	produced = start + count;
	if (chime_phase) chime_trace_published(produced, count, s32_board.cycles);
	struct amiga_audio_stats *row = amiga_audio_stats_row;
	if (row) amiga_audio_delivery(row);
	Permit();
	return true;
}

void amiga_music_close(void)
{
	sound_music_command = NULL;
	sound_music_fade = NULL;
	stopping = true;
	while (state == PLAYING) Wait(1UL << notify_bit);
	FreeSignal(notify_bit);
}
