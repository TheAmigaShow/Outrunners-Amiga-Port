#include "game_heap.h"
#include <exec/memory.h>
#include <proto/exec.h>
#include <stdint.h>
#include <string.h>

typedef union
{
	size_t size;
	uint64_t align;
} Allocation;

static void *pool;
static struct SignalSemaphore pool_lock;

bool amiga_heap_open(void)
{
	InitSemaphore(&pool_lock);
	pool = CreatePool(MEMF_FAST, 32768, 8192);
	return pool != NULL;
}

void amiga_heap_close(void)
{
	/* The game has returned to its DOS stack and its audio process is done. */
	DeletePool(pool);
	pool = NULL;
}

void *amiga_malloc(size_t size)
{
	if (size > PTRDIFF_MAX - sizeof(Allocation))
		return NULL;

	ObtainSemaphore(&pool_lock);
	Allocation *allocation = AllocPooled(pool, sizeof(*allocation) + size);
	ReleaseSemaphore(&pool_lock);
	if (!allocation)
		return NULL;
	allocation->size = size;
	return allocation + 1;
}

void amiga_free(void *pointer)
{
	if (pointer)
	{
		Allocation *allocation = (Allocation *)pointer - 1;
		const size_t bytes = sizeof(*allocation) + allocation->size;
		ObtainSemaphore(&pool_lock);
		FreePooled(pool, allocation, bytes);
		ReleaseSemaphore(&pool_lock);
	}
}

void *amiga_calloc(size_t count, size_t size)
{
	if (size && count > SIZE_MAX / size)
		return NULL;
	size *= count;
	void *pointer = amiga_malloc(size);
	if (pointer)
		memset(pointer, 0, size);
	return pointer;
}

void *amiga_realloc(void *pointer, size_t size)
{
	if (!size)
	{
		amiga_free(pointer);
		return NULL;
	}

	void *replacement = amiga_malloc(size);
	if (replacement && pointer)
	{
		size_t old_size = ((Allocation *)pointer - 1)->size;
		memcpy(replacement, pointer, old_size < size ? old_size : size);
		amiga_free(pointer);
	}
	return replacement;
}
