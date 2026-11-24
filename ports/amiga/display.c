#include "backend.h"
#include <string.h>
#include <stdio.h>
#include "s32/video.h"

#include <cybergraphics/cybergraphics.h>
#include <graphics/gfxbase.h>
#include <exec/memory.h>
#include <graphics/displayinfo.h>
#include <intuition/intuition.h>
#include <proto/cybergraphics.h>
#include <proto/Picasso96.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/intuition.h>

enum { WIDTH = 320, HEIGHT = 256, TOP = (HEIGHT - S32_VIDEO_HEIGHT) / 2,
	PLANES = 8, PLANE_BYTES = WIDTH * HEIGHT / 8,
	/* Kalms 040/060 prefetches up to 16 bytes past the last source pixel. */
	CHUNKY_BYTES = WIDTH * S32_VIDEO_HEIGHT + 32 };
extern void amiga_c2p_init(void);
extern void amiga_c2p(const uint8_t *pixels, void *planes);

struct GfxBase *GfxBase;
struct IntuitionBase *IntuitionBase;
struct Library *CyberGfxBase;
struct Library *P96Base;

static struct Screen *screen;
static struct Window *window;
/* Intuition sprite: two control words, one transparent row, two end words. */
enum { POINTER_BYTES = 6 * sizeof(UWORD) };
static UWORD *blank_pointer;
static unsigned rtg_left, rtg_top;
static struct BitMap bitmaps[2];
static struct ScreenBuffer *screen_buffers[2];
static struct MsgPort *safe_port;
static uint8_t *planes[2], *chunky;
static unsigned displayed;
static bool waiting_for_safe;
static bool use_rtg;
static struct RenderInfo p96_frame;
static uint16_t *rtg_pixels;
static s32_packed_format output_format;

static void load_palette(void)
{
	ULONG colors[1 + 256 * 3 + 1];
	colors[0] = 256u << 16;
	colors[1 + 256 * 3] = 0;
	for (unsigned i = 0; i < 256; ++i)
	{
		colors[1 + i * 3] = ((i >> 5) * 255 / 7) * 0x01010101u;
		colors[2 + i * 3] = (((i >> 2) & 7) * 255 / 7) * 0x01010101u;
		colors[3 + i * 3] = ((i & 3) * 255 / 3) * 0x01010101u;
	}
	LoadRGB32(&screen->ViewPort, colors);
}

static char display_problem[224];

static const char *chip_error(const char *stage)
{
	snprintf(display_problem, sizeof(display_problem),
		"%s (Chip RAM free %lu bytes, largest block %lu bytes)", stage,
		AvailMem(MEMF_CHIP), AvailMem(MEMF_CHIP | MEMF_LARGEST));
	return display_problem;
}

static const char *screen_error(LONG code, ULONG mode)
{
	const char *reason;
	switch (code)
	{
		case OSERR_NOMONITOR: reason = "monitor driver unavailable"; break;
		case OSERR_UNKNOWNMODE: reason = "unknown display mode"; break;
		case OSERR_NOTAVAILABLE: reason = "display mode unavailable"; break;
		case OSERR_NOCHIPS: reason = "chipset does not support this mode"; break;
		case OSERR_TOODEEP: reason = "display mode does not support the requested depth"; break;
		case OSERR_NOCHIPMEM: reason = "insufficient Chip RAM"; break;
		case OSERR_NOMEM: reason = "insufficient memory"; break;
		default: reason = "screen request failed"; break;
	}
	snprintf(display_problem, sizeof(display_problem),
		"OpenScreen: %s (mode 0x%08lx, Intuition error %ld, Chip RAM free %lu, largest block %lu)",
		reason, mode, code, AvailMem(MEMF_CHIP), AvailMem(MEMF_CHIP | MEMF_LARGEST));
	return display_problem;
}

static struct Screen *open_screen(bool rtg, ULONG mode, LONG *code, struct BitMap *bitmap)
{
	struct TagItem tags[] = {
		{SA_DisplayID, mode}, {SA_Width, WIDTH}, {SA_Height, HEIGHT},
		{SA_Depth, rtg ? 16 : PLANES}, {SA_Type, CUSTOMSCREEN},
		{SA_ShowTitle, FALSE}, {SA_Quiet, TRUE}, {SA_ErrorCode, (ULONG)code},
		{bitmap ? SA_BitMap : TAG_IGNORE, (ULONG)bitmap},
		{TAG_DONE, 0}
	};
	*code = 0;
	return OpenScreenTagList(NULL, tags);
}

