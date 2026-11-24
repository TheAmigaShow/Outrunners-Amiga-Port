#include "backend.h"

static struct IntuitionBase *loading_intuition;
static struct GfxBase *loading_graphics;
#define INTUITION_BASE_NAME loading_intuition
#define GRAPHICS_BASE_NAME loading_graphics
#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <intuition/intuitionbase.h>
#include <graphics/gfxbase.h>
#include <string.h>

static struct Window *window;
static int next_y;

bool amiga_loading_open(void)
{
	loading_intuition = (void *)OpenLibrary("intuition.library", 37);
	loading_graphics = (void *)OpenLibrary("graphics.library", 37);
	if (loading_intuition && loading_graphics)
		window = OpenWindowTags(NULL, WA_PubScreenName, (ULONG)"Workbench",
			WA_PubScreenFallBack, TRUE, WA_Title, (ULONG)"OutRunners - Loading",
			WA_Left, 10, WA_Top, 18, WA_Width, 470, WA_Height, 120, WA_AutoAdjust, TRUE,
			WA_DragBar, TRUE, WA_DepthGadget, TRUE, WA_Activate, TRUE,
			WA_SmartRefresh, TRUE, WA_NoCareRefresh, TRUE, WA_IDCMP, IDCMP_VANILLAKEY, TAG_DONE);
	if (!window)
	{
		amiga_loading_close();
		return false;
	}
	struct RastPort *rp = window->RPort;
	struct DrawInfo *info = GetScreenDrawInfo(window->WScreen);
	const unsigned bg = info ? info->dri_Pens[BACKGROUNDPEN] : 0;
	const unsigned fg = info ? info->dri_Pens[TEXTPEN] : 1;
	if (info) FreeScreenDrawInfo(window->WScreen, info);
	SetBPen(rp, bg);
	SetAPen(rp, bg);
	RectFill(rp, window->BorderLeft, window->BorderTop,
		window->Width - window->BorderRight - 1, window->Height - window->BorderBottom - 1);
	SetAPen(rp, fg);
	SetDrMd(rp, JAM2);
	next_y = window->BorderTop + 2;
	return true;
}

void amiga_loading_line(const char *text)
{
	if (!window) return;
	struct RastPort *rp = window->RPort;
	const int x = window->BorderLeft + 3, top = window->BorderTop + 2;
	const int right = window->Width - window->BorderRight - 3;
	const int bottom = window->Height - window->BorderBottom - 2;
	if (right < x || rp->TxHeight == 0 || bottom - top + 1 < rp->TxHeight) return;
	size_t len = strlen(text);
	while (len)
	{
		struct TextExtent extent;
		const unsigned count = TextFit(rp, text, len, &extent, NULL, 1, right - x + 1, rp->TxHeight);
		if (!count) break;
		if (next_y + rp->TxHeight - 1 > bottom)
		{
			ScrollRaster(rp, 0, rp->TxHeight, x, top, right, bottom);
			next_y -= rp->TxHeight;
		}
		Move(rp, x, next_y + rp->TxBaseline);
		Text(rp, text, count);
		next_y += rp->TxHeight;
		text += count;
		len -= count;
	}
}

void amiga_loading_wait_return(void)
{
	for (;;)
	{
		WaitPort(window->UserPort);
		struct IntuiMessage *msg;
		while ((msg = (struct IntuiMessage *)GetMsg(window->UserPort)))
		{
			const bool enter = msg->Class == IDCMP_VANILLAKEY && msg->Code == '\r';
			ReplyMsg((struct Message *)msg);
			if (enter) return;
		}
	}
}

void amiga_loading_close(void)
{
	if (window) CloseWindow(window);
	if (loading_graphics) CloseLibrary((struct Library *)loading_graphics);
	if (loading_intuition) CloseLibrary((struct Library *)loading_intuition);
	window = NULL;
	loading_graphics = NULL;
	loading_intuition = NULL;
}
