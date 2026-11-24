/**
 *    Name:        video.c
 *    Description: Sega System Multi 32 video: the tilemaps, text, bitmap and backdrop layers, the sprite
 *                 frame buffers and the two mixers, drawing each frame of both screens from the board's
 *                 memories
 *    Version:     0.1.0
 *    Created:     13-10-2026
 *    Modified:    06-11-2026
 *    Author:      Sebastian Toyos
 *
 *    license:BSD-3-Clause
 *    Behavior follows MAME's sega/segas32_v.cpp (Aaron Giles) pixel for pixel, as it draws the Multi 32
 *    (screen_update_multi32_left and _right): tools/core/system32/reference/video builds that file, and
 *    tools/check_scenarios.py --video compares its frames with these. MAME's terms are in
 *    tools/core/system32/reference/mame/LICENSE.
 *
 *    How a frame is made: every layer draws a palette offset per pixel (0 is transparent) into a buffer
 *    of its own, through its clip windows. The sprites were drawn before, into the hidden one of two
 *    frame buffers, when the board sent the list. The mixer then takes, pixel by pixel, the topmost
 *    layer that is not transparent in the order its priority registers give, maybe blends it with the
 *    one below, and looks the color up in the palette.
 *
 *    The two screens share the video RAM, so their tilemaps, text, bitmap and backdrop are the same; each
 *    has a mixer and a palette of its own, and sprites of its own: a sprite goes to one screen's frame
 *    buffers or the other's, as bit 11 of its fourth word says.
 *
 *    Video RAM holds the tilemap pages (32 x 16 cells of a 16x16 tile, 0x400 bytes each), the text cells
 *    and their graphics, the bitmap layer, the line tables, and the registers at 0x31FF00.
 */
#include "s32/video.h"
#include "s32/workers.h"

#include "s32/machine.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define WIDTH   S32_VIDEO_MAX_WIDTH
#define HEIGHT  S32_VIDEO_HEIGHT

static void *packed_output;
static bool packed_565, half_scenery, pair_scenery_rows;
static s32_packed_format packed_format;
static uint16_t packed_colors[32768];

void s32_video_pair_scenery_rows(bool enabled)
{
	pair_scenery_rows = enabled;
}
static bool (*scenery_filter)(uint32_t);

void s32_video_set_output(void *buffer, s32_packed_format format, bool half)
{
	packed_output = buffer;
	packed_format = format;
	packed_565 = format != S32_RGB332;
	half_scenery = half;
	if (!buffer)
		return;
	for (unsigned c = 0; c < 32768; c++)
	{
		const unsigned r = c & 31, g = (c >> 5) & 31, b = c >> 10;
		uint16_t pen = (uint16_t)(r << 11 | g << 6 | b);
		if (format == S32_RGB332) pen = (r & 28) << 3 | (g & 28) | b >> 3;
		if (format == S32_BGR565 || format == S32_BGR565_SWAP) pen = b << 11 | g << 6 | r;
		if (format == S32_RGB565_SWAP || format == S32_BGR565_SWAP) pen = (pen >> 8) | (pen << 8);
		packed_colors[c] = pen;
	}
}

/* The layers in the mixer's order: its registers come in this order */
enum
{
	LAYER_TEXT,
	LAYER_NBG0,
	LAYER_NBG1,
	LAYER_NBG2,
	LAYER_NBG3,
	LAYER_BITMAP,
	LAYER_SPRITES,
	LAYER_BACKDROP,
	LAYERS
};

/* A rectangle with inclusive edges (the clip windows keep one past their right and bottom edges) */
typedef struct area
{
	int left;
	int right;
	int top;
	int bottom;
} area;

/* What a layer drew: a palette offset per pixel, and the rows where everything is transparent */
typedef struct layer
{
	uint16_t pixels[HEIGHT][WIDTH];
	bool empty[HEIGHT];
} layer;

/* Where the clip windows turn a layer on and off along each row: lists of columns that start at the
   left edge and end one past the right edge, one for each set of windows (0-31) and one for each row
   whose windows come from the line table (32 + the row) */
typedef struct clip_runs
{
	uint8_t list_of_row[256];
	uint16_t lists[256][16];
} clip_runs;

/* The source of one row of a layer: its pixel at a screen column */
typedef uint16_t (*pixel_source)(const void *row, int x);
/* Draw [first, end) and return the number of transparent pixels. */
typedef int (*span_source)(const void *row, uint16_t *pixels, int first, int end);

/* A layer in the mixer's list */
typedef struct mix_layer
{
	uint16_t palette_base;
	uint16_t sprite_blend;      /* sprite groups this layer blends with */
	uint8_t blend;              /* layers this one blends with */
	uint8_t index;              /* LAYER_ */
	uint8_t priority;           /* the register's priority << 3 | the layer's own rank */
	uint8_t shift;              /* of the pixel, before the palette base is added */
	bool plain;                 /* ready for direct RGB output at zero offset and without blending */
	uint8_t offsets;            /* color offsets 0 or 1, or none (2) */
} mix_layer;

/* What one sprite draws with */
typedef struct sprite_drawing
{
	uint16_t *row;
	uint8_t *extra_row;         /* the extra palette's marks of the row (s32_video_use_extra_palette) */
	uint8_t extra;              /* this sprite's marks: EXTRA_COLOURS, EXTRA_GHOST */
	const area *inside;         /* the clip it draws in */
	const area *outside;        /* the clip it leaves alone, on the rows it covers */
	bool outside_row;
	bool indirect;              /* pens go through a table of 16 colors */
	bool shadow;                /* darkens what is under it instead */
	bool bits8;
	int color;
	int transparent_bits;       /* an indirect color with all these bits set is transparent */
	uint16_t table[16];
} sprite_drawing;

enum { TILE_TRANSPARENT, TILE_OPAQUE, TILE_MIXED, TILE_UNIFORM_ROWS };
enum { ADDRESSABLE_TILES = 4 * 8192 };  /* Four banks of 13-bit tile numbers. */

static struct
{
	s32_graphics graphics;
	uint8_t tile_opacity[ADDRESSABLE_TILES / 4];
	uint8_t *image_pens;                    /* s32_video_init's: the tiles' pens taken out of the image */
	const uint8_t **image_tiles;
	const uint8_t *sprite_rom;              /* s32_video_init's image of the sprite chips */
	uint32_t sprite_rom_words;

	area visible;                           /* 320, 416 or WIDE's x 224 */
	int wide_side;                          /* s32_video_set_wide_side: WIDE's columns more at each side, or 0 */
	int side;                               /* this frame's: WIDE's side (wide_side), else 0 */
	int sprite_side;                        /* the sprite list's: every sprite that much to the right */
	bool extra_used;                        /* s32_video_use_extra_palette */
	int screens_shown;                      /* s32_video_show_screens: bit 0 the left, bit 1 the right */
	uint16_t extra_palette[S32_SCREENS][S32_PALETTE_ENTRIES];
	bool extra_set[S32_SCREENS][S32_PALETTE_ENTRIES];    /* the entries s32_video_extra_colour set */
	uint8_t extra_pixels[S32_SCREENS][2][HEIGHT][WIDTH];    /* the sprite buffers' pixels' marks */
	layer layers[LAYERS];                   /* the sprites' entry is unused: they have their own buffers */
	uint16_t sprites[S32_SCREENS][2][HEIGHT][WIDTH];    /* each screen's frame buffers: one is shown, the
	                                                       list draws into the other */
	int shown_sprites;
	bool erase_pending[S32_SCREENS][2], marks_pending[S32_SCREENS][2];
	uint32_t frame[S32_SCREENS][HEIGHT][WIDTH];
	clip_runs runs;
} video;

static const uint16_t transparent_row[WIDTH] = { 0 };

/* A sprite pixel's marks (the extra palette's plane) */
#define EXTRA_COLOURS 1    /* its colour from the extra palette, where one is set */
#define EXTRA_GHOST   2    /* half its colour, half the colour under it */

/* ---- the board's memories ---- */

static bool bit(uint32_t value, int number)
{
	return (value >> number) & 1;
}

static int sign_extend(uint32_t value, int bits)
{
	const uint32_t sign = 1u << (bits - 1);
	return (int)((value & ((sign << 1) - 1)) ^ sign) - (int)sign;
}

/* x >> count, rounding down for negative numbers too */
static int shift_down(int value, int count)
{
	return value >= 0 ? value >> count : ~(~value >> count);
}

/* The video RAM and the sprite RAM are 16-bit words in the host's order (s32/machine.h): a word as it is,
   a byte at S32_BYTE_LANE from its address */
static uint16_t video_word(uint32_t index)
{
	uint16_t word;
	memcpy(&word, &s32_board.video_ram[(index & 0xffff) * 2], sizeof(word));
	return word;
}

static uint8_t video_byte(uint32_t address)
{
	return s32_board.video_ram[(address ^ S32_BYTE_LANE) & (S32_VIDEO_RAM_SIZE - 1)];
}

/* A register of the tilemap chip, by its offset from 0x31FF00 */
static uint16_t video_register(uint32_t offset)
{
	return video_word((0x1ff00 + offset) / 2);
}

/* A register of a screen's mixer, by its offset in the mixer's window */
static uint16_t mixer_register(int screen, uint32_t offset)
{
	return s32_board.mixer[screen][offset / 2];
}

static uint16_t sprite_word(uint32_t index)
{
	uint16_t word;
	memcpy(&word, &s32_board.sprite_ram[(index & 0xffff) * 2], sizeof(word));
	return word;
}

static uint32_t big_endian_word(const uint8_t *at)
{
	return (uint32_t)at[0] << 24 | (uint32_t)at[1] << 16 | (uint32_t)at[2] << 8 | at[3];
}

static void clip_area(area *target, const area *bounds)
{
	if (bounds->left > target->left)
		target->left = bounds->left;
	if (bounds->right < target->right)
		target->right = bounds->right;
	if (bounds->top > target->top)
		target->top = bounds->top;
	if (bounds->bottom < target->bottom)
		target->bottom = bounds->bottom;
}

/* ---- tiles and text cells ---- */

/* The extracted game supplies the entire tile address space. Smaller
   graphics fixtures retain the hardware wrapping used by the reference. */
static unsigned tile_index(unsigned tile)
{
	return tile < video.graphics.tile_count ? tile : tile % video.graphics.tile_count;
}

/* Build once while the tile bank is being installed, before row workers start. */
static void classify_tiles(const s32_graphics *graphics)
{
	for (unsigned tile = 0; tile < graphics->tile_count && tile < ADDRESSABLE_TILES; tile++)
	{
		const uint8_t *pens = graphics->tile_pens[tile];
		if (!pens)
			continue;
		unsigned kind;
		if (pens[0])
			kind = memchr(pens, 0, 256) ? TILE_MIXED : TILE_OPAQUE;
		else
			kind = memcmp(pens, transparent_row, 256) ? TILE_MIXED : TILE_TRANSPARENT;
		if (kind == TILE_MIXED)
		{
			/* Some road tiles change opacity only between scanlines. */
			kind = TILE_UNIFORM_ROWS;
			for (int y = 0; y < 16; y++)
			{
				const uint8_t *row = pens + 16 * y;
				if (row[0] ? memchr(row, 0, 16) != NULL : memcmp(row, transparent_row, 16) != 0)
				{
					kind = TILE_MIXED;
					break;
				}
			}
		}
		video.tile_opacity[tile >> 2] |= (uint8_t)(kind << ((tile & 3) * 2));
	}
}

/* The pen (0-15) of a pixel of a 16x16 tile */
static int tile_pen(uint32_t tile, int x, int y)
{
	return video.graphics.tile_pens[tile_index(tile)][y * 16 + x];
}

/* A pixel of a tilemap page (32 x 16 cells, 512 x 256 pixels). A cell is the tile number (13 bits, the
   bank above them), its color (bits 4-12, over the number) and its flips (bit 14 x, bit 15 y). */
static uint16_t page_pixel(int page, int bank, int y, int x)
{
	const uint16_t cell = video_word((uint32_t)page << 9 | (uint32_t)(y >> 4) << 5 | (uint32_t)(x >> 4));
	const uint32_t tile = (uint32_t)bank << 13 | (cell & 0x1fff);
	const int column = (cell & 0x4000) ? 15 - (x & 15) : (x & 15);
	const int line = (cell & 0x8000) ? 15 - (y & 15) : (y & 15);
	const int pen = tile_pen(tile, column, line);

	return pen ? (uint16_t)(((cell >> 4) & 0x1ff) << 4 | pen) : 0;
}

/* ---- clip windows ---- */

/* Window 0-4 (0x60 + 8 each: left, top, right, bottom), with one past its right and bottom edges */
static void window_of(int number, bool flip, area *window)
{
	const uint32_t at = 0x60 + 8 * (uint32_t)number;
	const int left = video_register(at) & 0x1ff;
	const int top = video_register(at + 2) & 0xff;
	const int right = (video_register(at + 4) & 0x1ff) + 1;
	const int bottom = (video_register(at + 6) & 0xff) + 1;

	if (!flip)
	{
		window->left = left + video.side;
		window->right = right + video.side;
		window->top = top;
		window->bottom = bottom;
		if (video.side && left == 0 && right >= 320)
		{
			window->left = 0;
			window->right = video.visible.right + 1;    /* (WIDE: the whole screen, the wider one) */
		}
	}
	else
	{
		window->left = video.visible.right + 1 - right;
		window->right = video.visible.right + 1 - left;
		window->top = video.visible.bottom + 1 - bottom;
		window->bottom = video.visible.bottom + 1 - top;
	}
}

static void sort_by_left(const area *windows, int *order)
{
	for (int index = 1; index < 5; index++)
	{
		const int key = order[index];
		int place = index - 1;

		while (place >= 0 && windows[order[place]].left > windows[key].left)
		{
			order[place + 1] = order[place];
			place--;
		}
		order[place + 1] = key;
	}
}

/* The list for a set of windows: taken from left to right, a window that starts inside the last run
   stretches it */
static void merge_windows(uint16_t *list, const area *windows, const int *order, int set, const area *edges)
{
	uint16_t *end = list;

	*end++ = (uint16_t)edges->left;
	for (int index = 0; index < 5; index++)
	{
		const area *window = &windows[order[index]];

		if (!bit((uint32_t)set, order[index]))
			continue;
		if (end != list + 1 && window->left <= end[-1])
		{
			if (window->right > end[-1])
				end[-1] = (uint16_t)window->right;
		}
		else
		{
			*end++ = (uint16_t)window->left;
			*end++ = (uint16_t)window->right;
		}
	}
	*end = (uint16_t)edges->right;
}