static struct Screen *open_pal(struct BitMap *bitmap, ULONG *mode, LONG *code)
{
	*mode = PAL_MONITOR_ID | LORES_KEY;
	struct Screen *result = open_screen(false, *mode, code, bitmap);
	if (!result && (!*code || *code == OSERR_NOMONITOR ||
		*code == OSERR_UNKNOWNMODE || *code == OSERR_NOTAVAILABLE))
	{
		struct DisplayInfo info = {0};
		if (GetDisplayInfoData(NULL, (UBYTE *)&info, sizeof(info), DTAG_DISP, LORES_KEY)
			&& !info.NotAvailable && (info.PropertyFlags & DIPF_IS_PAL))
		{
			*mode = LORES_KEY;
			result = open_screen(false, *mode, code, bitmap);
		}
	}
	return result;
}

static bool allocate_planes(unsigned index)
{
	planes[index] = AllocMem(PLANES * PLANE_BYTES, MEMF_CHIP | MEMF_CLEAR);
	if (!planes[index]) return false;
	InitBitMap(&bitmaps[index], PLANES, WIDTH, HEIGHT);
	for (unsigned plane = 0; plane < PLANES; ++plane)
		bitmaps[index].Planes[plane] = planes[index] + plane * PLANE_BYTES;
	return true;
}

static const char *open_aga(void)
{
	chunky = AllocMem(CHUNKY_BYTES, MEMF_FAST | MEMF_CLEAR);
	if (!chunky) return "AGA renderer could not allocate 71712 bytes of Fast RAM";
	if (!allocate_planes(0)) return chip_error("AGA display needs an 81920-byte Chip RAM block");
	ULONG mode;
	LONG code;
	/* Use native flips when the classic chipset is identified. Some compatible
	   drivers expose PAL eight-plane modes without implementing this flag. */
	if ((GfxBase->ChipRevBits0 & GFXF_AA_LISA) && (safe_port = CreateMsgPort()) && allocate_planes(1))
	{
		screen = open_pal(&bitmaps[0], &mode, &code);
		if (screen)
		{
			/* The front buffer must refer to the bitmap supplied to OpenScreen. */
			screen_buffers[0] = AllocScreenBuffer(screen, &bitmaps[0], 0);
			screen_buffers[1] = AllocScreenBuffer(screen, &bitmaps[1], 0);
			if (screen_buffers[0] && screen_buffers[1])
			{
				for (unsigned i = 0; i < 2; ++i)
					screen_buffers[i]->sb_DBufInfo->dbi_SafeMessage.mn_ReplyPort = safe_port;
			}
			else
			{
				for (unsigned i = 0; i < 2; ++i)
					if (screen_buffers[i]) FreeScreenBuffer(screen, screen_buffers[i]);
				screen_buffers[0] = screen_buffers[1] = NULL;
				CloseScreen(screen);
				screen = NULL;
			}
		}
	}
	if (!screen)
	{
		if (planes[1])
		{
			FreeMem(planes[1], PLANES * PLANE_BYTES);
			planes[1] = NULL;
		}
		screen = open_pal(NULL, &mode, &code);
		if (!screen) return screen_error(code, mode);
		if (GetBitMapAttr(screen->RastPort.BitMap, BMA_DEPTH) != PLANES)
			return "PAL AGA screen opened without eight bitplanes";
	}
	amiga_c2p_init();
	load_palette();
	return NULL;
}

/* A P96 mode can be larger than the arcade image. Open the advertised mode
   at its native size and centre the upload within the full-screen window. */
