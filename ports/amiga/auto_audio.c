#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <proto/dos.h>

#define DIRECTORY "RAM:OutRunners-AHI-session"
#define ORIGINAL DIRECTORY "/Original.prefs"
#define ACTIVE "ENV:Sys/ahi.prefs"

static uint32_t be32(const unsigned char *p)
{
    return ((uint32_t)p[0]<<24) | ((uint32_t)p[1]<<16) |
           ((uint32_t)p[2]<<8) | p[3];
}

/* Return the unique Unit 0 payload only after validating every chunk. */
static unsigned char *unit0(unsigned char *p, size_t n)
{
    unsigned char *found = NULL;
    if (n < 12 || memcmp(p,"FORM",4) || be32(p+4) != n-8 || memcmp(p+8,"PREF",4))
        return NULL;
    for (size_t i=12; i<n;) {
        if (n-i<8) return NULL;
        uint32_t len=be32(p+i+4);
        unsigned char *chunk=p+i;
        i+=8;
        if (len>n-i || (len&1)>n-i-len) return NULL;
        if (!memcmp(chunk,"AHIU",4)) {
            if (!len) return NULL;
            if (!p[i]) {
                if (found || len<12) return NULL;
                found=p+i;
            }
        }
        i+=len+(len&1);
    }
    return found;
}

static unsigned char *read_active(size_t *n)
{
    BPTR f=Open((STRPTR)ACTIVE,MODE_OLDFILE);
    if (!f) return NULL;
    LONG size=-1;
    if (Seek(f,0,OFFSET_END)>=0) size=Seek(f,0,OFFSET_BEGINNING);
    unsigned char *p=NULL;
    if (size>=12 && size<=1048576) {
        p=malloc(size);
        if (p && Read(f,p,size)!=size) { free(p); p=NULL; }
        if (p) *n=size;
    }
    Close(f);
    return p;
}

static int write_file(const char *path, const unsigned char *p, size_t n)
{
    BPTR f=Open((STRPTR)path,MODE_NEWFILE);
    if (!f) return 0;
    int ok=Write(f,(APTR)p,n)==(LONG)n;
    return Close(f) && ok;
}

/* Check the current mode and settings before updating the Unit 0 record. */
static int change(const unsigned char *expected,const unsigned char *target)
{
    BPTR f=Open((STRPTR)ACTIVE,MODE_OLDFILE);
    if (!f) return 0;
    Close(f);
    f=Open((STRPTR)ACTIVE,MODE_READWRITE);
    if (!f) return 0;
    LONG n=-1;
    if (Seek(f,0,OFFSET_END)>=0) n=Seek(f,0,OFFSET_BEGINNING);
    unsigned char *p=(n>=12 && n<=1048576) ? malloc(n) : NULL;
    unsigned char *u=(p && Read(f,p,n)==n) ? unit0(p,n) : NULL;
    int ok=0;
    if (u && !memcmp(u+2,expected,10) && Seek(f,u+2-p,OFFSET_BEGINNING)>=0) {
        ok=Write(f,(APTR)target,10)==10;
        if (!ok && Seek(f,u+2-p,OFFSET_BEGINNING)>=0)
            Write(f,(APTR)expected,10);
    }
    free(p);
    return Close(f) && ok;
}

static int run(const char *command)
{
    Flush(Output());
    return SystemTagList((STRPTR)command,NULL);
}

int main(int argc,char **argv)
{
    const char *game=(argc==2 && !strcmp(argv[1],"AGA")) ?
        "Execute \"Run Pad4 AGA\"" : "Execute \"Run Pad4 RTG\"";
    size_t n=0;
    PutStr("Reading AHI prefs...\n");
    unsigned char *p=read_active(&n);
    unsigned char *u=p ? unit0(p,n) : NULL;
    unsigned char previous[10], game_settings[10];
    int owned=0, attempted=0, backup_ready=0;
    if (!u) {
        PutStr("AHI Unit 0 preferences unavailable. Using existing audio setup.\n");
    } else if (u[2]==0 && u[3]==1 && be32(u+8)==22050) {
        PutStr("AHI Unit 0 already set to 22050 Hz and one mixing channel.\n");
    } else {
        BPTR lock=CreateDir((STRPTR)DIRECTORY);
        if (!lock) {
            PutStr("AHI session directory unavailable. Using current settings.\n");
        } else {
            UnLock(lock);
            owned=1;
            memcpy(previous,u+2,sizeof previous);
            if (write_file(ORIGINAL,p,n)) {
                backup_ready=1;
                u[2]=0; u[3]=1;
                u[8]=0; u[9]=0; u[10]=0x56; u[11]=0x22;
                memcpy(game_settings,u+2,10);
                attempted=change(previous,game_settings);
                if (attempted)
                    PutStr("AHI Unit 0 set to 22050 Hz and one mixing channel.\n");

            }
            if (!attempted) PutStr("Could not prepare temporary AHI preferences.\n");
        }
    }
    free(p);
    int rc=run(game);
    if (attempted) {
        if (!change(game_settings,previous)) {
            PutStr("AHI restore unconfirmed. Original settings saved in " ORIGINAL "\n");
            return 10;
        }
    }
    if (backup_ready && !attempted) {
        size_t size=0;
        unsigned char *current=read_active(&size);
        unsigned char *record=current ? unit0(current,size) : NULL;
        int intact=record && !memcmp(record+2,previous,10);
        free(current);
        if (!intact) {
            PutStr("Original settings saved in " ORIGINAL "\n");
            return 10;
        }
    }
    if (owned) {
        DeleteFile((STRPTR)ORIGINAL);
        DeleteFile((STRPTR)DIRECTORY);
    }
    return rc<0 ? 20 : rc;
}
