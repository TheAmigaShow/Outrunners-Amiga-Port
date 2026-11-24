#ifndef ORUNNERS_OPENING_TRACE_H
#define ORUNNERS_OPENING_TRACE_H
#include <stdbool.h>
#include <stdint.h>

extern volatile bool opening_active;
bool opening_trace_open(void);
uint32_t opening_clock(void);
void opening_frame(unsigned board_frame, unsigned mode, unsigned mode_step, unsigned game_step);
enum opening_work { OPEN_DRAW, OPEN_SPRITES, OPEN_SOUND_END, OPEN_PRESENT, OPEN_WORK_COUNT };
void opening_draw_result(bool fading, bool skipped);
void opening_work_done(enum opening_work part, uint32_t start);
void opening_block(unsigned first, unsigned ready, unsigned used, unsigned frames,
	bool drained, bool music, uint32_t start, uint32_t mixed);
void opening_report(void);
#endif