static const char *open_picasso96(void)
{
	struct TagItem modes[] = {
		{P96MA_MinWidth, WIDTH}, {P96MA_MinHeight, HEIGHT},
		{P96MA_MinDepth, 16}, {P96MA_MaxDepth, 16},
		{P96MA_FormatsAllowed, RGBFF_R5G6B5 | RGBFF_R5G6B5PC | RGBFF_B5G6R5PC},
		{TAG_DONE, 0}
	};
	P96Base = OpenLibrary(P96NAME, 2);
	if (!P96Base) return "Could not open Picasso96API.library v2";
	struct List *list = p96AllocModeListTagList(modes);
	if (!list) return "Picasso96 could not enumerate 16-bit screen modes";
	ULONG mode = INVALID_ID, width = 0, height = 0;
	for (struct Node *node = list->lh_Head; node->ln_Succ; node = node->ln_Succ)
	{
		const struct P96Mode *entry = (const struct P96Mode *)node;
		if (entry->Width < WIDTH || entry->Height < HEIGHT || entry->Depth != 16) continue;
		if (mode == INVALID_ID || (ULONG)entry->Width * entry->Height < width * height)
		{
			mode = entry->DisplayID;
			width = entry->Width;
			height = entry->Height;
		}
	}
	p96FreeModeList(list);
	if (mode == INVALID_ID) return "Picasso96 has no 16-bit screen mode of at least 320x256";
	LONG code = 0;
	struct TagItem tags[] = {
		{P96SA_DisplayID, mode}, {P96SA_Width, width}, {P96SA_Height, height},
		{P96SA_Depth, 16}, {P96SA_Type, CUSTOMSCREEN}, {P96SA_ShowTitle, FALSE},
		{P96SA_Quiet, TRUE}, {P96SA_ErrorCode, (ULONG)&code}, {TAG_DONE, 0}
	};
	screen = p96OpenScreenTagList(tags);
	if (!screen) return screen_error(code, mode);
	/* The driver handles destination pitch and any format conversion. Match
	   native 565 where possible so the normal upload only copies pixels. */
	switch (p96GetBitMapAttr(screen->RastPort.BitMap, P96BMA_RGBFORMAT))
	{
		case RGBFB_R5G6B5PC:
			output_format = S32_RGB565_SWAP;
			p96_frame.RGBFormat = RGBFB_R5G6B5PC;
			break;
		case RGBFB_B5G6R5PC:
			output_format = S32_BGR565_SWAP;
			p96_frame.RGBFormat = RGBFB_B5G6R5PC;
			break;
		default:
			output_format = S32_RGB565;
			p96_frame.RGBFormat = RGBFB_R5G6B5;
			break;
	}
	p96_frame.BytesPerRow = WIDTH * sizeof(*rtg_pixels);
	return NULL;
}

static void clear_rtg_window(void)
{
	/* Intuition clips this fill to the damage region during a refresh. */
	if (P96Base)
		p96RectFill(window->RPort, 0, 0, window->Width - 1, window->Height - 1, 0);
	else
		FillPixelArray(window->RPort, 0, 0, window->Width, window->Height, 0);
}

const char *amiga_display_open(enum amiga_display_mode requested)
{
	ULONG mode = PAL_MONITOR_ID | LORES_KEY;
	const bool rtg = requested != AMIGA_AGA;
	use_rtg = rtg;
	GfxBase = (struct GfxBase *)OpenLibrary("graphics.library", 39);
	IntuitionBase = (struct IntuitionBase *)OpenLibrary("intuition.library", 39);
	if (!GfxBase) return "Could not open graphics.library v39";
	if (!IntuitionBase) return "Could not open intuition.library v39";

	const char *p96_error = NULL;
	if (rtg)
	{
		p96_error = open_picasso96();
		if (p96_error && requested == AMIGA_P96) return p96_error;
		if (p96_error && P96Base)
		{
			CloseLibrary(P96Base);
			P96Base = NULL;
		}
	}
	if (rtg && !screen)
	{
		CyberGfxBase = OpenLibrary("cybergraphics.library", 41);
		if (!CyberGfxBase)
			return p96_error;
		mode = BestCModeIDTags(CYBRBIDTG_NominalWidth, WIDTH, CYBRBIDTG_NominalHeight, HEIGHT,
			CYBRBIDTG_Depth, 16, TAG_DONE);
		if (mode == INVALID_ID || !IsCyberModeID(mode))
			return "No 16-bit CyberGraphX screen mode available";
	}
	if (!rtg)
	{
		const char *error = open_aga();
		if (error) return error;
	}
	else if (!screen)
	{
		LONG code;
		screen = open_screen(true, mode, &code, NULL);
		if (!screen) return screen_error(code, mode);
	}
	if (rtg)
	{
		rtg_left = (screen->Width - WIDTH) / 2;
		rtg_top = (screen->Height - HEIGHT) / 2 + TOP;
		if (!P96Base)
		{
			switch (GetCyberMapAttr(screen->RastPort.BitMap, CYBRMATTR_PIXFMT))
			{
				case PIXFMT_RGB16: output_format = S32_RGB565; break;
				case PIXFMT_BGR16: output_format = S32_BGR565; break;
				case PIXFMT_RGB16PC: output_format = S32_RGB565_SWAP; break;
				case PIXFMT_BGR16PC: output_format = S32_BGR565_SWAP; break;
			}
		}
		if (output_format != S32_RGB332)
		{
			rtg_pixels = AllocMem(WIDTH * S32_VIDEO_HEIGHT * 2, MEMF_FAST | MEMF_CLEAR);
			if (!rtg_pixels) return "Could not allocate the RTG framebuffer in Fast RAM";
			p96_frame.Memory = rtg_pixels;
		}
	}
	blank_pointer = AllocMem(POINTER_BYTES, MEMF_CHIP | MEMF_CLEAR);
	if (!blank_pointer) return chip_error("Could not allocate the blank mouse pointer");
	window = OpenWindowTags(NULL, WA_CustomScreen, screen, WA_Left, 0, WA_Top, 0,
		WA_Width, (ULONG)screen->Width, WA_Height, (ULONG)screen->Height, WA_Borderless, TRUE, WA_Activate, TRUE,
		WA_SimpleRefresh, TRUE, WA_Backdrop, TRUE,
		WA_IDCMP, (ULONG)(IDCMP_RAWKEY | IDCMP_INACTIVEWINDOW | (rtg ? IDCMP_REFRESHWINDOW : 0)), TAG_DONE);
	if (!window) return "OpenWindow failed on the game screen";
	SetPointer(window, blank_pointer, 1, 16, 0, 0);
	if (rtg) clear_rtg_window();
	return NULL;
}