/* Windows 2 and 3 can take their columns from the line table instead, row by row (0xFFFF: none) */
static void line_window(int number, int line, bool flip, uint32_t table, const area *edges, area *window, int *crossing)
{
	const uint16_t left = video_word(table + 0x100 * (uint32_t)(number - 2) + (uint32_t)line);
	const uint16_t right = video_word(table + 0x200 + 0x100 * (uint32_t)(number - 2) + (uint32_t)line);

	if (left == 0xffff || right == 0xffff)
		*crossing &= ~(1 << number);
	else if (!flip)
	{
		window->left = (left & 0x1ff) + video.side;
		window->right = (right & 0x1ff) + 1 + video.side;
		if (video.side && (left & 0x1ff) == 0 && (right & 0x1ff) + 1 >= 320)
		{
			window->left = 0;
			window->right = video.visible.right + 1;
		}
	}
	else
	{
		window->left = video.visible.right + 1 - ((right & 0x1ff) + 1);
		window->right = video.visible.right + 1 - (left & 0x1ff);
	}
	clip_area(window, edges);
	if (window->left >= window->right)
		*crossing &= ~(1 << number);
}

/* A layer's runs: 'selected' picks windows 0-4. The first run of each row is drawn when the layer draws
   outside its windows (or has them off), and the runs alternate from there. */
static bool find_clip_runs(bool enabled, bool outside, int selected, clip_runs *runs)
{
	const bool flip = bit(video_register(0x00), 9);
	const uint16_t line_control = video_register(0x04);
	const bool line_windows = bit(line_control, 4) || bit(line_control, 5);
	const area edges = { video.visible.left, video.visible.right + 1, video.visible.top, video.visible.bottom + 1 };
	area windows[5];
	int order[5];

	memset(runs, 0, sizeof(*runs));
	runs->lists[0][0] = (uint16_t)edges.left;
	runs->lists[0][1] = (uint16_t)edges.right;
	if (!enabled)
		return true;

	for (int number = 0; number < 5; number++)
	{
		window_of(number, flip, &windows[number]);
		clip_area(&windows[number], &edges);
		order[number] = number;
	}
	sort_by_left(windows, order);
	for (int set = 1; set < 32; set++)
		if (set & selected)
			merge_windows(runs->lists[set], windows, order, set, &edges);

	for (int y = edges.top; y < edges.bottom; y++)
	{
		int crossing = 0;
		area row_windows[5];
		int row_order[5];

		for (int number = 0; number < 5; number++)
			if (bit((uint32_t)selected, number) && y >= windows[number].top && y < windows[number].bottom)
				crossing |= 1 << number;
		if (!line_windows)
		{
			runs->list_of_row[y] = (uint8_t)crossing;
			continue;
		}

		for (int number = 0; number < 5; number++)
		{
			row_windows[number] = windows[number];
			row_order[number] = number;
		}
		for (int number = 2; number <= 3; number++)
			if (bit(line_control, number + 2) && bit((uint32_t)selected, number))
				line_window(number, flip ? video.visible.bottom - y : y, flip, (uint32_t)(line_control >> 10) * 0x400,
					&edges, &row_windows[number], &crossing);
		sort_by_left(row_windows, row_order);
		merge_windows(runs->lists[32 + y], row_windows, row_order, crossing, &edges);
		runs->list_of_row[y] = (uint8_t)(32 + y);
	}
	return outside;
}

/* One row of a layer through its runs, which alternate between drawn and cleared starting with 'drawn'.
   Overlapping windows can make a run start before the last one ended: it is drawn again. */
static void draw_row(layer *out, int y, const uint16_t *runs, bool drawn, pixel_source pixel, span_source span, const void *row)
{
	uint16_t *pixels = out->pixels[y];
	int transparent = 0;

	for (;; runs++, drawn = !drawn)
	{
		if (drawn && span)
			transparent += span(row, pixels, runs[0], runs[1] < WIDTH ? runs[1] : WIDTH);
		else if (drawn)
		{
			for (int x = runs[0]; x < runs[1] && x < WIDTH; x++)
			{
				pixels[x] = pixel(row, x);
				transparent += pixels[x] == 0;
			}
		}
		else
		{
			for (int x = runs[0]; x < runs[1] && x < WIDTH; x++)
				pixels[x] = 0;
			transparent += runs[1] - runs[0];
		}
		if (runs[1] > video.visible.right)
			break;
	}
	out->empty[y] = transparent == video.visible.right + 1 - video.visible.left;
}

/* Expand the sampled scenery within each clip run. The mixer reuses the
   even source row on the following scanline. Sprites and HUD use their own rows. */
static void half_row(layer *out, int y, const uint16_t *runs, bool drawn, pixel_source source, const void *row)
{
	uint16_t *dst = out->pixels[y];
	uint16_t any = 0;
	for (;; runs++, drawn = !drawn)
	{
		int x = runs[0], end = runs[1] < WIDTH ? runs[1] : WIDTH;
		if (!drawn)
		{
			if (x < end) memset(dst + x, 0, (end - x) * sizeof(*dst));
		}
		else
			while (x < end)
			{
				const uint16_t pen = source(row, x);
				any |= pen;
				dst[x++] = pen;
				if (x < end) dst[x++] = pen;
			}
		if (end > video.visible.right) break;
	}
	out->empty[y] = !any;
}

/* ---- the layers ---- */

/* The four pages of tilemap 0-3 (upper left, upper right, lower left, lower right) and its tile bank */
static int pages_of(int number, int *pages)
{
	const uint16_t upper = video_register(0x40 + 4 * (uint32_t)number);
	const uint16_t lower = video_register(0x42 + 4 * (uint32_t)number);

	pages[0] = upper & 0x7f;
	pages[1] = (upper >> 8) & 0x7f;
	pages[2] = lower & 0x7f;
	pages[3] = (lower >> 8) & 0x7f;
	/* the I/O chip's port H gives each tilemap a bank of its own (System 32: one, with register 0x00) */
	return (s32_board.tilebank_external >> (2 * number)) & 3;
}

/* Register 0x00 flips tilemap 0-3 against the whole screen's flip (bit 9); bit 8 keeps them from
   flipping vertically */
static void flips_of(int number, bool *flip_x, bool *flip_y)
{
	const uint16_t control = video_register(0x00);
	const bool screen_flip = bit(control, 9);
	const bool layer_flip = bit(control, number);

	*flip_x = layer_flip ? !screen_flip : screen_flip;
	*flip_y = (layer_flip && !bit(control, 8)) ? !screen_flip : screen_flip;
}

typedef struct zoomed_row
{
	int pages[2];       /* left and right page of the row's half */
	int bank;
	int line;           /* the row within the pages */
	uint32_t x_start;   /* source column at the left edge, 12.20 */
	uint32_t x_step;
} zoomed_row;

static uint16_t zoomed_pixel(const void *source, int x)
{
	const zoomed_row *row = (const zoomed_row *)source;
	const uint32_t column = row->x_start + row->x_step * (uint32_t)(x - video.visible.left - video.side);

	return page_pixel(row->pages[(column >> 29) & 1], row->bank, row->line, (column >> 20) & 0x1ff);
}

typedef struct sprite_grouping
{
	uint8_t shift;      /* where the group is in a sprite pixel */
	uint8_t mask;
	uint8_t base;       /* the first group's priority register */
} sprite_grouping;

/* The sprite groupings register 0x4C picks */
static const sprite_grouping groupings[16] = {
	{ 14, 0x00, 0x01 }, { 14, 0x01, 0x02 }, { 13, 0x03, 0x04 }, { 12, 0x07, 0x08 },
	{ 14, 0x01, 0x00 }, { 13, 0x03, 0x00 }, { 12, 0x07, 0x00 }, { 11, 0x0f, 0x00 },
	{ 14, 0x01, 0x00 }, { 13, 0x03, 0x00 }, { 12, 0x07, 0x00 }, { 11, 0x0f, 0x00 },
	{ 13, 0x01, 0x00 }, { 12, 0x03, 0x00 }, { 11, 0x07, 0x00 }, { 10, 0x0f, 0x00 }
};

/* Which foreground pixels can hide NBG0 in this screen's priority order. */
typedef struct sky_cover
{
	bool road;
	bool sprites;
	uint8_t group_above[256];   /* the sprite group is wholly in the high byte */
	uint16_t shadow_pen;
} sky_cover;

/* What a zoomed tilemap draws with, the same on every row (its rows on the workers' bands) */
typedef struct zoomed_layer
{
	int number;
	int pages[4];
	int bank;
	bool first_drawn;
	uint32_t x_start;
	uint32_t x_step;
	uint32_t source_y;  /* at the top row, 12.20 */
	uint32_t y_step;
	const sky_cover *cover;
} zoomed_layer;

/* Use a register-sized coverage mask per scanline, one bit per complete
   16-pixel span. Partial spans follow the normal sampler at the same phase. */
static uint32_t covered_spans(const sky_cover *cover, int y)
{
	const uint16_t *road = cover->road && !video.layers[LAYER_NBG2].empty[y]
		? video.layers[LAYER_NBG2].pixels[y] : transparent_row;
	const uint16_t *sprites = video.sprites[0][video.shown_sprites][y];
	uint32_t mask = 0;
	for (int x = 0; x + 16 <= video.visible.right + 1; x += 16)
	{
		int i = x;
		for (; i < x + 16; i++)
		{
			if (road[i] & 0x1fff)
				continue;
			const uint16_t pixel = sprites[i];
			if (!cover->sprites || !cover->group_above[pixel >> 8]
				|| (pixel & cover->shadow_pen) == cover->shadow_pen)
				break;
		}
		if (i == x + 16)
			mask |= UINT32_C(1) << (x / 16);
	}
	return mask;
}

static void draw_visible_sky(const zoomed_row *row, int y, uint32_t covered)
{
	layer *sky = &video.layers[LAYER_NBG0];
	uint16_t any = 0;
	for (int x = 0; x <= video.visible.right; covered >>= 1)
	{
		const int end = x + 16 <= video.visible.right + 1 ? x + 16 : video.visible.right + 1;
		if (covered & 1)
			x = end;
		else
			for (; x < end; x++)
			{
				const uint16_t pixel = zoomed_pixel(row, x);
				sky->pixels[y][x] = pixel;
				any |= pixel;
			}
	}
	sky->empty[y] = !any;
}

static zoomed_row zoomed_row_at(const zoomed_layer *zoomed, int y)
{
	const uint32_t source_y = zoomed->source_y + (uint32_t)(y - video.visible.top) * zoomed->y_step;
	const int half = (source_y >> 27) & 2;
	return (zoomed_row) {
		{ zoomed->pages[half], zoomed->pages[half + 1] }, zoomed->bank, (source_y >> 20) & 0xff,
		zoomed->x_start, zoomed->x_step
	};
}

static void draw_zoomed_rows(void *context, int first, int last)
{
	const zoomed_layer *zoomed = (const zoomed_layer *)context;

	for (int y = first; y <= last; y++)
	{
		if (half_scenery && (y & 1)) continue;
		const zoomed_row row = zoomed_row_at(zoomed, y);

		if (half_scenery)
		{
			half_row(&video.layers[LAYER_NBG0 + zoomed->number], y, video.runs.lists[video.runs.list_of_row[y]],
				zoomed->first_drawn, zoomed_pixel, &row);
			continue;
		}
		if (zoomed->cover)
		{
			draw_visible_sky(&row, y, covered_spans(zoomed->cover, y));
			continue;
		}
		draw_row(&video.layers[LAYER_NBG0 + zoomed->number], y, video.runs.lists[video.runs.list_of_row[y]],
			zoomed->first_drawn, zoomed_pixel, NULL, &row);
	}
}

/* NBG0 and NBG1: scrolled (with a fraction) and zoomed about a center. The steps are screen pixels per
   source pixel (0x200 = 1); the source moves by their reciprocal, in 12.20. */
static zoomed_layer prepare_zoomed(int number, const sky_cover *cover)
{
	const uint32_t scroll = 0x10 + 8 * (uint32_t)number;
	const uint32_t center = 0x30 + 4 * (uint32_t)number;
	int pages[4];
	const int bank = pages_of(number, pages);
	bool flip_x;
	bool flip_y;
	int x_zoom = video_register(0x50 + 4 * (uint32_t)number) & 0xfff;
	int y_zoom = bit(video_register(0x00), 14) ? video_register(0x52 + 4 * (uint32_t)number) & 0xfff : x_zoom;
	uint32_t x_step;
	uint32_t y_step;
	uint32_t x_start;
	uint32_t source_y;
	const bool first_drawn = find_clip_runs(bit(video_register(0x02), 11 + number), bit(video_register(0x02), 6 + number),
		(video_register(0x06) >> (4 * number)) & 0x0f, &video.runs);

	flips_of(number, &flip_x, &flip_y);
	if (x_zoom < 0x80)
		x_zoom = 0x80;
	if (y_zoom < 0x80)
		y_zoom = 0x80;
	x_step = (0x200u << 20) / (uint32_t)x_zoom;
	y_step = (0x200u << 20) / (uint32_t)y_zoom;

	/* the scroll with its fraction, less the center (one bit more of it while zoomed) */
	x_start = (uint32_t)(video_register(scroll + 2) & 0x3ff) << 20;
	x_start += (uint32_t)(video_register(scroll) & 0xff00) << 4;
	source_y = (uint32_t)(video_register(scroll + 6) & 0x1ff) << 20;
	source_y += (uint32_t)(video_register(scroll + 4) & 0xfe00) << 4;
	x_start -= (uint32_t)sign_extend(video_register(center), x_zoom != 0x200 ? 10 : 9) * x_step;
	source_y -= (uint32_t)sign_extend(video_register(center + 2), y_zoom != 0x200 ? 10 : 9) * y_step;
	x_start += (uint32_t)video.visible.left * x_step;
	source_y += (uint32_t)video.visible.top * y_step;
	if (flip_y)
	{
		source_y += (uint32_t)(video.visible.bottom - 2 * video.visible.top) * y_step;
		y_step = 0u - y_step;
	}
	if (flip_x)
	{
		x_start += (uint32_t)(video.visible.right - 2 * video.side - 2 * video.visible.left) * x_step;
		x_step = 0u - x_step;
	}

	return (zoomed_layer) {
		number, { pages[0], pages[1], pages[2], pages[3] }, bank, first_drawn, x_start, x_step, source_y, y_step, cover
	};
}

static void draw_zoomed(int number, const sky_cover *cover)
{
	zoomed_layer zoomed = prepare_zoomed(number, cover);
	s32_workers_rows(draw_zoomed_rows, &zoomed, video.visible.top, video.visible.bottom);
}

typedef struct scrolled_row
{
	int pages[2];
	int bank;
	int line;
	int x_start;        /* source column at the left edge */
	int x_step;
} scrolled_row;

static uint16_t scrolled_pixel(const void *source, int x)
{
	const scrolled_row *row = source;
	const unsigned col = row->x_start + row->x_step * (x - video.visible.left - video.side);
	return page_pixel(row->pages[(col >> 9) & 1], row->bank, row->line, col & 511);
}

/* A scrolled layer advances one source column per output pixel, so only
   crossing a 16-pixel cell boundary needs another map and tile lookup. */
