#ifndef ORUNNERS_AMIGA_GAME_HEAP_H
#define ORUNNERS_AMIGA_GAME_HEAP_H

#include <stdbool.h>
#include <stdlib.h>

bool amiga_heap_open(void);
void amiga_heap_close(void);
void *amiga_malloc(size_t size);
void *amiga_calloc(size_t count, size_t size);
void *amiga_realloc(void *pointer, size_t size);
void amiga_free(void *pointer);

/* Only game translation units use the Fast RAM pool. The C library keeps its
   own heap for file descriptors, stdio and the command line through exit. */
#define malloc(size) amiga_malloc(size)
#define calloc(count, size) amiga_calloc(count, size)
#define realloc(pointer, size) amiga_realloc(pointer, size)
#define free(pointer) amiga_free(pointer)
#endif