bool amiga_display_events(bool keys[128])
{
	struct IntuiMessage *message;
	while ((message = (struct IntuiMessage *)GetMsg(window->UserPort)))
	{
		if (message->Class == IDCMP_RAWKEY)
			keys[message->Code & 127] = !(message->Code & 128);
		else if (message->Class == IDCMP_REFRESHWINDOW)
		{
			BeginRefresh(window);
			clear_rtg_window();
			EndRefresh(window, TRUE);
		}
		else if (message->Class == IDCMP_INACTIVEWINDOW)
			for (unsigned i = 0; i < 128; ++i)
				keys[i] = false;
		ReplyMsg((struct Message *)message);
	}
	return !keys[0x45];
}

void amiga_display_target(bool half)
{
	s32_video_set_output(use_rtg ? (void *)rtg_pixels : (void *)chunky, output_format, half);
}

bool amiga_display_frame(const uint32_t *pixels)
{
	if (P96Base)
	{
		p96WritePixelArray(&p96_frame, 0, 0, window->RPort, rtg_left, rtg_top, WIDTH, S32_VIDEO_HEIGHT);
		return true;
	}
	if (use_rtg)
	{
		if (rtg_pixels)
		{
			ULONG pitch;
			UBYTE *base;
			APTR lock = LockBitMapTags(screen->RastPort.BitMap, LBMI_BASEADDRESS, (ULONG)&base,
				LBMI_BYTESPERROW, (ULONG)&pitch, TAG_DONE);
			if (!lock) return false;
			for (int y = 0; y < S32_VIDEO_HEIGHT; y++)
			{
				uint16_t *dst = (uint16_t *)(base + (rtg_top + y) * pitch) + rtg_left;
				const uint16_t *src = rtg_pixels + y * WIDTH;
				memcpy(dst, src, WIDTH * 2);
			}
			UnLockBitMap(lock);
			return true;
		}
		return WritePixelArray((APTR)pixels, 0, 0, S32_VIDEO_MAX_WIDTH * sizeof(*pixels),
			window->RPort, rtg_left, rtg_top, WIDTH, S32_VIDEO_HEIGHT, RECTFMT_ARGB) == WIDTH * S32_VIDEO_HEIGHT;
	}
	if (!screen_buffers[0])
	{
		amiga_c2p(chunky, planes[0]);
		BltBitMapRastPort(&bitmaps[0], 0, TOP, window->RPort, 0, TOP, WIDTH, S32_VIDEO_HEIGHT, 0xc0);
		WaitBlit();
		return true;
	}
	if (waiting_for_safe && !GetMsg(safe_port))
		return false;
	waiting_for_safe = false;

	const unsigned next = displayed ^ 1;
	amiga_c2p(chunky, planes[next]);
	if (ChangeScreenBuffer(screen, screen_buffers[next]))
	{
		displayed = next;
		waiting_for_safe = true;
		return true;
	}
	return false;
}