static int scrolled_span(const void *source, uint16_t *pixels, int x, int end)
{
	const scrolled_row *row = source;
	unsigned column = (unsigned)(row->x_start + row->x_step * (x - video.visible.left - video.side));
	int transparent = 0;

	while (x < end)
	{
		const int within = column & 15;
		int count = row->x_step > 0 ? 16 - within : within + 1;
		if (count > end - x)
			count = end - x;

		const unsigned page = row->pages[(column >> 9) & 1];
		const uint16_t cell = video_word((page << 9) | ((unsigned)(row->line >> 4) << 5) | ((column >> 4) & 31));
		const unsigned tile = tile_index(((unsigned)row->bank << 13) | (cell & 0x1fff));
		unsigned kind = (video.tile_opacity[tile >> 2] >> ((tile & 3) * 2)) & 3;
		const int line = (row->line & 15) ^ ((cell & 0x8000) ? 15 : 0);
		const uint8_t *pens = video.graphics.tile_pens[tile];
		if (kind == TILE_UNIFORM_ROWS)
			kind = pens[line * 16] ? TILE_OPAQUE : TILE_TRANSPARENT;
		column += row->x_step * count;
		if (kind == TILE_TRANSPARENT)
		{
			memset(pixels + x, 0, (size_t)count * sizeof(*pixels));
			transparent += count;
			x += count;
			continue;
		}
		pens += line * 16;
		const uint16_t color = cell & 0x1ff0;
		int index = (cell & 0x4000) ? 15 - within : within;
		const int step = (cell & 0x4000) ? -row->x_step : row->x_step;

		if (kind == TILE_OPAQUE)
		{
			while (count--)
			{
				pixels[x++] = color | pens[index];
				index += step;
			}
			continue;
		}
		while (count--)
		{
			const uint8_t pen = pens[index];
			pixels[x++] = pen ? (color | pen) : 0;
			transparent += pen == 0;
			index += step;
		}
	}
	return transparent;
}

/* What a scrolled tilemap draws with, the same on every row */
typedef struct scrolled_layer
{
	int number;
	uint32_t table;
	bool row_scroll;
	bool row_select;
	int x_scroll;
	int y_scroll;
	int pages[4];
	int bank;
	bool flip_x;
	bool flip_y;
	bool first_drawn;
} scrolled_layer;

static scrolled_row scrolled_row_at(const scrolled_layer *scrolled, int y)
{
	const int line = scrolled->flip_y ? video.visible.bottom - y : y;
	int source_y = scrolled->y_scroll + line;
	scrolled_row row;

	row.x_start = (scrolled->flip_x ? video.visible.right - 2 * video.side : video.visible.left) + scrolled->x_scroll;
	row.x_step = scrolled->flip_x ? -1 : 1;
	if (scrolled->row_scroll)
		row.x_start += video_word(scrolled->table + (uint32_t)line) & 0x3ff;
	if (scrolled->row_select)
		source_y = (scrolled->y_scroll + video_word(scrolled->table + 0x200 + (uint32_t)line)) & 0x1ff;
	row.pages[0] = scrolled->pages[(source_y >> 7) & 2];
	row.pages[1] = scrolled->pages[((source_y >> 7) & 2) + 1];
	row.bank = scrolled->bank;
	row.line = source_y & 0xff;
	return row;
}

static void draw_scrolled_rows(void *context, int first, int last)
{
	const scrolled_layer *scrolled = (const scrolled_layer *)context;

	for (int y = first; y <= last; y++)
	{
		if (half_scenery && (y & 1)) continue;
		scrolled_row row = scrolled_row_at(scrolled, y);
		if (half_scenery)
		{
			half_row(&video.layers[LAYER_NBG0 + scrolled->number], y, video.runs.lists[video.runs.list_of_row[y]],
				scrolled->first_drawn, scrolled_pixel, &row);
			continue;
		}
		draw_row(&video.layers[LAYER_NBG0 + scrolled->number], y, video.runs.lists[video.runs.list_of_row[y]],
			scrolled->first_drawn, NULL, scrolled_span, &row);
	}
}

/* NBG2 and NBG3: scrolled, and row by row the line table can add to the column (row scroll) or pick
   the source row (row select). Register 0x04 turns those on; its top bits place the table. */
static scrolled_layer prepare_scrolled(int number)
{
	const uint16_t line_control = video_register(0x04);
	const bool line_windows = bit(line_control, number + 2);
	scrolled_layer scrolled;

	scrolled.number = number;
	scrolled.table = (uint32_t)(line_control >> 10) * 0x400 + 0x100 * (uint32_t)(number - 2);
	scrolled.row_scroll = bit(line_control, number - 2) && !line_windows;
	scrolled.row_select = bit(line_control, number) && !line_windows;
	scrolled.x_scroll = (video_register(0x12 + 8 * (uint32_t)number) & 0x3ff) - (video_register(0x30 + 4 * (uint32_t)number) & 0x1ff);
	scrolled.y_scroll = video_register(0x16 + 8 * (uint32_t)number) & 0x1ff;
	scrolled.bank = pages_of(number, scrolled.pages);
	scrolled.first_drawn = find_clip_runs(bit(video_register(0x02), 11 + number), bit(video_register(0x02), 6 + number),
		(video_register(0x06) >> (4 * number)) & 0x0f, &video.runs);
	flips_of(number, &scrolled.flip_x, &scrolled.flip_y);
	return scrolled;
}

static void draw_scrolled(int number)
{
	scrolled_layer scrolled = prepare_scrolled(number);
	s32_workers_rows(draw_scrolled_rows, &scrolled, video.visible.top, video.visible.bottom);
}

/* The text layer, with no scroll and no windows; the screen's flip turns the whole 512 x 256 map */
static void draw_text_rows(void *context, int first, int last)
{
	const bool flip = *(const bool *)context;

	const uint16_t control = video_register(0x5c);
	const uint32_t page = (uint32_t)((control >> 4) & 31) << 11;
	const uint32_t bank = (uint32_t)(control & 7) << 13;
	layer *text = &video.layers[LAYER_TEXT];
	const int left = video.side;
	const int right = left + (video.side ? 320 : video.visible.right + 1);

	for (int y = first; y <= last; y++)
	{
		const int sy = flip ? 255 - y : y;
		const uint32_t cells = page + (uint32_t)(sy >> 3) * 64;
		const uint32_t glyph_row = bank + (uint32_t)(sy & 7) * 2;
		bool empty = true;

		if (left)
		{
			memset(text->pixels[y], 0, left * sizeof(uint16_t));
			memset(&text->pixels[y][right], 0, (video.visible.right + 1 - right) * sizeof(uint16_t));
		}
		for (int x = left; x < right; x += 8)
		{
			const int col = (x - left) >> 3;
			const uint16_t cell = video_word(cells + (flip ? 63 - col : col));
			const uint32_t at = glyph_row + (uint32_t)(cell & 511) * 16;
			uint32_t pens = video_word(at) | (uint32_t)video_word(at + 1) << 16;
			uint16_t *dst = &text->pixels[y][x];
			if (!pens)
			{
				memset(dst, 0, 8 * sizeof(*dst));
				continue;
			}
			empty = false;
			const uint16_t base = (cell >> 9) << 4;
			/* Low byte first in each board word, high nibble first in each byte. */
			for (int n = 0; n < 8; n += 2, pens >>= 8)
			{
				const unsigned hi = (pens >> 4) & 15, lo = pens & 15;
				dst[flip ? 7 - n : n] = hi ? base | hi : 0;
				dst[flip ? 6 - n : n + 1] = lo ? base | lo : 0;
			}
		}
		text->empty[y] = empty;
	}
}

static void draw_text(void)
{
	bool flip = bit(video_register(0x00), 9);

	s32_workers_rows(draw_text_rows, &flip, video.visible.top, video.visible.bottom);
}

typedef struct bitmap_row
{
	uint32_t start;     /* the row in video RAM: a byte offset (8 bits a pixel) or a word (4 bits) */
	int x_scroll;
	int color;
	bool bits8;
} bitmap_row;

/* Read one board word for each group of two bytes or four nibbles. A zero
   word proves the whole group transparent at any palette base. */
static int bitmap_span(const void *source, uint16_t *pixels, int first, int end)
{
	const bitmap_row *row = source;
	const unsigned bits = row->bits8 ? 8 : 4;
	const unsigned mask = (1u << bits) - 1;
	const unsigned per_word = 16 / bits;
	const uint32_t base = row->bits8 ? row->start / 2 : row->start;
	int transparent = 0;
	for (int x = first; x < end; )
	{
		const unsigned col = (x - video.side + row->x_scroll) & 511;
		const unsigned within = col & (per_word - 1);
		unsigned pens = video_word(base + col / per_word) >> (within * bits);
		int count = (int)(per_word - within);
		if (count > end - x)
			count = end - x;
		if (!pens)
		{
			memset(pixels + x, 0, count * sizeof(*pixels));
			transparent += count;
			x += count;
			continue;
		}
		for (int n = 0; n < count; n++, x++, pens >>= bits)
		{
			const unsigned pen = pens & mask;
			pixels[x] = pen ? (uint16_t)(row->color | pen) : 0;
			transparent += pen == 0;
		}
	}
	return transparent;
}

/* What the bitmap layer draws with, the same on every row */
typedef struct bitmap_layer
{
	bool bits8;
	int x_scroll;
	int y_scroll;
	int color;
	bool first_drawn;
} bitmap_layer;

static void draw_bitmap_rows(void *context, int first, int last)
{
	const bitmap_layer *bitmap = (const bitmap_layer *)context;

	for (int y = first; y <= last; y++)
	{
		const bitmap_row row = {
			bitmap->bits8 ? 512 * (uint32_t)((y + bitmap->y_scroll) & 0xff) : 128 * (uint32_t)((y + bitmap->y_scroll) & 0x1ff),
			bitmap->x_scroll, bitmap->color, bitmap->bits8
		};

		const uint16_t *runs = video.runs.lists[video.runs.list_of_row[y]];
		bool drawn = bitmap->first_drawn;
		while (runs[0] == runs[1] && runs[1] <= video.visible.right)
		{
			runs++;
			drawn = !drawn;
		}
		if (!drawn && runs[1] > video.visible.right)
			video.layers[LAYER_BITMAP].empty[y] = true;
		else
			draw_row(&video.layers[LAYER_BITMAP], y, runs, drawn, NULL, bitmap_span, &row);
	}
}

/* The bitmap layer: 512 pixels a row from the start of video RAM, 4 or 8 bits each (register 0x00
   bit 11), scrolled by registers 0x88 and 0x8A, its palette from 0x8C; window 4 clips it */
static void draw_bitmap(void)
{
	bitmap_layer bitmap;

	bitmap.bits8 = bit(video_register(0x00), 11);
	bitmap.x_scroll = video_register(0x88) & 0x1ff;
	bitmap.y_scroll = video_register(0x8a) & 0x1ff;
	bitmap.color = (video_register(0x8c) << 4) & 0x1fff0 & ~((1 << (bitmap.bits8 ? 8 : 4)) - 1);
	bitmap.first_drawn = find_clip_runs(bit(video_register(0x02), 15), bit(video_register(0x02), 10), 0x10, &video.runs);
	s32_workers_rows(draw_bitmap_rows, &bitmap, video.visible.top, video.visible.bottom);
}

/* The backdrop: one color for the screen, or one per line (register 0x5E bit 15), wrapping at 511 */
static int backdrop_pen(int y)
{
	const uint16_t control = video_register(0x5e);
	const bool flip = bit(video_register(0x00), 9);

	if (flip) y = video.visible.bottom - y;
	return (control & 0x1e00) + (bit(control, 15) ? (control + y) & 0x1ff : 0);
}

static void draw_backdrop(void)
{
	for (int y = video.visible.top; y <= video.visible.bottom; y++)
	{
		const int color = backdrop_pen(y);
		uint16_t *row = video.layers[LAYER_BACKDROP].pixels[y];

		for (int x = video.visible.left; x <= video.visible.right; x++)
			row[x] = (uint16_t)color;
	}
}

/* Every layer the registers leave on (0x02 and 0x8E turn them off, 0x00 bits 12-13 too for NBG2-3);
   the backdrop is always there. The layers drawn, as bits by LAYER_. */
s32_tile_observer s32_video_tile_observer = NULL;

/* Every cell of the pages tilemap 'number' shows, to s32_video_tile_observer */
static void observe_tilemap(int number)
{
	int pages[4];
	const int bank = pages_of(number, pages);

	for (int index = 0; index < 4; index++)
	{
		bool seen = false;

		for (int earlier = 0; earlier < index; earlier++)
			seen = seen || pages[earlier] == pages[index];
		if (seen)
			continue;
		for (uint32_t cell = 0; cell < 32 * 16; cell++)
		{
			const uint16_t word = video_word((uint32_t)pages[index] << 9 | cell);

			s32_video_tile_observer(number, (uint32_t)bank << 13 | (word & 0x1fff), (word >> 4) & 0x1ff);
		}
	}
}

static void draw_background(int drawn)
{
	sky_cover cover;
	const int priority = mixer_register(0, 0x20 + 2 * LAYER_NBG0) & 15;
	const bool blend = bit(mixer_register(0, 0x4e), 11);
	cover.road = bit(drawn, LAYER_NBG2)
		&& (mixer_register(0, 0x20 + 2 * LAYER_NBG2) & 15) > priority
		&& !(blend && ((mixer_register(0, 0x30 + 2 * LAYER_NBG2) >> 6) & 255));
	cover.sprites = false;
	const sprite_grouping grouping = groupings[mixer_register(0, 0x4c) & 15];
	cover.shadow_pen = ((1u << grouping.shift) - 1) & 0x3ffe;
	if (!(s32_board.sprite_control_latched[2] & 3) && !video.extra_used)
	{
		const int per_group = 1 << (grouping.shift - 8);
		for (int high = 0; high < 256; high += per_group)
		{
			const int group = (high / per_group) & grouping.mask;
			const bool above = (mixer_register(0, 2 * (grouping.base | group)) & 15) >= priority;
			memset(cover.group_above + high, above, per_group);
			cover.sprites |= above;
		}
	}
	draw_zoomed(0, cover.sprites || cover.road ? &cover : NULL);
}

/* The layers that are on, of those 'wanted' (LAYER_* bits): the ones the mixers of the screens shown take */
static int enabled_layers(int wanted)
{
	const uint16_t control = video_register(0x00);
	const uint16_t off = video_register(0x02);
	const uint16_t more_off = video_register(0x8e);
	int drawn = 0;
	if (bit((uint32_t)wanted, LAYER_NBG0) && !bit(off, 0) && !bit(more_off, 1))
		drawn |= 1 << LAYER_NBG0;
	if (bit((uint32_t)wanted, LAYER_NBG1) && !bit(off, 1) && !bit(more_off, 2))
		drawn |= 1 << LAYER_NBG1;
	if (bit((uint32_t)wanted, LAYER_NBG2) && !bit(off, 2) && !bit(more_off, 3) && !bit(control, 12))
		drawn |= 1 << LAYER_NBG2;
	if (bit((uint32_t)wanted, LAYER_NBG3) && !bit(off, 3) && !bit(more_off, 4) && !bit(control, 13))
		drawn |= 1 << LAYER_NBG3;
	if (bit((uint32_t)wanted, LAYER_TEXT) && !bit(off, 4) && !bit(more_off, 0))
		drawn |= 1 << LAYER_TEXT;
	if (bit((uint32_t)wanted, LAYER_BITMAP) && !bit(off, 5) && !bit(more_off, 5))
		drawn |= 1 << LAYER_BITMAP;
	return drawn;
}

