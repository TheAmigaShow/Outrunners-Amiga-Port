#ifndef ORUNNERS_CHIME_TRACE_H
#define ORUNNERS_CHIME_TRACE_H

#include <stdbool.h>
#include <stdint.h>

enum chime_phase { CHIME_IDLE, CHIME_APPROACH, CHIME_GRID, CHIME_DRIVING };
extern volatile enum chime_phase chime_phase;
bool chime_trace_open(void);
void chime_trace_tick(void);
void chime_trace_published(unsigned end, unsigned count, uint64_t board_cycles);
void chime_trace_block(enum chime_phase phase, unsigned first, unsigned ready,
	unsigned used, unsigned frames, bool drained);
void chime_trace_report(void);

#endif
