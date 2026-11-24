#include "backend.h"
#include "opening_trace.h"
#include "sound/chips.h"

#include <devices/ahi.h>
#include <exec/memory.h>
#include <proto/exec.h>
#include <string.h>

static unsigned buffer_bytes;
static bool compact_output;
static struct MsgPort *port;
static struct AHIRequest *requests[2];
static void *buffers[2];
static bool pending[2], opened;
static unsigned slot, last_peak8;

unsigned amiga_audio_last_peak8(void)
{
	return last_peak8;
}
static volatile uint32_t drained;
struct amiga_audio_stats *volatile amiga_audio_stats_row;

uint32_t amiga_audio_drained(void)
{
	return drained;
}

bool amiga_audio_open(bool compact)
{
	compact_output = compact;
	buffer_bytes = compact ? 1024 : 4096;
	port = CreateMsgPort();
	if (!port)
		return false;
	for (unsigned i = 0; i < 2; ++i)
	{
		requests[i] = (struct AHIRequest *)CreateIORequest(port, sizeof(*requests[i]));
		buffers[i] = AllocMem(buffer_bytes, MEMF_CHIP);
		if (!requests[i] || !buffers[i])
			return false;
	}

	requests[0]->ahir_Version = 4;
	if (OpenDevice(AHINAME, 0, (struct IORequest *)requests[0], 0))
		return false;
	opened = true;
	*requests[1] = *requests[0];
	return true;
}

bool amiga_audio_queue(const int16_t *samples, size_t frames, unsigned missing_fx, unsigned held_fx)
{
	struct AHIRequest *request = requests[slot];
	struct IORequest *io = (struct IORequest *)request;
	const unsigned previous = slot ^ 1;

	if (!frames)
		return true;
	const unsigned frame_bytes = compact_output ? 2 : 4;
	if (frames > buffer_bytes / frame_bytes)
		return false;
	if (pending[slot])
	{
		pending[slot] = false;
		if (WaitIO(io))
			return false;
	}
	/* A slow game may have already drained the previous block. */
	const bool ran_dry = pending[previous] && CheckIO((struct IORequest *)requests[previous]);
	if (ran_dry)
	{
		drained++;
		pending[previous] = false;
		if (WaitIO((struct IORequest *)requests[previous]))
			return false;
	}

	struct amiga_audio_stats *row = amiga_audio_stats_row;
	if (compact_output)
	{
		int8_t *out = buffers[slot];
		if (row || opening_active)
		{
			/* Measure the signed bytes written to AHI without reading Chip RAM. */
			uint32_t squares = 0;
			unsigned peak = 0;
			for (size_t i = 0; i < frames * 2; ++i)
			{
				const int sample = (int8_t)((samples[i] + 128) >> 8);
				out[i] = (int8_t)sample;
				const unsigned magnitude = sample < 0 ? -sample : sample;
				if (magnitude > peak) peak = magnitude;
				squares += sample * sample;
			}
			last_peak8 = peak;
			if (row)
			{
				if (peak * 256 > row->peak) row->peak = peak * 256;
				row->sum_squares += (uint64_t)squares << 16;
			}
		}
		else
			for (size_t i = 0; i < frames * 2; ++i)
				out[i] = (int8_t)((samples[i] + 128) >> 8);
	}
	else
	{
		memcpy(buffers[slot], samples, frames * frame_bytes);
		if (row)
		{
			unsigned peak = 0;
			uint64_t squares = 0;
			for (size_t i = 0; i < frames * 2; ++i)
			{
				const int sample = samples[i];
				const unsigned magnitude = sample < 0 ? -sample : sample;
				if (magnitude > peak) peak = magnitude;
				squares += (uint32_t)(sample * sample);
			}
			if (peak > row->peak) row->peak = peak;
			row->sum_squares += squares;
		}
	}
	request->ahir_Std.io_Command = CMD_WRITE;
	request->ahir_Std.io_Message.mn_Node.ln_Pri = 0;
	request->ahir_Std.io_Data = buffers[slot];
	request->ahir_Std.io_Length = frames * frame_bytes;
	request->ahir_Std.io_Offset = 0;
	request->ahir_Frequency = compact_output ? 22050 : sound_chips_mix_rate();
	request->ahir_Type = compact_output ? AHIST_S8S : AHIST_S16S;
	request->ahir_Volume = 0x10000;
	request->ahir_Position = 0x8000;
	request->ahir_Link = pending[previous] ? requests[previous] : NULL;
	SendIO(io);
	pending[slot] = true;
	slot = previous;
	if (row)
	{
		row->writes++;
		row->output_frames += frames;
		row->drains += ran_dry;
		row->short_blocks += missing_fx + held_fx != 0;
		row->missing_fx += missing_fx;
		row->held_fx += held_fx;
	}
	return true;
}

void amiga_audio_close(void)
{
	for (unsigned i = 0; i < 2; ++i)
		if (pending[i] && !CheckIO((struct IORequest *)requests[i]))
			AbortIO((struct IORequest *)requests[i]);

	for (unsigned i = 0; i < 2; ++i)
		if (pending[i])
			WaitIO((struct IORequest *)requests[i]);

	if (opened)
		CloseDevice((struct IORequest *)requests[0]);
	for (unsigned i = 0; i < 2; ++i)
	{
		if (requests[i])
			DeleteIORequest(requests[i]);
		if (buffers[i])
			FreeMem(buffers[i], buffer_bytes);
	}
	if (port)
		DeleteMsgPort(port);
}