static void draw_tile_layers(int drawn)
{
	const bool sky = bit((uint32_t)drawn, LAYER_NBG0);
	/* A second mixer or a tile observer needs the original shared layer. Windowed
	   skies retain their clip runs. Keep ghosts on the full compositing path. */
	const bool defer_sky = sky && !half_scenery && video.screens_shown == 1 && !s32_video_tile_observer
		&& !video.extra_used && !bit(video_register(0x02), 11);
	if (sky && !defer_sky)
	{
		draw_zoomed(0, NULL);
		if (s32_video_tile_observer != NULL)
			observe_tilemap(0);
	}
	if (bit((uint32_t)drawn, LAYER_NBG1))
	{
		draw_zoomed(1, NULL);
		if (s32_video_tile_observer != NULL)
			observe_tilemap(1);
	}
	if (bit((uint32_t)drawn, LAYER_NBG2))
	{
		draw_scrolled(2);
		if (s32_video_tile_observer != NULL)
			observe_tilemap(2);
	}
	if (bit((uint32_t)drawn, LAYER_NBG3))
	{
		draw_scrolled(3);
		if (s32_video_tile_observer != NULL)
			observe_tilemap(3);
	}
	if (defer_sky)
		draw_background(drawn);
}

/* ---- sprites ---- */

s32_sprite_observer s32_video_sprite_observer = NULL;

static void put_sprite_pixel(const sprite_drawing *sprite, int x, int pen, int skipped_pen)
{
	int pixel;

	if (x < sprite->inside->left || x > sprite->inside->right || pen == skipped_pen)
		return;
	if (sprite->outside_row && x >= sprite->outside->left && x <= sprite->outside->right)
		return;
	if (!sprite->indirect)
	{
		if (pen == 0)
			return;
		pixel = sprite->color | pen;
	}
	else
	{
		pixel = sprite->bits8 ? sprite->table[pen >> 4] | (pen & 0x0f) : sprite->table[pen];
		if ((pixel & sprite->transparent_bits) == sprite->transparent_bits)
			return;
	}
	if (!sprite->shadow)
	{
		sprite->row[x] = (uint16_t)pixel;
		if (sprite->extra_row)
			sprite->extra_row[x] = sprite->extra;
	}
	else
		sprite->row[x] &= 0x7fff;
}

/* A 32-bit word of sprite graphics: from the ROM bank, or from sprite RAM itself */
static uint32_t sprite_graphics(bool from_ram, uint32_t bank, uint32_t index)
{
	if (from_ram)
	{
		/* the bytes in their order in sprite RAM, each at S32_BYTE_LANE in its word */
		const uint8_t *at = &s32_board.sprite_ram[(index & 0x7fff) * 4];
		const uint8_t bytes[4] = { at[0 ^ S32_BYTE_LANE], at[1 ^ S32_BYTE_LANE], at[2 ^ S32_BYTE_LANE],
		                           at[3 ^ S32_BYTE_LANE] };
		return big_endian_word(bytes);
	}
	return video.graphics.sprite_word((bank << 20) | (index & 0xfffff));
}

/* One row of a sprite: words of 8 pens (4 bits) or 4 (8 bits), each pen repeated or skipped by the
   zoom. The first and last pen of a word are transparent at all ones (unless the sprite is opaque),
   and a word that ends with them ends the row. */
static void draw_sprite_row(const sprite_drawing *sprite, bool from_ram, uint32_t bank, uint32_t address,
	int x, int x_end, int x_delta, int x_zoom, int end_pen)
{
	const int bits = sprite->bits8 ? 8 : 4;
	const int pens = 32 / bits;
	const int pen_mask = (1 << bits) - 1;
	int x_accumulator = 0;

	while (x != x_end)
	{
		const uint32_t word = sprite_graphics(from_ram, bank, address++);
		int pen = 0;

		for (int index = 0; index < pens; index++)
		{
			const int skipped_pen = (index == 0 || index == pens - 1) ? end_pen : 0;

			pen = (int)(word >> (32 - bits * (index + 1))) & pen_mask;
			while (x_accumulator < 0x10000 && x != x_end)
			{
				put_sprite_pixel(sprite, x, pen, skipped_pen);
				x += x_delta;
				x_accumulator += x_zoom;
			}
			x_accumulator -= 0x10000;
		}
		if (end_pen != 0 && pen == pen_mask)
			break;
	}
}

/* Direct ROM colours, with no shadow or exclusion window. Phase counts source
   pixels in 16.16 from the original, unclipped edge. A terminator still applies
   when clipping or minification skips every pixel of the word that contains it. */
static void direct_sprite_row(uint16_t *dst, uint32_t bank, uint32_t address, int x, int end,
	int dx, uint32_t phase, uint32_t step, int bits, int end_pen, int color)
{
	const int per_word = 32 / bits;
	const int mask = (1 << bits) - 1;
	const uint32_t first_word = (phase >> 16) / per_word;
	const uint32_t bank_base = bank << 20;
	uint32_t index = 0;

	if (end_pen)
		while (index < first_word)
		{
			if ((video.graphics.sprite_word(bank_base | ((address + index) & 0xfffff)) & mask) == (unsigned)end_pen)
				return;
			index++;
		}
	index = first_word;
	while (x != end)
	{
		const uint32_t word = video.graphics.sprite_word(bank_base | ((address + index) & 0xfffff));
		const uint32_t limit = (++index * per_word) << 16;
		while (phase < limit && x != end)
		{
			const int pos = (phase >> 16) & (per_word - 1);
			const int pen = (word >> ((per_word - 1 - pos) * bits)) & mask;
			if (pen && (pen != end_pen || (pos != 0 && pos != per_word - 1)))
				dst[x] = color | pen;
			phase += step;
			x += dx;
		}
		if (end_pen && (word & mask) == (unsigned)end_pen)
			break;
	}
}

/* Four bounded slots for immutable 4-bit ROM artwork. Decode rows on demand;
   pen zero also represents pixels beyond the source end marker. */
enum { ROW_CACHE_SLOTS = 4, ROW_CACHE_BYTES = 8192 };
typedef struct sprite_art
{
	uint32_t address;
	uint16_t width, height;
	uint8_t end_pen;
	uint8_t ready[32];
	uint8_t pens[ROW_CACHE_BYTES - 48];
} sprite_art;
_Static_assert(sizeof(sprite_art) <= ROW_CACHE_BYTES, "sprite row cache budget");
static sprite_art *sprite_art_cache;

void s32_video_set_scenery_filter(bool (*eligible)(uint32_t))
{
	scenery_filter = eligible;
	if (!eligible)
	{
		free(sprite_art_cache);
		sprite_art_cache = NULL;
	}
	else if (!sprite_art_cache && s32_workers_count() == 0)
		sprite_art_cache = calloc(ROW_CACHE_SLOTS, sizeof(*sprite_art_cache));
}

static sprite_art *cached_art(uint32_t address, unsigned width, unsigned height, unsigned end_pen)
{
	if (!sprite_art_cache || width * height > sizeof(sprite_art_cache->pens) || width * height < 2048)
		return NULL;
	/* Four slots share one replacement pool. Recent artwork often hashes to
	   the same slot even when the other three slots have gone cold. */
	static uint32_t serial, last_use[ROW_CACHE_SLOTS];
	const uint32_t now = ++serial;
	unsigned victim = 0;
	for (unsigned i = 0; i < ROW_CACHE_SLOTS; ++i)
	{
		sprite_art *art = &sprite_art_cache[i];
		if (art->address == address && art->width == width && art->height == height && art->end_pen == end_pen)
		{
			last_use[i] = now;
			return art;
		}
		if (now - last_use[i] > now - last_use[victim]) victim = i;
	}
	last_use[victim] = now;
	sprite_art *art = &sprite_art_cache[victim];
	art->address = address;
	art->width = width;
	art->height = height;
	art->end_pen = end_pen;
	memset(art->ready, 0, sizeof(art->ready));
	return art;
}

static const uint8_t *decoded_pens(sprite_art *art, unsigned row, uint32_t base, unsigned address)
{
	uint8_t *pens = art->pens + row * art->width;
	const unsigned bit_mask = 1u << (row & 7);
	if (!(art->ready[row >> 3] & bit_mask))
	{
		unsigned count = art->width / 8;
		const uint32_t *words = video.graphics.sprite_span ?
			video.graphics.sprite_span(base | (address & 0xfffff), count) : NULL;
		for (unsigned i = 0; i < count; ++i)
		{
			uint32_t word = words ? words[i] : video.graphics.sprite_word(base | ((address + i) & 0xfffff));
			uint8_t *p = pens + 8 * i;
			for (int j = 0; j < 8; ++j) p[j] = (word >> (28 - 4 * j)) & 15;
			if (art->end_pen)
			{
				if (p[0] == 15) p[0] = 0;
				if (p[7] == 15)
				{
					memset(p + 7, 0, 1 + 8 * (count - i - 1));
					break;
				}
			}
		}
		art->ready[row >> 3] |= bit_mask;
	}
	return pens;
}

static inline void draw_cached_row(uint16_t *dst, const uint8_t *pens, const uint16_t *columns,
	int x, int dx, int width, int color)
{
	for (int i = 0; i < width; ++i, x += dx)
	{
		unsigned pen = pens[columns[i]];
		if (pen) dst[x] = color | pen;
	}
}

/* Reuse the column mapping down a scaled, clipped ROM sprite. A word
   skipped by minification can still terminate the source row. */
typedef struct sprite_columns
{
	uint16_t word[WIDTH];
	uint8_t shift[WIDTH];
	int count;
} sprite_columns;

static inline void mapped_sprite_row(uint16_t *dst, const sprite_columns *map, uint32_t base,
	uint32_t address, int x, int dx, int bits, int end_pen, int color, bool paired, int full_pairs, uint16_t *second_row)
{
	const uint32_t *words = video.graphics.sprite_span
		? video.graphics.sprite_span(base | (address & 0xfffff), map->word[map->count - 1] + 1u) : NULL;
	const unsigned mask = (1u << bits) - 1;
	int next = end_pen ? 0 : map->word[0];
	for (int i = 0; i < map->count;)
	{
		const int index = map->word[i];
		while (end_pen && next < index)
		{
			const uint32_t skipped = words ? words[next] : video.graphics.sprite_word(base | ((address + next) & 0xfffff));
			next++;
			if ((skipped & mask) == mask) return;
		}
		const uint32_t word = words ? words[index] : video.graphics.sprite_word(base | ((address + index) & 0xfffff));
		next = index + 1;
		do
		{
			const int shift = map->shift[i++];
			const int pen = (word >> shift) & mask;
			if (pen && (pen != end_pen || (shift != 0 && shift != 32 - bits)))
			{
				dst[x] = color | pen;
				if (paired && i <= full_pairs) dst[x + dx] = color | pen;
				if (second_row)
				{
					second_row[x] = color | pen;
					if (i <= full_pairs) second_row[x + dx] = color | pen;
				}
			}
			x += paired ? 2 * dx : dx;
		} while (i < map->count && map->word[i] == index);
		if (end_pen && (word & mask) == mask) return;
	}
}

/* A sprite entry (8 words): flags, source height and width, screen height (and bank bits), screen
   width, y, x, the graphics' offset in the bank, and the color. Hands back how many more entries it
   takes: an indirect table kept in the list takes two. */
