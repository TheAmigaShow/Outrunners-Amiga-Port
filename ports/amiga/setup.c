#include <proto/exec.h>
#include <proto/lowlevel.h>
#include "mapping.h"

struct Library *LowLevelBase;

static int learn(void)
{
    const char *prompts[]={"A (red) for throttle","B (blue) for brake","L for downshift",
        "R for upshift","X (green) for coin","Y (yellow) for radio","your centre Start button (Menu or Home)"};
    UBYTE ids[PAD_BUTTONS];
    ULONG used=0;
    for(int i=0;i<PAD_BUTTONS;i++) {
        PutStr("Press and release "); PutStr((STRPTR)prompts[i]); PutStr("\n"); Flush(Output());
        int released=0;
        ULONG selected=0;
        for(int tick=0;tick<1500;tick++) {
            if (SetSignal(0,0)&SIGBREAKF_CTRL_C) return 0;
            ULONG raw=ReadJoyPort(1);
            if ((raw & JP_TYPE_MASK)==JP_TYPE_GAMECTLR) {
                ULONG bits=raw & JP_BUTTON_MASK;
                if (!bits && selected) break;
                if (!bits) released=1;
                else if (released && !selected) {
                    if ((bits & (bits-1)) || (used & bits)) {
                        PutStr("Release, then press one unused button.\n");
                        released=0;
                    } else selected=bits;
                }
            }
            Delay(1);
            if (tick==1499) return 0;
        }
        used |= selected;
        ids[i]=0;
        while ((1UL<<ids[i])!=selected) ids[i]++;
    }
    BPTR file=Open(MAPFILE,MODE_NEWFILE);
    if (!file) return 0;
    int ok=Write(file,ids,sizeof ids)==sizeof ids;
    return Close(file) && ok && load_mapping();
}

int main(void)
{
    if (load_mapping()) return 0;
    LowLevelBase=OpenLibrary("lowlevel.library",40);
    int ok=0;
    if (LowLevelBase) {
        PutStr("THEA500 pad setup. Release all buttons.\n");
        ok=learn();
        CloseLibrary(LowLevelBase);
    }
    if (!ok) PutStr("Pad setup incomplete. Check Port 2 and launch again.\n");
    return ok ? 0 : 10;
}