/* Exercise the production presentation path before reading sample pixels back.
   The P96 readback asks the driver for canonical big-endian RGB565. */
const char *amiga_display_test(void)
{
	static const uint16_t rgb[4] = {0xf800, 0x07e0, 0x001f, 0xffff};
	static const uint8_t pens[4] = {0xe0, 0x1c, 0x03, 0xff};
	for (int x = 0; x < WIDTH; ++x)
	{
		unsigned band = 4 * x / WIDTH;
		if (P96Base)
		{
			uint16_t pixel = rgb[band];
			if (output_format == S32_BGR565_SWAP)
				pixel = ((pixel & 31) << 11) | (pixel & 0x07e0) | (pixel >> 11);
			if (output_format == S32_RGB565_SWAP || output_format == S32_BGR565_SWAP)
				pixel = (pixel >> 8) | (pixel << 8);
			rtg_pixels[x] = pixel;
		}
		else chunky[x] = pens[band];
	}
	const unsigned pitch = P96Base ? WIDTH * sizeof(*rtg_pixels) : WIDTH;
	uint8_t *row = P96Base ? (uint8_t *)rtg_pixels : chunky;
	for (int y = 1; y < S32_VIDEO_HEIGHT; ++y) memcpy(row + y * pitch, row, pitch);
	unsigned presented = 0;
	for (unsigned field = 0; field < 75; ++field)
	{
		WaitTOF();
		if (presented < 2 && amiga_display_frame(NULL)) ++presented;
	}
	if (presented != 2) return "Display opened but two frames could not be presented";
	for (unsigned band = 0; band < 4; ++band)
	{
		unsigned x = (2 * band + 1) * WIDTH / 8;
		if (P96Base)
		{
			uint16_t pixel = 0;
			struct RenderInfo readback = {.Memory = &pixel, .BytesPerRow = 2, .RGBFormat = RGBFB_R5G6B5};
			p96ReadPixelArray(&readback, 0, 0, window->RPort, rtg_left + x, rtg_top + S32_VIDEO_HEIGHT / 2, 1, 1);
			if (pixel != rgb[band]) return "Picasso96 opened but RGB565 colour readback failed";
		}
		else if (ReadPixel(window->RPort, x, HEIGHT / 2) != pens[band])
			return "PAL AGA opened but colour readback failed";
	}
	if (P96Base)
	{
		uint16_t border = 0xffff;
		struct RenderInfo readback = {.Memory = &border, .BytesPerRow = 2, .RGBFormat = RGBFB_R5G6B5};
		p96ReadPixelArray(&readback, 0, 0, window->RPort, window->Width / 2, rtg_top - 1, 1, 1);
		if (border != 0) return "Picasso96 border did not read back as black";
	}
	return NULL;
}

static void wait_for_safe(void)
{
	while (!GetMsg(safe_port))
		WaitPort(safe_port);
}

void amiga_display_close(void)
{
	if (window)
		CloseWindow(window);
	if (blank_pointer)
		FreeMem(blank_pointer, POINTER_BYTES);
	if (waiting_for_safe)
		wait_for_safe();
	if (displayed)
	{
		while (!ChangeScreenBuffer(screen, screen_buffers[0]))
			WaitTOF();
		wait_for_safe();
	}
	for (unsigned i = 0; i < 2; ++i)
		if (screen_buffers[i])
			FreeScreenBuffer(screen, screen_buffers[i]);
	if (screen && P96Base) p96CloseScreen(screen);
	else if (screen) CloseScreen(screen);
	if (safe_port)
		DeleteMsgPort(safe_port);
	for (unsigned i = 0; i < 2; ++i)
		if (planes[i]) FreeMem(planes[i], PLANES * PLANE_BYTES);
	s32_video_set_output(NULL, false, false);
	if (rtg_pixels) FreeMem(rtg_pixels, WIDTH * S32_VIDEO_HEIGHT * 2);
	if (chunky)
		FreeMem(chunky, CHUNKY_BYTES);
	if (P96Base) CloseLibrary(P96Base);
	if (CyberGfxBase) CloseLibrary(CyberGfxBase);
	if (IntuitionBase)
		CloseLibrary((struct Library *)IntuitionBase);
	if (GfxBase)
		CloseLibrary((struct Library *)GfxBase);
}