static int draw_sprite(uint32_t entry, int x_offset, int y_offset, const area *inside, const area *outside)
{
	static const uint16_t transparent_bits[4][4] = {
		{ 0x7fff, 0x3fff, 0x1fff, 0x0fff },
		{ 0x3fff, 0x1fff, 0x0fff, 0x07ff },
		{ 0x3fff, 0x1fff, 0x0fff, 0x07ff },
		{ 0x1fff, 0x0fff, 0x07ff, 0x03ff }
	};
	const uint8_t *latched = s32_board.sprite_control_latched;
	const uint16_t flags = sprite_word(entry);
	const uint16_t size = sprite_word(entry + 1);
	const uint16_t height_word = sprite_word(entry + 2);
	const uint16_t width_word = sprite_word(entry + 3);
	const bool table_inline = bit(flags, 12);
	const bool from_ram = bit(flags, 10);
	const bool flip_y = bit(flags, 7);
	const bool flip_x = bit(flags, 6);
	const int source_height = size >> 8;
	const int height = height_word & 0x3ff;
	const int width = width_word & 0x3ff;
	/* the bank in bits 13 and 15 of the fourth word; bit 11 picks the screen */
	const uint32_t bank = ((width_word & 0x2000) >> 13 | (width_word & 0x8000) >> 14) % (video.graphics.sprite_banks ? video.graphics.sprite_banks : 1);
	const int screen = bit(width_word, 11);
	const int taken = (bit(flags, 13) && table_inline) ? 2 : 0;

	if (!bit((uint32_t)video.screens_shown, screen))
		return taken;    /* (a screen the port does not show: s32_video_show_screens) */
	sprite_drawing sprite;
	int source_width;
	int end_pen;
	int x;
	int y;
	int x_delta = 1;
	int y_delta = 1;
	int x_end;
	int y_end;
	int x_zoom;
	int y_zoom;
	int y_accumulator = 0;
	uint32_t address = sprite_word(entry + 6) | (uint32_t)(height_word & 0xf000) << 4;

	sprite.inside = inside;
	sprite.outside = outside;
	sprite.extra = (uint8_t)(!video.extra_used ? 0
		: ((size & S32_SPRITE_EXTRA_PALETTE) ? EXTRA_COLOURS : 0) | ((sprite_word(entry + 7) & S32_SPRITE_GHOST) ? EXTRA_GHOST : 0));
	sprite.extra_row = NULL;
	sprite.indirect = bit(flags, 13);
	sprite.shadow = bit(flags, 11) && bit(latched[5], 0);
	sprite.bits8 = bit(flags, 9);
	sprite.color = 0x8000 | (sprite_word(entry + 7) & (sprite.bits8 ? 0x7f00 : 0x7ff0));
	source_width = sprite.bits8 ? (size & 0x3f) : ((size >> 1) & 0x3f);
	end_pen = bit(flags, 8) ? 0 : (sprite.bits8 ? 0xff : 0x0f);
	if (source_width == 0 || source_height == 0 || width == 0 || height == 0)
		return taken;

	sprite.transparent_bits = transparent_bits[latched[4] & 3][latched[5] & 3];
	if (sprite.bits8)
		sprite.transparent_bits &= 0xfff0;
	if (sprite.indirect)
	{
		/* the table follows the entry, or is an entry of the list picked by the color word */
		const uint32_t table = table_inline ? entry + 8 : 8 * (uint32_t)(sprite_word(entry + 7) & 0x1fff);

		for (int index = 0; index < 16; index++)
			sprite.table[index] = (uint16_t)((sprite_word(table + (uint32_t)index) & (sprite.bits8 ? 0xfff0 : 0xffff)) |
				(bit(latched[5], 0) ? 0x8000 : 0));
	}

	if (s32_video_sprite_observer != NULL && !from_ram)
	{
		/* the pixel each pen puts in the buffer, as put_sprite_pixel works it out */
		uint16_t pixels[256];
		const int pens = sprite.bits8 ? 256 : 16;

		for (int pen = 0; pen < pens; pen++)
			pixels[pen] = (uint16_t)(!sprite.indirect ? (sprite.color | pen)
				: (sprite.bits8 ? sprite.table[pen >> 4] | (pen & 0x0f) : sprite.table[pen]));
		s32_video_sprite_observer(screen, bank << 20 | (address & 0xfffff), source_width, source_height, sprite.bits8,
			end_pen == 0, pixels);
	}

	x_zoom = (((sprite.bits8 ? 4 : 8) * source_width) << 16) / width;
	y_zoom = (source_height << 16) / height;

	/* place it: the list's offset, then its alignment (0 or 3 centered, 1 by the far edge, 2 by the near) */
	x = sign_extend(sprite_word(entry + 5), 12) + (bit(flags, 4) ? x_offset : 0) + video.sprite_side;
	y = sign_extend(sprite_word(entry + 4), 12) + (bit(flags, 5) ? y_offset : 0);
	switch (flags & 3)
	{
		case 0:
		case 3: x -= (width - 1) / 2; break;
		case 1: x -= width - 1; break;
		default: break;
	}
	switch ((flags >> 2) & 3)
	{
		case 0:
		case 3: y -= (height - 1) / 2; break;
		case 1: y -= height - 1; break;
		default: break;
	}
	if (flip_x)
	{
		x += width - 1;
		x_delta = -1;
	}
	if (flip_y)
	{
		y += height - 1;
		y_delta = -1;
	}
	x_end = x + x_delta * width;
	y_end = y + y_delta * height;

	/* rows end at the sprite's width, or at the clip. (MAME tests a flipped sprite's end against the clip's
	   right edge instead of its left one, so the sprite runs on to the left edge reading whatever follows
	   it in the ROM: an opaque one, which ignores end codes, leaves copies of the next frames there. The
	   reference, tools/core/system32/reference/mame/sega/segas32_v.cpp, has the same line fixed.) */
	if (inside->left > inside->right)
		return taken;
	if (x_delta > 0 && x_end > inside->right)
	{
		x_end = inside->right + 1;
		if (x >= x_end)
			return taken;
	}
	if (x_delta < 0 && x_end < inside->left)
	{
		x_end = inside->left - 1;
		if (x <= x_end)
			return taken;
	}

	/* Clip the vertical interval once, advancing the same fixed-point source phase. */
	const int top = flip_y ? y_end + 1 : y;
	const int bottom = flip_y ? y : y_end - 1;
	if (bottom < inside->top || top > inside->bottom || inside->top > inside->bottom)
		return taken;
	const int first_row = flip_y ? (y < inside->bottom ? y : inside->bottom)
		: (y > inside->top ? y : inside->top);
	const int origin_y = y;
	const uint32_t phase = (uint32_t)((first_row - y) * y_delta) * (uint32_t)y_zoom;
	const uint32_t frame_address = (bank << 20) | (address & 0xfffff);
	address += source_width * (phase >> 16);
	y_accumulator = phase & 0xffff;
	y = first_row;
	y_end = flip_y ? (top > inside->top ? top : inside->top) - 1
		: (bottom < inside->bottom ? bottom : inside->bottom) + 1;

	const int left = flip_x ? x_end + 1 : (x < inside->left ? inside->left : x);
	const int right = flip_x ? (x > inside->right ? inside->right : x) : x_end - 1;
	if (left > right)
		return taken;
	const bool excluded = outside->left <= outside->right && outside->top <= outside->bottom
		&& left <= outside->right && right >= outside->left && top <= outside->bottom && bottom >= outside->top;
	if (!from_ram && !sprite.indirect && !sprite.shadow && !video.extra_used && !excluded)
	{
		uint16_t cache[WIDTH];
		const int start = flip_x ? right : left;
		const uint32_t x_phase = (uint32_t)((start - x) * x_delta) * (uint32_t)x_zoom;
		const int bits = sprite.bits8 ? 8 : 4;
		sprite_columns map;
		const bool reuse = packed_output && abs(y_end - y) >= 4;
		const int visible_width = right - left + 1;
		const bool paired = reuse && !half_scenery && visible_width >= 48 && abs(y_end - y) >= 32
			&& scenery_filter && scenery_filter(frame_address);
		sprite_art *art = reuse && !paired && bits == 4 && s32_workers_count() == 0
			? cached_art(frame_address, source_width * 8, source_height, end_pen) : NULL;
		unsigned source_y = phase >> 16;
		if (reuse)
		{
			uint32_t phase_x = x_phase;
			map.count = paired ? (visible_width + 1) / 2 : visible_width;
			const unsigned step = paired ? 2u * x_zoom : (unsigned)x_zoom;
			for (int i = 0; i < map.count; i++, phase_x += step)
			{
				const unsigned col = phase_x >> 16;
				if (art) map.word[i] = col;
				else
				{
					map.word[i] = col / (32 / bits);
					map.shift[i] = 32 - bits - (col * bits % 32);
				}
			}
		}
		/* Sample alternate destination rows only for known foliage, rocks and
		   plants. Keep each pair anchored to the sprite when it is clipped. */
		if (paired && pair_scenery_rows)
		{
			for (int row = y; row != y_end;)
			{
				const bool twice = ((row - origin_y) % 2 == 0) && row + y_delta != y_end;
				uint16_t (*rows)[WIDTH] = video.sprites[screen][video.shown_sprites ^ 1];
				mapped_sprite_row(rows[row], &map, bank << 20, address, start, x_delta, bits,
					end_pen, sprite.color, true, visible_width / 2, twice ? rows[row + y_delta] : NULL);
				const int lines = twice ? 2 : 1;
				const int next = y_accumulator + lines * y_zoom;
				address += source_width * (next >> 16);
				y_accumulator = next & 0xffff;
				row += lines * y_delta;
			}
			return taken;
		}
		uint16_t runs[WIDTH + 1];
		int run_count = 0;
		bool cached = false;
		for (int row = y; row != y_end; row += y_delta)
		{
			uint16_t *dst = video.sprites[screen][video.shown_sprites ^ 1][row];
			const int next = y_accumulator + y_zoom;
			const bool repeat = next < 0x10000 && row + y_delta != y_end;
			if (!cached)
			{
				if (repeat)
					memset(&cache[left], 0, (right - left + 1) * sizeof(*cache));
				if (art)
					draw_cached_row(repeat ? cache : dst, decoded_pens(art, source_y, bank << 20, address),
						map.word, start, x_delta, visible_width, sprite.color);
				else if (paired)
					mapped_sprite_row(repeat ? cache : dst, &map, bank << 20, address, start,
						x_delta, bits, end_pen, sprite.color, true, visible_width / 2, NULL);
				else if (reuse)
					mapped_sprite_row(repeat ? cache : dst, &map, bank << 20, address, start,
						x_delta, bits, end_pen, sprite.color, false, 0, NULL);
				else direct_sprite_row(repeat ? cache : dst, bank, address, start, x_end, x_delta,
					x_phase, x_zoom, sprite.bits8 ? 8 : 4, end_pen, sprite.color);
			}
			if (repeat && !cached && reuse)
			{
				run_count = 0;
				for (int col = left; col <= right;)
				{
					while (col <= right && !cache[col]) col++;
					if (col > right) break;
					runs[run_count++] = col;
					while (col <= right && cache[col]) col++;
					runs[run_count++] = col;
				}
			}
			if ((cached || repeat) && reuse)
			{
				for (int i = 0; i < run_count; i += 2)
					memcpy(dst + runs[i], cache + runs[i], (runs[i+1] - runs[i]) * sizeof(*dst));
			}
			else if (cached || repeat)
				for (int col = left; col <= right; col++)
					if (cache[col])
						dst[col] = cache[col];
			cached = repeat;
			source_y += next >> 16;
			address += source_width * (next >> 16);
			y_accumulator = next & 0xffff;
		}
		return taken;
	}

	for (int row = y; row != y_end; row += y_delta)
	{
		if (row >= inside->top && row <= inside->bottom)
		{
			sprite.row = video.sprites[screen][video.shown_sprites ^ 1][row];
			if (video.extra_used)
				sprite.extra_row = video.extra_pixels[screen][video.shown_sprites ^ 1][row];
			sprite.outside_row = row >= outside->top && row <= outside->bottom;
			draw_sprite_row(&sprite, from_ram, bank, address, x, x_end, x_delta, x_zoom, end_pen);
		}
		/* the zoom steps through the source rows */
		y_accumulator += y_zoom;
		address += (uint32_t)(source_width * (y_accumulator >> 16));
		y_accumulator &= 0xffff;
	}
	return taken;
}

/* The list in sprite RAM from its entry 'number', 16 bytes an entry, its command in the top two bits of the
   first word: draw a sprite, set the clips (bit 12 the one to draw in, bit 13 the one to leave alone), jump
   (and with bit 13, set the offset sprites can add), or end. The board's starts at 0. Only its rows from
   first_row to last_row are drawn: each thread walks the whole list for a band of them (draw_sprites) */
static void draw_sprite_list(int number, int first_row, int last_row)
{
	/* (WIDE: the board's screen of 320 in the middle of a wider one, the sprites placed that much to the right) */
	const bool board_wide = bit(s32_board.sprite_control_latched[6], 0);
	const bool wide = video.wide_side != 0 && !board_wide;
	const int side = video.sprite_side;
	const area screen = { 0, board_wide ? 415 : 319 + 2 * side, first_row, last_row };
	area inside = screen;
	area outside = { 0, -1, 0, -1 };
	int x_offset = 0;
	int y_offset = 0;

	for (int entries = 0; entries < 0x20000 / 16; entries++)
	{
		const uint32_t entry = 8 * (uint32_t)(number & 0x1fff);
		const uint16_t command = sprite_word(entry);

		switch (command >> 14)
		{
			case 0:
				number += 1 + draw_sprite(entry, x_offset, y_offset, &inside, &outside);
				break;
			case 1:
				if (bit(command, 12))
				{
					inside.left = sign_extend(sprite_word(entry + 2), 12);
					inside.right = sign_extend(sprite_word(entry + 3), 12);
					inside.top = sign_extend(command, 12);
					inside.bottom = sign_extend(sprite_word(entry + 1), 12);
					if (wide && inside.left <= 0 && inside.right >= 319)
					{
						inside.left = 0;
						inside.right = screen.right;    /* (the whole screen: the wide one) */
					}
					else
					{
						inside.left += side;
						inside.right += side;
					}
					clip_area(&inside, &screen);
				}
				if (bit(command, 13))
				{
					outside.left = sign_extend(sprite_word(entry + 6), 12) + side;
					outside.right = sign_extend(sprite_word(entry + 7), 12) + side;
					outside.top = sign_extend(sprite_word(entry + 4), 12);
					outside.bottom = sign_extend(sprite_word(entry + 5), 12);
				}
				number++;
				break;
			case 2:
				if (bit(command, 13))
				{
					y_offset = sign_extend(sprite_word(entry + 1), 12);
					x_offset = sign_extend(sprite_word(entry + 2), 12);
				}
				number = command & 0x1fff;
				break;
			default:
				return;
		}
	}
}

static void draw_sprite_list_rows(void *context, int first_row, int last_row)
{
	draw_sprite_list(*(const int *)context, first_row, last_row);
}

/* The list from its entry 'number', a band of its rows on each of the workers' threads; on this one alone
   while an observer watches the sprites (it sees them in the list's order) */
static void draw_sprites(int number)
{
	const bool wide = video.wide_side != 0 && !bit(s32_board.sprite_control_latched[6], 0);

	video.sprite_side = wide ? video.wide_side : 0;
	if (s32_video_sprite_observer != NULL)
		draw_sprite_list(number, 0, HEIGHT - 1);
	else
		s32_workers_rows(draw_sprite_list_rows, &number, 0, HEIGHT - 1);
}

static void erase_sprite_bank(int screen, int bank, bool marks)
{
	memset(video.sprites[screen][bank], 0xff, sizeof(video.sprites[screen][bank]));
	if (marks) memset(video.extra_pixels[screen][bank], 0, sizeof(video.extra_pixels[screen][bank]));
}

/* Overlay snapshots and a newly visible screen must see all preceding erases. */
static void apply_pending_erase(int screen, int bank)
{
	if (video.erase_pending[screen][bank])
	{
		erase_sprite_bank(screen, bank, video.marks_pending[screen][bank]);
		video.erase_pending[screen][bank] = false;
		video.marks_pending[screen][bank] = false;
	}
}

void s32_video_sprites(bool erase, bool draw)
{
	if (erase)
		for (int screen = 0; screen < S32_SCREENS; screen++)
		{
			const int bank = video.shown_sprites;
			if (bit((unsigned)video.screens_shown, screen))
				erase_sprite_bank(screen, bank, video.extra_used);
			else
			{
				video.erase_pending[screen][bank] = true;
				video.marks_pending[screen][bank] |= video.extra_used;
			}
		}
	if (draw)
	{
		video.shown_sprites ^= 1;
		draw_sprites(0);
	}
}

/* ---- a port's sprites over the frame ---- */

#define OVERLAY_ENTRY 0x1F00    /* where the overlay's list goes in sprite RAM, for the time it is drawn */

static uint16_t overlay_saved[S32_SCREENS][HEIGHT][WIDTH];
static bool overlay_on;

void s32_video_overlay_sprites(const uint8_t *list, size_t size)
{
	static uint8_t sprite_ram_saved[S32_SPRITE_RAM_SIZE - OVERLAY_ENTRY * 16];
	uint8_t *place = &s32_board.sprite_ram[OVERLAY_ENTRY * 16];

	if (size > sizeof(sprite_ram_saved))
		size = sizeof(sprite_ram_saved);
	if (!overlay_on)
		for (int screen = 0; screen < S32_SCREENS; screen++)
		{
			apply_pending_erase(screen, video.shown_sprites);
			memcpy(overlay_saved[screen], video.sprites[screen][video.shown_sprites], sizeof(overlay_saved[screen]));
		}
	overlay_on = true;
	memcpy(sprite_ram_saved, place, size);
	memcpy(place, list, size);
	video.shown_sprites ^= 1;    /* (the list draws into the buffer not shown: this one) */
	draw_sprites(OVERLAY_ENTRY);
	video.shown_sprites ^= 1;
	memcpy(place, sprite_ram_saved, size);
}

