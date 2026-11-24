#include "ima.h"

/* Microsoft IMA WAV stereo blocks. The two predictors are independent and
   each channel receives four bytes (eight low-nibble-first deltas) per group. */
typedef struct { uint16_t delta, next; } ImaStep;
static ImaStep changes[89][8];

void amiga_ima_init(void)
{
	static const int steps[89] = {
		7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,
		66,73,80,88,97,107,118,130,143,157,173,190,209,230,253,279,307,337,
		371,408,449,494,544,598,658,724,796,876,963,1060,1166,1282,1411,1552,
		1707,1878,2066,2272,2499,2749,3024,3327,3660,4026,4428,4871,5358,
		5894,6484,7132,7845,8630,9493,10442,11487,12635,13899,15289,16818,
		18500,20350,22385,24623,27086,29794,32767
	};
	static const int8_t adjust[8] = {-1,-1,-1,-1,2,4,6,8};
	for (int index = 0; index < 89; index++)
		for (int mag = 0; mag < 8; mag++)
		{
			int next = index + adjust[mag];
			changes[index][mag].delta = ((2 * mag + 1) * steps[index]) >> 3;
			changes[index][mag].next = next < 0 ? 0 : (next > 88 ? 88 : next);
		}
}

bool amiga_ima_decode(const uint8_t *block, int16_t out[IMA_FRAMES][2])
{
	for (unsigned side = 0; side < 2; side++)
	{
		const uint8_t *head = block + 4 * side;
		int sample = (int16_t)(head[0] | head[1] << 8), index = head[2];
		if (index > 88 || head[3]) return false;
		out[0][side] = sample;
		unsigned frame = 1;
		for (unsigned at = 8 + 4 * side; at < IMA_BYTES; at += 8)
			for (unsigned j = 0; j < 4; j++)
			{
				unsigned data = block[at + j];
				for (int n = 0; n < 2; n++, data >>= 4)
				{
					const unsigned mag = data & 7;
					const ImaStep change = changes[index][mag];
					const int delta = change.delta;
					sample += (data & 8) ? -delta : delta;
					if (sample > 32767) sample = 32767;
					if (sample < -32768) sample = -32768;
					index = change.next;
					out[frame++][side] = sample;
				}
			}
	}
	return true;
}
