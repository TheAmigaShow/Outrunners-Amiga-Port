#ifndef ORUNNERS_AMIGA_IMA_H
#define ORUNNERS_AMIGA_IMA_H
#include <stdbool.h>
#include <stdint.h>
enum { IMA_BYTES = 1024, IMA_FRAMES = 1017, MUSIC_TRACKS = 39 };
void amiga_ima_init(void);
bool amiga_ima_decode(const uint8_t *block, int16_t out[IMA_FRAMES][2]);
#endif