void s32_video_overlay_end(void)
{
	if (!overlay_on)
		return;
	overlay_on = false;
	for (int screen = 0; screen < S32_SCREENS; screen++)
	{
		apply_pending_erase(screen, video.shown_sprites);
		memcpy(video.sprites[screen][video.shown_sprites], overlay_saved[screen], sizeof(overlay_saved[screen]));
	}
}

/* ---- the mixer ---- */

/* Which color offset a layer takes (0x3E bit 15 and the layer's bit there pick how its own flag counts) */
static uint8_t color_offsets(int screen, bool layer_bit, bool layer_flag)
{
	switch ((bit(mixer_register(screen, 0x3e), 15) ? 2 : 0) | (layer_bit ? 1 : 0))
	{
		case 1: return 2;
		case 2: return layer_flag ? 0 : 2;
		default: return layer_flag ? 0 : 1;
	}
}

/* The sprite groups a layer blends with: the group equal to a value, at most it, at least it, or all */
static uint16_t sprite_blend(int encoding)
{
	const int value = encoding & 0x0f;

	switch ((encoding >> 4) & 3)
	{
		case 0: return (uint16_t)(1 << value);
		case 1: return (uint16_t)((1 << value) | ((1 << value) - 1));
		case 2: return (uint16_t)(~((1 << value) - 1) & 0xffff);
		default: return 0xffff;
	}
}

/* The first layer down the list that is not transparent there; the backdrop always is. A sprite pixel
   all ones lets the layers below show, and the shadow pen darkens them. */
static const mix_layer *top_layer(const mix_layer *entry, const uint16_t *const *rows, int x, int sprite_pixel,
	int shadow_mask, int sprite_bits, int shadow_pen, int *pixel, int *shadow)
{
	for (;; entry++)
	{
		if (entry->index != LAYER_SPRITES)
		{
			*pixel = rows[entry->index][x] & 0x1fff;
			if (*pixel != 0 || entry->index == LAYER_BACKDROP)
				return entry;
			continue;
		}
		*pixel = sprite_pixel;
		*shadow = ~sprite_pixel & shadow_mask;
		if ((sprite_pixel & 0x7fff) != 0x7fff)
		{
			*pixel = sprite_pixel & sprite_bits;
			if ((*pixel & 0x7ffe) != shadow_pen)
				return entry;
			*shadow = 1;
		}
	}
}

static uint16_t palette_color(int screen, const mix_layer *entry, int pixel)
{
	return s32_board.palette[screen][(entry->palette_base + ((pixel >> entry->shift) & 0xfff0) + (pixel & 0x0f)) & 0x3fff];
}

uint16_t s32_video_tile_pen_color(int screen, int tilemap, uint16_t color, int pen)
{
	const uint16_t control = mixer_register(screen, 0x20 + 2 * (uint32_t)(LAYER_NBG0 + tilemap));
	mix_layer layer;

	layer.palette_base = (uint16_t)((control & 0x00f0) << 6);
	layer.shift = (control >> 8) & 3;
	return palette_color(screen, &layer, ((color & 0x1ff) << 4 | (pen & 0x0f)) & 0x1fff);
}

uint16_t s32_video_sprite_pixel_color(int screen, uint16_t pixel)
{
	const uint16_t sprite_control = mixer_register(screen, 0x4c);
	const sprite_grouping grouping = groupings[sprite_control & 0x0f];
	const int group = (pixel >> grouping.shift) & grouping.mask;
	const uint16_t control = mixer_register(screen, 2 * (uint32_t)(grouping.base | group));
	mix_layer sprites;

	sprites.palette_base = (uint16_t)((((sprite_control & 3) != 3) ? control : sprite_control) & 0x00f0) << 6;
	sprites.shift = (control >> 8) & 3;
	return palette_color(screen, &sprites, pixel & (((1 << grouping.shift) - 1) & 0x3fff));
}

/* What a screen's mixing reads, the same on every row (its rows on the workers' bands) */
typedef struct mixing
{
	int screen;
	int blend_factor;
	sprite_grouping grouping;
	int shadow_mask;
	int sprite_bits;
	int shadow_pen;
	uint8_t sprite_flips;
	int offsets[3][3];
	uint16_t faded[2][3][32];
	mix_layer orders[16][8];
	mix_layer background[7];
	mix_layer sprites[16];
} mixing;

static void mix_rows(void *context, int first_row, int last_row);

/* A screen's frame from the layers and its sprites: registers 0x00-0x1E give each sprite group its priority and palette, 0x20-0x2C
   each layer, 0x30-0x3A what each blends with, 0x40-0x4A two color offsets, 0x4C the sprite grouping and
   shadows, 0x4E blending */
static mixing *prepare_mix(int screen, int drawn)
{
	const uint16_t blend_control = mixer_register(screen, 0x4e);
	const bool blending = bit(blend_control, 11);
	const int blend_factor = (blend_control >> 8) & 7;
	const uint16_t sprite_control = mixer_register(screen, 0x4c);
	const sprite_grouping grouping = groupings[sprite_control & 0x0f];
	const int shadow_mask = bit(sprite_control, 2) ? 0x8000 : 0;
	const int sprite_bits = ((1 << grouping.shift) - 1) & 0x3fff;
	const int shadow_pen = 0x7ffe & sprite_bits;
	const uint8_t sprite_flips = s32_board.sprite_control_latched[2];
	static mixing setup;
	int (*const offsets)[3] = setup.offsets;
	mix_layer sorted[8];
	mix_layer (*const orders)[8] = setup.orders;
	int count = 0;

	for (int channel = 0; channel < 3; channel++)
	{
		offsets[0][channel] = sign_extend(mixer_register(screen, 0x40 + 2 * (uint32_t)channel), 6);
		offsets[1][channel] = sign_extend(mixer_register(screen, 0x46 + 2 * (uint32_t)channel), 6);
		offsets[2][channel] = 0;
	}

	/* RGB332 and all four 565 layouts pack independent colour channels. */
	if (packed_output)
	{
		for (int set = 0; set < 2; ++set)
		{
			if (!(offsets[set][0] | offsets[set][1] | offsets[set][2])) continue;
			for (int channel = 0; channel < 3; ++channel)
				for (int c = 0; c < 32; ++c)
				{
					const int sum = c + offsets[set][channel];
					const int level = sum < 0 ? 0 : sum > 31 ? 31 : sum;
					setup.faded[set][channel][c] = packed_colors[level << (5 * channel)];
				}
		}
	}

	/* the layers that are on and have a priority, then the backdrop, from the top down */
	for (int index = LAYER_TEXT; index <= LAYER_BITMAP; index++)
	{
		const uint16_t control = mixer_register(screen, 0x20 + 2 * (uint32_t)index);
		const uint16_t blend = mixer_register(screen, 0x30 + 2 * (uint32_t)index);

		if (!bit((uint32_t)drawn, index) || (control & 0x0f) == 0)
			continue;
		sorted[count].index = (uint8_t)index;
		sorted[count].priority = (uint8_t)((control & 0x0f) << 3 | (6 - index));
		sorted[count].palette_base = (uint16_t)((control & 0x00f0) << 6);
		sorted[count].shift = (control >> 8) & 3;
		sorted[count].blend = blending ? (uint8_t)((blend >> 6) & 0xff) : 0;
		sorted[count].sprite_blend = sprite_blend(blend & 0x3f);
		sorted[count].offsets = color_offsets(screen, bit(mixer_register(screen, 0x3e), index), bit(blend, 14));
		count++;
	}
	sorted[count].index = LAYER_BACKDROP;
	sorted[count].priority = 1 << 3;
	sorted[count].palette_base = (uint16_t)((mixer_register(screen, 0x2c) & 0x00f0) << 6);
	sorted[count].shift = (mixer_register(screen, 0x2c) >> 8) & 3;
	sorted[count].blend = 0;
	sorted[count].sprite_blend = 0;
	sorted[count].offsets = color_offsets(screen, bit(mixer_register(screen, 0x3e), 8), bit(mixer_register(screen, 0x3e), 14));
	count++;
	for (int index = 0; index < count; index++)
		for (int other = index + 1; other < count; other++)
			if (sorted[other].priority > sorted[index].priority)
			{
				const mix_layer swap = sorted[other];

				sorted[other] = sorted[index];
				sorted[index] = swap;
			}

	for (int i = 0; i < count; i++)
	{
		const int *offset = offsets[sorted[i].offsets];
		sorted[i].plain = !sorted[i].blend && !(offset[0] | offset[1] | offset[2]);
	}

	/* each sprite group goes in above the first layer of a lower priority */
	for (int group = 0; group <= grouping.mask; group++)
	{
		const uint16_t control = mixer_register(screen, 2 * (uint32_t)(grouping.base | group));
		const int priority = (control & 0x0f) << 3 | 7;
		int place = count;
		int copied = 0;
		mix_layer *sprites;

		for (int index = 0; index < count; index++)
		{
			if (priority > sorted[index].priority && place == count)
				place = copied++;
			orders[group][copied++] = sorted[index];
		}
		sprites = &orders[group][place];
		sprites->index = LAYER_SPRITES;
		sprites->priority = (uint8_t)priority;
		sprites->palette_base = (uint16_t)((((sprite_control & 3) != 3) ? control : sprite_control) & 0x00f0) << 6;
		sprites->shift = (control >> 8) & 3;
		sprites->blend = 0;
		sprites->sprite_blend = 0;
		sprites->offsets = color_offsets(screen, bit(mixer_register(screen, 0x3e), 6), bit(sprite_control, 15));
		const int *offset = offsets[sprites->offsets];
		sprites->plain = !(offset[0] | offset[1] | offset[2]);
		setup.sprites[group] = *sprites;
	}

	memcpy(setup.background, sorted, count * sizeof(*sorted));
	setup.screen = screen;
	setup.blend_factor = blend_factor;
	setup.grouping = grouping;
	setup.shadow_mask = shadow_mask;
	setup.sprite_bits = sprite_bits;
	setup.shadow_pen = shadow_pen;
	setup.sprite_flips = sprite_flips;
	return &setup;
}

/* Resolve ordinary opaque spans directly to the display format. Empty tile
   rows disappear from the order once per scanline. Special pixels fall back
   to the full mixer with its exact lower-layer and shadow rules. */
static const mix_layer *opaque_winner(const mixing *setup, const mix_layer **layers,
	const uint16_t *const *rows, int x, int group, int sprite, int *pen)
{
	const mix_layer *sp = &setup->sprites[group];
	for (;; layers++)
	{
		const mix_layer *bg = *layers;
		if (sp->priority > bg->priority && (sprite & 0x7fff) != 0x7fff)
		{
			*pen = sprite & setup->sprite_bits;
			if (((~sprite) & setup->shadow_mask) || (*pen & 0x7ffe) == setup->shadow_pen)
				return NULL;
			return sp;
		}
		*pen = rows[bg->index][x] & 0x1fff;
		if (*pen || bg->index == LAYER_BACKDROP)
		{
			if (bg->blend || (sp->priority > bg->priority && ((~sprite) & setup->shadow_mask)))
				return NULL;
			return bg;
		}
	}
}

static void mix_rows(void *context, int first_row, int last_row)
{
	const mixing *setup = (const mixing *)context;
	const int screen = setup->screen;
	const int blend_factor = setup->blend_factor;
	const sprite_grouping grouping = setup->grouping;
	const int shadow_mask = setup->shadow_mask;
	const int sprite_bits = setup->sprite_bits;
	const int shadow_pen = setup->shadow_pen;
	const uint8_t sprite_flips = setup->sprite_flips;
	const int (*const offsets)[3] = setup->offsets;
	const mix_layer (*const orders)[8] = setup->orders;

	/* the sprite buffer is scanned flipped as the sprite controls say */
	for (int y = first_row; y <= last_row; y++)
	{
		const int sprite_y = bit(sprite_flips, 1) ? video.visible.bottom - (y - video.visible.top) : y;
		const uint16_t *rows[LAYERS];
		int sprite_x = bit(sprite_flips, 0) ? video.visible.right : video.visible.left;
		const int sprite_dx = bit(sprite_flips, 0) ? -1 : 1;

		for (int index = LAYER_TEXT; index <= LAYER_BITMAP; index++)
			{
				const int line = half_scenery && index >= LAYER_NBG0 && index <= LAYER_NBG3 ? y & ~1 : y;
				rows[index] = video.layers[index].empty[line] ? transparent_row : video.layers[index].pixels[line];
			}
		rows[LAYER_SPRITES] = video.sprites[screen][video.shown_sprites][sprite_y];
		rows[LAYER_BACKDROP] = video.layers[LAYER_BACKDROP].pixels[y];

		const bool packed = packed_output && screen == 0 && video.visible.right == 319;
		uint8_t *out8 = packed ? (uint8_t *)packed_output + y * 320 : NULL;
		uint16_t *out16 = packed && packed_565 ? (uint16_t *)packed_output + y * 320 : NULL;
		const mix_layer *visible[7];
		if (packed)
		{
			int n = 0;
			for (const mix_layer *bg = setup->background; ; bg++)
			{
				if (rows[bg->index] != transparent_row) visible[n++] = bg;
				if (bg->index == LAYER_BACKDROP) break;
			}
		}
		for (int x = video.visible.left; x <= video.visible.right; x++, sprite_x += sprite_dx)
		{
			const int sprite_pixel = rows[LAYER_SPRITES][sprite_x];
			const int group = (sprite_pixel >> grouping.shift) & grouping.mask;
			int shadow = 0;
			int pixel;
			if (packed && !video.extra_used)
			{
				const mix_layer *winner = opaque_winner(setup, visible, rows, x, group, sprite_pixel, &pixel);
				if (winner)
				{
					const unsigned color = palette_color(screen, winner, pixel) & 0x7fff;
					unsigned pen;
					if (winner->plain) pen = packed_colors[color];
					else
					{
						const uint16_t (*fade)[32] = setup->faded[winner->offsets];
						pen = fade[0][color & 31] | fade[1][(color >> 5) & 31] | fade[2][color >> 10];
					}
					if (out16) out16[x] = pen;
					else out8[x] = (uint8_t)pen;
					continue;
				}
			}
			const mix_layer *first = top_layer(orders[group], rows, x, sprite_pixel, shadow_mask, sprite_bits,
				shadow_pen, &pixel, &shadow);
			uint16_t color = palette_color(screen, first, pixel);

			const uint8_t marks = video.extra_used && first->index == LAYER_SPRITES
				? video.extra_pixels[screen][video.shown_sprites][sprite_y][sprite_x] : 0;

			if (first->plain && !shadow && !marks)
			{
				if (packed)
				{
					if (out16) out16[x] = packed_colors[color & 0x7fff];
					else out8[x] = (uint8_t)packed_colors[color & 0x7fff];
					continue;
				}
				video.frame[screen][y][x] = ((uint32_t)(color & 31) << 19)
					| ((uint32_t)(color & 0x03e0) << 6) | ((color & 0x7c00) >> 7);
				continue;
			}
			if (marks & EXTRA_COLOURS)
			{
				const int index = (first->palette_base + ((pixel >> first->shift) & 0xfff0) + (pixel & 0x0f)) & 0x3fff;

				if (video.extra_set[screen][index])
					color = video.extra_palette[screen][index];
			}
			int red = (color & 0x1f) + offsets[first->offsets][0];
			int green = ((color >> 5) & 0x1f) + offsets[first->offsets][1];
			int blue = ((color >> 10) & 0x1f) + offsets[first->offsets][2];

			if (first->blend != 0)
			{
				int below;
				const mix_layer *second = top_layer(first + 1, rows, x, sprite_pixel, shadow_mask, sprite_bits,
					shadow_pen, &below, &shadow);

				if (bit(first->blend, second->index) && (second->index != LAYER_SPRITES || bit(first->sprite_blend, group)))
				{
					color = palette_color(screen, second, below);
					red = shift_down(red * (7 - blend_factor) + ((color & 0x1f) + offsets[second->offsets][0]) * (blend_factor + 1), 3);
					green = shift_down(green * (7 - blend_factor) + (((color >> 5) & 0x1f) + offsets[second->offsets][1]) * (blend_factor + 1), 3);
					blue = shift_down(blue * (7 - blend_factor) + (((color >> 10) & 0x1f) + offsets[second->offsets][2]) * (blend_factor + 1), 3);
				}
			}
			if (marks & EXTRA_GHOST)
			{
				/* a ghost: half its colour, half the colour of the layer under it */
				int below;
				int ignored_shadow = 0;
				const mix_layer *second = top_layer(first + 1, rows, x, sprite_pixel, shadow_mask, sprite_bits,
					shadow_pen, &below, &ignored_shadow);
				const uint16_t under = palette_color(screen, second, below);

				red = (red + (under & 0x1f) + offsets[second->offsets][0]) / 2;
				green = (green + ((under >> 5) & 0x1f) + offsets[second->offsets][1]) / 2;
				blue = (blue + ((under >> 10) & 0x1f) + offsets[second->offsets][2]) / 2;
			}
			if (shadow)
			{
				red = shift_down(red, 1);
				green = shift_down(green, 1);
				blue = shift_down(blue, 1);
			}
			red = red > 31 ? 31 : (red > 0 ? red : 0);
			green = green > 31 ? 31 : (green > 0 ? green : 0);
			blue = blue > 31 ? 31 : (blue > 0 ? blue : 0);
			if (packed)
			{
				const unsigned pen = packed_colors[red | green << 5 | blue << 10];
				if (out16) out16[x] = pen;
				else out8[x] = (uint8_t)pen;
				continue;
			}
			video.frame[screen][y][x] = (uint32_t)red << 19 | (uint32_t)green << 11 | (uint32_t)blue << 3;
		}
	}
}

