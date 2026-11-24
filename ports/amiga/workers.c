#include "s32/workers.h"

void s32_workers_start(int count)
{
	(void)count;
}

void s32_workers_rows(s32_rows_work work, void *context, int first, int last)
{
	if (first <= last)
		work(context, first, last);
}

int s32_workers_count(void)
{
	return 0;
}
