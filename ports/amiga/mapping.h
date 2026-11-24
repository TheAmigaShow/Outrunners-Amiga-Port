#include <proto/dos.h>
#include <libraries/lowlevel.h>

#define MAPFILE "PROGDIR:A500Pad.map"
enum { GAS, BRAKE, DOWN_SHIFT, UP_SHIFT, COIN, RADIO, START_PAD, PAD_BUTTONS };
static ULONG binding[PAD_BUTTONS] = { JPF_BUTTON_RED, JPF_BUTTON_BLUE, JPF_BUTTON_REVERSE,
    JPF_BUTTON_FORWARD, JPF_BUTTON_GREEN, JPF_BUTTON_YELLOW, JPF_BUTTON_PLAY };

static int load_mapping(void)
{
    UBYTE ids[PAD_BUTTONS+1];
    BPTR file=Open(MAPFILE,MODE_OLDFILE);
    if (!file) return 0;
    LONG n=Read(file,ids,sizeof(ids));
    Close(file);
    if (n!=PAD_BUTTONS) return 0;
    ULONG used=0;
    for (int i=0;i<PAD_BUTTONS;i++) {
        if (ids[i]>=32) return 0;
        ULONG bit=1UL<<ids[i];
        if (!(bit & JP_BUTTON_MASK) || (used & bit)) return 0;
        used |= bit;
    }
    for(int i=0;i<PAD_BUTTONS;i++) binding[i]=1UL<<ids[i];
    return 1;
}