/* Adjacent samples in the same tile can share its lookup. */
static inline void sky_pens(const zoomed_row *row, int x, uint16_t *left, uint16_t *right)
{
	const uint32_t a = row->x_start + row->x_step * (uint32_t)(x - video.visible.left - video.side);
	const uint32_t b = a + row->x_step;
	if ((a >> 24) != (b >> 24))
	{
		*left = zoomed_pixel(row, x);
		*right = zoomed_pixel(row, x + 1);
		return;
	}
	const unsigned page = row->pages[(a >> 29) & 1];
	const uint16_t cell = video_word((page << 9) | ((unsigned)(row->line >> 4) << 5) | ((a >> 24) & 31));
	const unsigned tile = tile_index(((unsigned)row->bank << 13) | (cell & 0x1fff));
	const unsigned kind = (video.tile_opacity[tile >> 2] >> ((tile & 3) * 2)) & 3;
	if (kind == TILE_TRANSPARENT)
	{
		*left = *right = 0;
		return;
	}
	const unsigned line = (row->line & 15) ^ ((cell & 0x8000) ? 15 : 0);
	const unsigned flip = (cell & 0x4000) ? 15 : 0;
	const uint8_t *pens = video.graphics.tile_pens[tile] + line * 16;
	const unsigned first = pens[((a >> 20) & 15) ^ flip];
	*left = first ? (cell & 0x1ff0) | first : 0;
	if ((a >> 20) == (b >> 20))
		*right = *left;
	else
	{
		const unsigned second = pens[((b >> 20) & 15) ^ flip];
		*right = second ? (cell & 0x1ff0) | second : 0;
	}
}

/* A repeated, opaque sky tile can give us one colour per scanline. Keep the
   classification local to the frame so palette fades and page changes apply
   immediately. A detailed tilemap or a transparent tile ends the sky band. */
typedef struct sky_shade
{
	uint16_t normal, shadow;
} sky_shade;

typedef struct sky_band
{
	int map_row, cell;
	sky_shade shades[16];
} sky_band;

/* RGB sums cover sixteen source pixels. Retain the half-step available in
   RGB565 green; use the same channel ordering as the regular output palette. */
static uint16_t pack_sky(unsigned red, unsigned green, unsigned blue, bool shadow)
{
	const unsigned r = (red + 8) >> 4, b = (blue + 8) >> 4;
	const unsigned green5 = (green + 8) >> 4;
	const unsigned color = r | (green5 << 5) | (b << 10);
	if (shadow) return packed_colors[(color & 0x7bde) >> 1];
	if (!packed_565) return packed_colors[color];
	const unsigned g = (green + 4) >> 3;
	const bool bgr = packed_format == S32_BGR565 || packed_format == S32_BGR565_SWAP;
	const uint16_t rgb = bgr ? (uint16_t)(b << 11 | g << 5 | r) : (uint16_t)(r << 11 | g << 5 | b);
	return (packed_format == S32_RGB565_SWAP || packed_format == S32_BGR565_SWAP)
		? (uint16_t)(rgb >> 8 | rgb << 8) : rgb;
}

static bool make_sky_shades(sky_band *band, unsigned tile, int cell, const mix_layer *sky)
{
	const unsigned kind = (video.tile_opacity[tile >> 2] >> ((tile & 3) * 2)) & 3;
	if (kind != TILE_OPAQUE) return false;
	unsigned colors[16];
	for (int pen = 1; pen < 16; pen++)
		colors[pen] = palette_color(0, sky, (cell & 0x1ff0) | pen) & 0x7fff;
	const uint8_t *pens = video.graphics.tile_pens[tile];
	for (int y = 0; y < 16; y++)
	{
		const unsigned anchor = colors[pens[16 * y]];
		unsigned r = 0, g = 0, b = 0;
		for (int x = 0; x < 16; x++)
		{
			const unsigned c = colors[pens[16 * y + x]];
			const int red = c & 31, green = (c >> 5) & 31, blue = c >> 10;
			/* Keep high-contrast repeating details in the original tile path. */
			if (abs(red - (int)(anchor & 31)) > 4 ||
				abs(green - (int)((anchor >> 5) & 31)) > 4 ||
				abs(blue - (int)(anchor >> 10)) > 4) return false;
			r += red;
			g += green;
			b += blue;
		}
		band->shades[y].normal = pack_sky(r, g, b, false);
		band->shades[y].shadow = pack_sky(r, g, b, true);
	}
	return true;
}

static const sky_shade *sky_band_row(sky_band *band, const zoomed_row *row, const mix_layer *sky)
{
	const int first = row->pages[0] * 512 + (row->line >> 4) * 32;
	const int second = row->pages[1] * 512 + (row->line >> 4) * 32;
	const int key = (first << 7) | row->pages[1];
	if (band->map_row != key)
	{
		const int cell = video_word(first);
		for (int x = 0; x < 32; x++)
			if (video_word(first + x) != cell || video_word(second + x) != cell) return NULL;
		if (cell != band->cell)
		{
			const unsigned tile = tile_index(((unsigned)row->bank << 13) | (cell & 0x1fff));
			if (!make_sky_shades(band, tile, cell, sky)) return NULL;
			band->cell = cell;
		}
		band->map_row = key;
	}
	return &band->shades[(row->line & 15) ^ ((band->cell & 0x8000) ? 15 : 0)];
}

static inline unsigned direct_pixel(const mixing *setup, const mix_layer *road, const mix_layer *sky,
	const mix_layer *back, const zoomed_row *row, int x, int pen, unsigned sprite, int backdrop, const sky_shade *shade)
{
	const mix_layer *layer = pen ? road : sky;
	const mix_layer *sp = &setup->sprites[(sprite >> setup->grouping.shift) & setup->grouping.mask];
	const int sprite_pen = sprite & setup->sprite_bits;
	const bool draws_sprite = (sprite & 0x7fff) != 0x7fff && (sprite_pen & 0x7ffe) != setup->shadow_pen;
	if (!pen && !shade && !(draws_sprite && sp->priority > sky->priority))
	{
		pen = zoomed_pixel(row, x);
		if (!pen)
		{
			layer = back;
			pen = backdrop;
		}
	}
	bool shadow = false;
	if (sprite != 0xffff && sp->priority > layer->priority)
	{
		shadow = (~sprite & setup->shadow_mask) != 0;
		if (draws_sprite)
		{
			layer = sp;
			pen = sprite_pen;
		}
		else if ((sprite & 0x7fff) != 0x7fff)
			shadow = true;
	}
	if (shade && layer == sky) return shadow ? shade->shadow : shade->normal;
	unsigned color = palette_color(0, layer, pen) & 0x7fff;
	/* Mask each RGB555 channel's low bit before halving. This is the plain
	   arcade shadow, including when a flag darkens another sprite. */
	if (shadow) color = (color & 0x7bde) >> 1;
	return packed_colors[color];
}

/* Specialise the two inner loops so illustrated backgrounds incur no extra
   gradient checks per pixel. */
#if defined(__GNUC__)
__attribute__((always_inline))
#endif
static inline void draw_direct_row(const mixing *setup,
	const mix_layer *road, const mix_layer *sky, const mix_layer *back, const zoomed_row *sky_row,
	int y, int start, int end, const uint16_t *road_pens, int backdrop, const sky_shade *shade)
{
	const uint16_t *sprites = video.sprites[0][video.shown_sprites][y];
	uint8_t *out8 = (uint8_t *)packed_output + 320 * y;
	uint16_t *out16 = packed_565 ? (uint16_t *)packed_output + 320 * y : NULL;
	for (int x = start; x < end; x += 2)
	{
		unsigned first, second;
		const uint16_t left = road_pens[x], right = road_pens[x + 1];
		const bool no_sprites = (sprites[x] & sprites[x + 1]) == 0xffff;
		if (no_sprites && left && right)
		{
			first = packed_colors[palette_color(0, road, left) & 0x7fff];
			second = left == right ? first : packed_colors[palette_color(0, road, right) & 0x7fff];
		}
		else if (no_sprites && !left && !right && scenery_filter)
		{
			if (shade)
				first = second = shade->normal;
			else
			{
				uint16_t a, b;
				sky_pens(sky_row, x, &a, &b);
				first = packed_colors[palette_color(0, a ? sky : back, a ? a : backdrop) & 0x7fff];
				second = a == b ? first : packed_colors[palette_color(0, b ? sky : back, b ? b : backdrop) & 0x7fff];
			}
		}
		else
		{
			first = direct_pixel(setup, road, sky, back, sky_row, x, left, sprites[x], backdrop, shade);
			second = direct_pixel(setup, road, sky, back, sky_row, x + 1, right, sprites[x + 1], backdrop, shade);
		}
		if (out16)
		{
			out16[x] = first;
			out16[x + 1] = second;
		}
		else
		{
			out8[x] = (uint8_t)first;
			out8[x + 1] = (uint8_t)second;
		}
	}
}

typedef struct road_strip
{
	uint32_t key;
	uint16_t pens[16], rgb[16];
	uint8_t indexed[16];
	bool opaque;
} road_strip;

static const road_strip *road_strip_at(road_strip *strips, const scrolled_row *row,
	unsigned column, const mix_layer *road)
{
	const unsigned page = row->pages[(column >> 9) & 1];
	const uint16_t cell = video_word((page << 9) | ((unsigned)(row->line >> 4) << 5) | ((column >> 4) & 31));
	const unsigned line = (row->line & 15) ^ (cell & 0x8000 ? 15 : 0);
	const unsigned flip = ((cell & 0x4000) != 0) ^ (row->x_step < 0);
	const unsigned cell_key = ((unsigned)row->bank << 14) | (cell & 0x3fff);
	const unsigned key = 1 + ((cell_key * 16 + line) * 2 + flip);
	road_strip *strip = strips + ((key ^ (key >> 5) ^ (key >> 11)) & 63);
	if (strip->key != key)
	{
		const unsigned tile = tile_index(((unsigned)row->bank << 13) | (cell & 0x1fff));
		const uint8_t *pixels = video.graphics.tile_pens[tile];
		strip->opaque = true;
		for (int i = 0; i < 16; ++i)
		{
			unsigned pen = pixels ? pixels[line * 16 + (flip ? 15 - i : i)] : 0;
			strip->pens[i] = pen ? (cell & 0x1ff0) | pen : 0;
			if (!pen) strip->opaque = false;
			unsigned color = packed_colors[palette_color(0, road, strip->pens[i]) & 0x7fff];
			strip->rgb[i] = color;
			strip->indexed[i] = (uint8_t)color;
		}
		strip->key = key;
	}
	return strip;
}

/* A fully visible sprite pen above both tilemaps has a single packed result
   for the frame, including the shadow flag. An impossible tag marks a miss. */
typedef struct sprite_color { unsigned tag, color; } sprite_color;
static inline bool visible_sprite(sprite_color *cache, const mixing *setup, const mix_layer *road,
	unsigned sprite, unsigned *color)
{
	if (sprite == 0xffff) return false;
	sprite_color *entry = cache + ((sprite ^ (sprite >> 7)) & 255);
	if (entry->tag != sprite + 1)
	{
		const mix_layer *sp = &setup->sprites[(sprite >> setup->grouping.shift) & setup->grouping.mask];
		const unsigned pen = sprite & setup->sprite_bits;
		entry->tag = sprite + 1;
		entry->color = 0x10000;
		if ((sprite & 0x7fff) != 0x7fff && (pen & 0x7ffe) != setup->shadow_pen && sp->priority > road->priority)
		{
			unsigned c = palette_color(0, sp, pen) & 0x7fff;
			if (~sprite & setup->shadow_mask) c = (c & 0x7bde) >> 1;
			entry->color = packed_colors[c];
		}
	}
	*color = entry->color;
	return *color <= 0xffff;
}

static void direct_spans(road_strip *strips, sprite_color *colors, const mixing *setup,
	const mix_layer *road, const mix_layer *sky, const mix_layer *back, const zoomed_row *sky_row,
	const scrolled_row *row, int y, int backdrop, const sky_shade *shade)
{
	uint16_t pens[320];
	const uint16_t *sprites = video.sprites[0][video.shown_sprites][y];
	uint16_t *out16 = packed_565 ? (uint16_t *)packed_output + 320 * y : NULL;
	uint8_t *out8 = (uint8_t *)packed_output + 320 * y;
	int x = 0;
	while (x < 320)
	{
		int first = x;
		while (x < 320 && (sprites[x] & sprites[x + 1]) == 0xffff) x += 2;
		if (x != first)
		{
			int i = first;
			unsigned col = row->x_start + row->x_step * (i - video.visible.left - video.side);
			while (i < x)
			{
				const int at = row->x_step > 0 ? col & 15 : 15 - (int)(col & 15);
				int n = 16 - at;
				if (n > x - i) n = x - i;
				const road_strip *strip = road_strip_at(strips, row, col, road);
				if (strip->opaque && n >= 2)
				{
					n &= ~1;
					if (out16) memcpy(out16 + i, strip->rgb + at, n * sizeof(*out16));
					else memcpy(out8 + i, strip->indexed + at, n);
				}
				else
				{
					if (n & 1)
					{
						pens[i + n] = scrolled_pixel(row, i + n);
						memcpy(pens + i, strip->pens + at, n * sizeof(*pens));
						++n;
					}
					else memcpy(pens + i, strip->pens + at, n * sizeof(*pens));
					draw_direct_row(setup, road, sky, back, sky_row, y, i, i + n, pens, backdrop, shade);
				}
				i += n;
				col += n * row->x_step;
			}
		}
		while (x < 320)
		{
			unsigned left, right;
			if (!visible_sprite(colors, setup, road, sprites[x], &left) ||
				!visible_sprite(colors, setup, road, sprites[x + 1], &right)) break;
			if (out16)
			{
				out16[x] = left;
				out16[x + 1] = right;
			}
			else
			{
				out8[x] = (uint8_t)left;
				out8[x + 1] = (uint8_t)right;
			}
			x += 2;
		}
		if (x < 320 && (sprites[x] & sprites[x + 1]) != 0xffff)
		{
			first = x;
			do { x += 2; } while (x < 320 && (sprites[x] & sprites[x + 1]) != 0xffff);
			scrolled_span(row, pens, first, x);
			draw_direct_row(setup, road, sky, back, sky_row, y, first, x, pens, backdrop, shade);
		}
	}
}

/* Sampling only chooses the implementation; either path resolves the same pixels.
   Long stretches of empty sprite buffer are cheaper with the straight row loop. */
static bool foreground_is_dense(void)
{
	unsigned occupied = 0;
	for (int y = 4; y < HEIGHT; y += 8)
		for (int x = 4; x < 320; x += 16)
			occupied += video.sprites[0][video.shown_sprites][y][x] != 0xffff;
	return occupied * 4 >= (HEIGHT / 8) * 20;
}

static void draw_dense_road(const mixing *setup, const mix_layer *road, const mix_layer *sky,
	const mix_layer *back, const zoomed_layer *sky_map, const scrolled_layer *road_map)
{
	road_strip strips[64] = {0};
	sprite_color colors[256] = {0};
	sky_band band = { .map_row = -1, .cell = -1 };
	bool in_sky = scenery_filter != NULL;
	for (int y = 0; y < HEIGHT; y++)
	{
		const zoomed_row sky_row = zoomed_row_at(sky_map, y);
		const scrolled_row road_row = scrolled_row_at(road_map, y);
		const sky_shade *shade = in_sky ? sky_band_row(&band, &sky_row, sky) : NULL;
		in_sky = shade != NULL;
		const int backdrop = backdrop_pen(y);
		direct_spans(strips, colors, setup, road, sky, back, &sky_row, &road_row, y, backdrop, shade);
	}
}

/* An overlay can occupy only a few scanlines. Keep the exact tile pens
   of the general mixer, including the detailed sky behind overlay screens. */
static void draw_overlay_scene(mixing *setup, const mix_layer *text,
	const mix_layer *bitmap, const mix_layer *road, const mix_layer *sky, const mix_layer *back)
{
	const zoomed_layer sky_map = prepare_zoomed(0, NULL);
	const scrolled_layer road_map = prepare_scrolled(2);
	layer *road_layer = &video.layers[LAYER_NBG2];
	for (int y = 0; y < HEIGHT; ++y)
	{
		const zoomed_row sky_row = zoomed_row_at(&sky_map, y);
		const scrolled_row road_row = scrolled_row_at(&road_map, y);
		const int backdrop = backdrop_pen(y);
		uint16_t *road_pens = road_layer->pixels[y];
		const int transparent = scrolled_span(&road_row, road_pens, 0, 320);
		if ((text && !video.layers[LAYER_TEXT].empty[y]) || (bitmap && !video.layers[LAYER_BITMAP].empty[y]))
		{
			road_layer->empty[y] = transparent == 320;
			draw_visible_sky(&sky_row, y, 0);
			for (int x = 0; x < 320; ++x)
				video.layers[LAYER_BACKDROP].pixels[y][x] = backdrop;
			mix_rows(setup, y, y);
		}
		else
			draw_direct_row(setup, road, sky, back, &sky_row, y, 0, 320, road_pens, backdrop, NULL);
	}
}

/* Draw the common single-screen road scene directly into the packed output.
   Overlay rows, clipping, blending and sprite flips use the general mixer. */
static bool draw_direct(mixing *setup, int drawn)
{
	const int tiles = (1 << LAYER_NBG0) | (1 << LAYER_NBG2);
	const int overlays = (1 << LAYER_TEXT) | (1 << LAYER_BITMAP);
	if (!packed_output || video.screens_shown != 1 || video.visible.right != 319 ||
		half_scenery || video.extra_used || s32_video_tile_observer ||
		(drawn & ~overlays) != tiles || (video_register(0x02) & ((1 << 11) | (1 << 13))) ||
		(setup->sprite_flips & 3))
		return false;

	const mix_layer *layers[LAYERS] = { NULL };
	for (const mix_layer *entry = setup->background; ; entry++)
	{
		layers[entry->index] = entry;
		if (entry->index == LAYER_BACKDROP) break;
	}
	const mix_layer *sky = layers[LAYER_NBG0];
	const mix_layer *road = layers[LAYER_NBG2];
	const mix_layer *back = layers[LAYER_BACKDROP];
	if (!sky || !road || !road->plain || !sky->plain || !back->plain || road->priority <= sky->priority)
		return false;
	for (int group = 0; group <= setup->grouping.mask; group++)
		if (!setup->sprites[group].plain) return false;
	for (int y = 0; y < HEIGHT; y++)
		if ((layers[LAYER_TEXT] && !video.layers[LAYER_TEXT].empty[y]) ||
			(layers[LAYER_BITMAP] && !video.layers[LAYER_BITMAP].empty[y]))
		{
			draw_overlay_scene(setup, layers[LAYER_TEXT], layers[LAYER_BITMAP], road, sky, back);
			return true;
		}

	const zoomed_layer sky_map = prepare_zoomed(0, NULL);
	const scrolled_layer road_map = prepare_scrolled(2);
	sky_band band = { .map_row = -1, .cell = -1 };
	bool in_sky = scenery_filter != NULL;
	if (scenery_filter && foreground_is_dense())
		draw_dense_road(setup, road, sky, back, &sky_map, &road_map);
	else
	{
		for (int y = 0; y < HEIGHT; y++)
		{
			const zoomed_row sky_row = zoomed_row_at(&sky_map, y);
			const scrolled_row road_row = scrolled_row_at(&road_map, y);
			const sky_shade *shade = in_sky ? sky_band_row(&band, &sky_row, sky) : NULL;
			in_sky = shade != NULL;
			const int backdrop = backdrop_pen(y);
			uint16_t pens[320];
			scrolled_span(&road_row, pens, 0, 320);
			if (shade)
				draw_direct_row(setup, road, sky, back, &sky_row, y, 0, 320, pens, backdrop, shade);
			else
				draw_direct_row(setup, road, sky, back, &sky_row, y, 0, 320, pens, backdrop, NULL);
		}
	}
	return true;
}

/* ---- the frame ---- */

void s32_video_draw_frame(void)
{
	const bool board_wide = bit(video_register(0x00), 15);
	const int width = board_wide ? 416 : 320 + 2 * video.wide_side;

	video.side = board_wide ? 0 : video.wide_side;
	video.visible.left = 0;
	video.visible.right = width - 1;
	video.visible.top = 0;
	video.visible.bottom = HEIGHT - 1;

	/* each screen's I/O chip turns it on (CNT1); the layers they share are drawn once, those the mixers of
	   the screens shown take (a layer with no priority is not mixed); a screen the port does not show keeps
	   its last frame */
	int wanted = 0;
	int drawn = -1;

	for (int screen = 0; screen < S32_SCREENS; screen++)
		if (bit((uint32_t)video.screens_shown, screen) && s32_display_enabled(screen))
			for (int index = LAYER_TEXT; index <= LAYER_BITMAP; index++)
				if ((mixer_register(screen, 0x20 + 2 * (uint32_t)index) & 0x0f) != 0)
					wanted |= 1 << index;
	for (int screen = 0; screen < S32_SCREENS; screen++)
	{
		if (!bit((uint32_t)video.screens_shown, screen))
			continue;
		if (!s32_display_enabled(screen))
		{
			if (packed_output && !screen && width == 320)
				memset(packed_output, 0, 320 * HEIGHT * (packed_565 ? 2 : 1));
			else for (int y = 0; y < HEIGHT; y++)
				memset(video.frame[screen][y], 0, (size_t)width * sizeof(video.frame[screen][y][0]));
			continue;
		}
		const bool first = drawn < 0;
		if (first)
		{
			drawn = enabled_layers(wanted);
			if (bit((uint32_t)drawn, LAYER_TEXT)) draw_text();
			if (bit((uint32_t)drawn, LAYER_BITMAP)) draw_bitmap();
		}
		mixing *setup = prepare_mix(screen, drawn);
		if (first)
		{
			if (draw_direct(setup, drawn))
				continue;
			draw_tile_layers(drawn);
			draw_backdrop();
		}
		s32_workers_rows(mix_rows, setup, video.visible.top, video.visible.bottom);
	}
}

void s32_video_show_screens(int screens)
{
	/* A port may switch cabinets after an erase-only or draw-only event. */
	for (int screen = 0; screen < S32_SCREENS; screen++)
		if (bit((unsigned)(screens & ~video.screens_shown), screen))
			for (int bank = 0; bank < 2; bank++)
				apply_pending_erase(screen, bank);
	video.screens_shown = screens & ((1 << S32_SCREENS) - 1);
}

void s32_video_init_graphics(const s32_graphics *graphics)
{
	uint8_t *image_pens = video.image_pens;
	const uint8_t **image_tiles = video.image_tiles;
	const uint8_t *sprite_rom = video.sprite_rom;
	const uint32_t sprite_rom_words = video.sprite_rom_words;

	memset(&video, 0, sizeof(video));
	scenery_filter = NULL;
	s32_video_set_output(NULL, S32_RGB332, false);
	video.screens_shown = (1 << S32_SCREENS) - 1;
	video.graphics = *graphics;
	pair_scenery_rows = false;
	free(sprite_art_cache);
	sprite_art_cache = NULL;
	classify_tiles(graphics);
	video.image_pens = image_pens;
	video.image_tiles = image_tiles;
	video.sprite_rom = sprite_rom;
	video.sprite_rom_words = sprite_rom_words;
	video.visible.right = 319;
	video.visible.bottom = HEIGHT - 1;
	s32_board.sprites_due = s32_video_sprites;
	s32_board.frame_shown = s32_video_draw_frame;
}

/* A word of the sprite chips' image s32_video_init was given */
static uint32_t image_sprite_word(uint32_t place)
{
	return big_endian_word(&video.sprite_rom[(size_t)(place % video.sprite_rom_words) * 4]);
}

void s32_video_init(const uint8_t *tiles, size_t tiles_size, const uint8_t *sprites, size_t sprites_size)
{
	/* a tile's rows of 8 bytes: two pens a byte with the high nibble first, and the byte pairs of each half
	   of the row swapped */
	static const uint8_t byte_of_column[16] = { 0, 0, 2, 2, 1, 1, 3, 3, 4, 4, 6, 6, 5, 5, 7, 7 };
	const uint32_t count = (uint32_t)(tiles_size / 128);
	s32_graphics graphics = {0};

	free(video.image_pens);
	free((void *)video.image_tiles);
	video.image_pens = malloc((size_t)count * 256);
	video.image_tiles = malloc((size_t)count * sizeof(video.image_tiles[0]));
	if (!video.image_pens || !video.image_tiles)
		abort();
	for (uint32_t tile = 0; tile < count; tile++)
	{
		uint8_t *pens = &video.image_pens[(size_t)tile * 256];

		for (int y = 0; y < 16; y++)
			for (int x = 0; x < 16; x++)
			{
				const uint8_t value = tiles[(size_t)tile * 128 + y * 8 + byte_of_column[x]];

				pens[y * 16 + x] = (x & 1) ? (value & 0x0f) : (value >> 4);
			}
		video.image_tiles[tile] = pens;
	}
	video.sprite_rom = sprites;
	video.sprite_rom_words = (uint32_t)(sprites_size / 4);
	graphics.tile_pens = video.image_tiles;
	graphics.tile_count = count;
	graphics.sprite_word = image_sprite_word;
	graphics.sprite_banks = video.sprite_rom_words >> 20;
	s32_video_init_graphics(&graphics);
}

const uint32_t *s32_video_frame(int screen, int *width)
{
	if (width)
		*width = video.visible.right + 1;
	return &video.frame[screen & 1][0][0];
}

void s32_video_use_extra_palette(bool used)
{
	video.extra_used = used;
}

void s32_video_extra_colour(int screen, uint32_t index, uint16_t colour)
{
	video.extra_palette[screen & 1][index & 0x3fff] = colour;
	video.extra_set[screen & 1][index & 0x3fff] = true;
}

void s32_video_set_wide_side(int side)
{
	video.wide_side = side < 0 ? 0 : (side > S32_VIDEO_WIDE_SIDE_MOST ? S32_VIDEO_WIDE_SIDE_MOST : side);
}

int s32_video_side(void)
{
	return video.side;
}

uint64_t s32_video_frame_hash(void)
{
	uint64_t hash = 0xcbf29ce484222325ull;

	for (int screen = 0; screen < S32_SCREENS; screen++)
		for (int y = 0; y < HEIGHT; y++)
			for (int x = 0; x <= video.visible.right; x++)
				for (int shift = 0; shift < 24; shift += 8)
					hash = (hash ^ ((video.frame[screen][y][x] >> shift) & 0xff)) * 0x100000001b3ull;
	return hash;
}
